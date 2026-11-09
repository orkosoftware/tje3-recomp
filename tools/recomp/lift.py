"""Static recompiler: lift x86 functions of default.xbe to C++ (runtime/cpu.h).

Usage:
    python -I lift.py <default.xbe> <out_dir> <addr|@file> [...] [--extern addr,...]

Each function becomes `void f_XXXXXXXX(Cpu* c)`. Blocks are found by recursive
descent from the entry; a jump to another function's entry is a tail call.
Calls push the real return address, so the guest stack is byte-identical to
the original. Addresses given with --extern are not lifted: calls to them go
to `hle_XXXXXXXX(Cpu*)`, which the platform layer (or a test harness) provides.

Flags are function-local lazy state (fop/fw/fa/fb/fr/fcf/fof); conditions are
computed from it on use, so the C++ compiler removes what is dead.
"""
import os
import struct
import sys

import capstone
from capstone import x86_const as X

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "emu"))
from xbe_image import XbeImage  # noqa: E402

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp",
         "si": "esi", "di": "edi"}
REG8LO = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8HI = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

JCC = {
    "je": "ZF", "jz": "ZF", "jne": "!ZF", "jnz": "!ZF",
    "jb": "CF", "jc": "CF", "jnae": "CF", "jae": "!CF", "jnb": "!CF", "jnc": "!CF",
    "jbe": "(CF || ZF)", "jna": "(CF || ZF)", "ja": "(!CF && !ZF)", "jnbe": "(!CF && !ZF)",
    "jl": "(SF != OF)", "jnge": "(SF != OF)", "jge": "(SF == OF)", "jnl": "(SF == OF)",
    "jle": "(ZF || SF != OF)", "jng": "(ZF || SF != OF)", "jg": "(!ZF && SF == OF)",
    "jnle": "(!ZF && SF == OF)",
    "js": "SF", "jns": "!SF", "jp": "PF", "jpe": "PF", "jnp": "!PF", "jpo": "!PF",
    "jo": "OF", "jno": "!OF",
}
FLAGS = {
    "CF": "flag_cf(fop, fw, fa, fb, fr, fcf)", "ZF": "flag_zf(fw, fr)", "SF": "flag_sf(fw, fr)",
    "OF": "flag_of(fop, fw, fa, fb, fr, fof)", "PF": "flag_pf(fr)",
}


def cond(mnemonic):
    expr = JCC[mnemonic]
    for k, v in FLAGS.items():
        expr = expr.replace(k, v)
    return expr


class LiftError(Exception):
    pass


class Lifter:
    def __init__(self, img, func_starts, externs, wraps=()):
        self.wraps = set(wraps)      # lifted as usual, but direct calls go to wrap_XXXXXXXX
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.func_starts = func_starts
        self.externs = set(externs)
        self.callees = set()
        self.code_refs = set()
        self._sorted_starts = None
        self.code_ranges = [(va, va + vsize) for name, va, vsize, raw, rsize in img.sections
                            if name in (".text", "D3D", "DSOUND", "XGRPH", "XPP") or name.startswith("PSGSFD")]

    def is_code(self, v):
        return any(lo <= v < hi for lo, hi in self.code_ranges)

    def data_code_pointers(self):
        """Dwords in the data sections that point into code (vtables, function tables)."""
        out = set()
        for name, va, vsize, raw, rsize in self.img.sections:
            if name not in (".rdata", ".data", "DOLBY"):
                continue
            data = self.img.read(va, min(vsize, rsize))
            for o in range(0, len(data) - 3, 4):
                v = struct.unpack_from("<I", data, o)[0]
                if self.is_code(v):
                    out.add(v)
        return out

    # --- decoding ------------------------------------------------------------------
    def decode(self, addr):
        try:
            code = self.img.read(addr, 16)
        except ValueError:
            raise LiftError(f"code address {addr:#x} outside the image")
        for ins in self.md.disasm(code, addr):
            return ins
        raise LiftError(f"cannot decode at {addr:#x}")

    def jump_table(self, ins, entry):
        """Targets of `jmp [reg*4 + table]`. The bound comes from the cmp/ja before
        the jump (through a byte index table if there is one); without one, entries
        are read while they point into this function."""
        mem = ins.operands[0].mem
        table = mem.disp & 0xffffffff
        idx_reg = ins.reg_name(mem.index)
        bound, byte_table = None, None
        prev, steps = self.ends.get(ins.address), 0
        while prev is not None and steps < 12:
            if prev.mnemonic in ("movzx", "mov") and len(prev.operands) == 2                     and prev.operands[1].type == X.X86_OP_MEM and prev.operands[1].size == 1:
                dst = prev.reg_name(prev.operands[0].reg)
                if REG8LO.get(dst, dst) == idx_reg or dst == idx_reg:
                    byte_table = prev.operands[1].mem.disp & 0xffffffff
            if prev.mnemonic in ("cmp", "and") and prev.operands[1].type == X.X86_OP_IMM \
                    and prev.operands[0].type == X.X86_OP_REG:
                reg = prev.reg_name(prev.operands[0].reg)
                if REG16.get(reg, REG8LO.get(reg, reg)) == idx_reg or (byte_table is None and reg == idx_reg):
                    bound = prev.operands[1].imm & 0xffffffff
                    break
                if byte_table is not None:          # the bound applies to the byte index register
                    bound = prev.operands[1].imm & 0xffffffff
                    break
            if prev.mnemonic in ("mov", "lea", "add", "sub", "neg", "xor", "shr", "shl") \
                    and prev.operands and prev.operands[0].type == X.X86_OP_REG \
                    and REG8LO.get(prev.reg_name(prev.operands[0].reg), prev.reg_name(prev.operands[0].reg)) == idx_reg \
                    and byte_table is None:
                break                               # index computed some other way: no usable bound
            prev, steps = self.ends.get(prev.address), steps + 1
        if bound is not None and bound <= 1024:
            n = (max(self.img.read(byte_table, bound + 1)) + 1) if byte_table is not None else bound + 1
            return sorted(set(struct.unpack(f"<{n}I", self.img.read(table, 4 * n))))
        lo, hi = entry, self.next_start(entry)
        targets = []
        for k in range(256):
            t = struct.unpack("<I", self.img.read(table + 4 * k, 4))[0]
            if not (lo <= t < hi) or table <= t < table + 4 * (k + 1):
                break
            targets.append(t)
        if not targets:
            raise LiftError(f"unbounded jump table at {ins.address:#x}")
        return sorted(set(targets))

    def next_start(self, entry):
        import bisect
        if self._sorted_starts is None:
            self._sorted_starts = sorted(self.func_starts)
        i = bisect.bisect_right(self._sorted_starts, entry)
        return self._sorted_starts[i] if i < len(self._sorted_starts) else entry + 0x10000

    def discover(self, entry):
        """Recursive descent. Returns {block_addr: [ins...]}, jump tables, tail calls."""
        blocks, tables, todo, seen = {}, {}, [entry], set()
        starts = set()
        self.fall_from = {}          # block start -> instructions of the block that falls into it
        self.extern_fall = {}        # block start -> HLE function its last instruction falls into
        self.ends = {}               # address -> instruction ending there (linear predecessor)
        pending = []                 # table jumps, resolved once the rest is decoded
        while todo or pending:
            if not todo:
                for jins in pending:
                    tables[jins.address] = self.jump_table(jins, entry)
                    todo.extend(tables[jins.address])
                pending = []
                continue
            a = todo.pop()
            if a in seen:
                continue
            seen.add(a)
            starts.add(a)
            cur, block, fall = a, [], None
            while True:
                ins = self.decode(cur)
                block.append(ins)
                self.ends[ins.address + ins.size] = ins
                m = ins.mnemonic
                nxt = cur + ins.size
                if m == "jmp":
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t != entry and t in self.func_starts:
                            self.callees.add(t)                   # tail call
                        else:
                            todo.append(t)
                    elif op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index != 0:
                        pending.append(ins)
                    break
                if m.startswith("j") or m in ("loop", "loope", "loopne", "jecxz"):
                    t = ins.operands[0].imm & 0xffffffff
                    todo.append(t)
                    todo.append(nxt)
                    self.fall_from.setdefault(nxt, list(block))
                    fall = nxt          # not-taken path continues at the next instruction
                    break
                if m in ("ret", "retn", "int3", "ud2", "hlt"):
                    break
                if m == "call" and ins.operands[0].type == X.X86_OP_IMM:
                    self.callees.add(ins.operands[0].imm & 0xffffffff)
                elif m in ("push", "mov") and ins.operands[-1].type == X.X86_OP_IMM:
                    v = ins.operands[-1].imm & 0xffffffff
                    if self.is_code(v):
                        self.code_refs.add(v)       # callback / thread routine passed by address
                cur = nxt
                if cur in self.externs and cur != entry:
                    # falls into an HLE-replaced function: call it
                    block.append(None)
                    self.callees.add(cur)
                    blocks[a] = (block, None)
                    self.extern_fall[a] = cur
                    block = None
                    break
                if cur in starts or (cur in self.func_starts and cur != entry):
                    # Fall into a block we already have, or into code that also starts
                    # another function: keep lifting it here (the flags, which are
                    # function-local, may still be live: some "function starts" in
                    # the map split a test from its branch).
                    block.append(None)
                    todo.append(cur)
                    blocks[a] = (block, cur)
                    block = None
                    break
            if block is not None:
                blocks[a] = (block, fall)
        # A jump into the middle of a block gets its own (overlapping) block; both
        # copies end at the same branch, so the duplication is harmless.
        return blocks, tables, starts

    # --- operands ------------------------------------------------------------------
    def addr(self, ins, op):
        mem = op.mem
        parts = []
        if mem.segment == X.X86_REG_FS:
            parts.append("c->fs_base")          # thread information block
        elif mem.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES):
            raise LiftError(f"segment override at {ins.address:#x}")
        if mem.base:
            parts.append(self.reg_read(ins.reg_name(mem.base)))
        if mem.index:
            r = self.reg_read(ins.reg_name(mem.index))
            parts.append(f"{r} * {mem.scale}u" if mem.scale != 1 else r)
        if mem.disp or not parts:
            parts.append(f"0x{mem.disp & 0xffffffff:x}u")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    @staticmethod
    def reg_read(name):
        if name in REG32:
            return f"c->{name}"
        if name in REG16:
            return f"(c->{REG16[name]} & 0xffffu)"
        if name in REG8LO:
            return f"(c->{REG8LO[name]} & 0xffu)"
        if name in REG8HI:
            return f"((c->{REG8HI[name]} >> 8) & 0xffu)"
        raise LiftError(f"register {name}")

    @staticmethod
    def reg_write(name, expr):
        if name in REG32:
            return f"c->{name} = (uint32_t)({expr});"
        if name in REG16:
            return f"c->{REG16[name]} = set16(c->{REG16[name]}, {expr});"
        if name in REG8LO:
            return f"c->{REG8LO[name]} = set8lo(c->{REG8LO[name]}, {expr});"
        if name in REG8HI:
            return f"c->{REG8HI[name]} = set8hi(c->{REG8HI[name]}, {expr});"
        raise LiftError(f"register {name}")

    def rd(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & ((1 << (8 * size)) - 1):x}u"
        a = self.addr(ins, op)
        return {1: f"R8({a})", 2: f"R16({a})", 4: f"R32({a})"}[size]

    def wr(self, ins, op, expr):
        if op.type == X.X86_OP_REG:
            return self.reg_write(ins.reg_name(op.reg), expr)
        a = self.addr(ins, op)
        return {1: f"W8({a}, (uint8_t)({expr}));", 2: f"W16({a}, (uint16_t)({expr}));",
                4: f"W32({a}, (uint32_t)({expr}));"}[op.size]

    # --- instruction semantics ---------------------------------------------------------
    def setf(self, op, w, a, b, r):
        return f"fop = {op}; fw = {w}; fa = {a}; fb = {b}; fr = {r};"

    def lift_ins(self, ins, fn_entry):
        m, ops = ins.mnemonic, ins.operands
        nxt = (ins.address + ins.size) & 0xffffffff
        if ins.prefix[0] in (0xf3, 0xf2) and m.startswith("rep"):
            pass
        L = []
        if m == "nop" or m.startswith("prefetch") or m in ("wait", "fwait", "fnclex", "emms_"):
            return ["/* nop */"]
        if m == "mov":
            if ops[0].size != ops[1].size and ops[1].type != X.X86_OP_IMM:
                raise LiftError(f"mov size mismatch at {ins.address:#x}")
            return [self.wr(ins, ops[0], self.rd(ins, ops[1], ops[0].size))]
        if m in ("movzx", "movsx"):
            s = ops[1].size
            v = self.rd(ins, ops[1])
            if m == "movsx":
                v = {1: f"(uint32_t)(int32_t)(int8_t){v}", 2: f"(uint32_t)(int32_t)(int16_t){v}"}[s]
            return [self.wr(ins, ops[0], v)]
        if m == "lea":
            return [self.wr(ins, ops[0], self.addr(ins, ops[1]))]
        if m == "xchg":
            a, b = self.rd(ins, ops[0]), self.rd(ins, ops[1])
            return [f"{{ uint32_t t0 = {a}, t1 = {b}; {self.wr(ins, ops[0], 't1')} {self.wr(ins, ops[1], 't0')} }}"]
        if m == "push":
            size = ops[0].size if ops[0].type != X.X86_OP_IMM else 4
            v = self.rd(ins, ops[0], size)
            if size == 2:
                return [f"{{ uint32_t v = {v}; c->esp -= 2; W16(c->esp, (uint16_t)v); }}"]
            return [f"{{ uint32_t v = {v}; c->esp -= 4; W32(c->esp, v); }}"]
        if m == "pop":
            if ops[0].size == 2:
                return [f"{{ uint32_t v = R16(c->esp); c->esp += 2; {self.wr(ins, ops[0], 'v')} }}"]
            return [f"{{ uint32_t v = R32(c->esp); c->esp += 4; {self.wr(ins, ops[0], 'v')} }}"]
        if m == "pushfd":
            return ["{ uint32_t v = 0x202u | (flag_cf(fop, fw, fa, fb, fr, fcf) ? 1u : 0u) | (flag_zf(fw, fr) ? 0x40u : 0u)"
                    " | (flag_sf(fw, fr) ? 0x80u : 0u) | (flag_of(fop, fw, fa, fb, fr, fof) ? 0x800u : 0u)"
                    " | (c->df ? 0x400u : 0u); c->esp -= 4; W32(c->esp, v); }"]
        if m == "leave":
            return ["c->esp = c->ebp; c->ebp = R32(c->esp); c->esp += 4;"]
        if m == "cdq":
            return ["c->edx = (uint32_t)((int32_t)c->eax >> 31);"]
        if m == "cwde":
            return ["c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"]
        if m in ("cld", "std"):
            return [f"c->df = {1 if m == 'std' else 0};"]
        if m in ("clc", "stc"):
            return [f"fop = F_EAGER; fcf = {1 if m == 'stc' else 0};"]
        if m in ("add", "sub", "cmp", "and", "or", "xor", "test", "adc", "sbb"):
            return self.alu(ins)
        if m in ("inc", "dec", "neg", "not"):
            return self.unary(ins)
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcr", "rcl"):
            return self.shift(ins)
        if m in ("shld", "shrd"):
            return self.shiftd(ins)
        if m in ("imul", "mul", "div", "idiv"):
            return self.muldiv(ins)
        if m.startswith("set"):
            return [self.wr(ins, ops[0], f"({cond('j' + m[3:])} ? 1u : 0u)")]
        if m == "bsf":
            v = self.rd(ins, ops[1])
            return [f"{{ uint32_t v = {v}; fop = F_LOGIC; fw = 32; fr = v; if (v) {{ uint32_t i = 0; while (!((v >> i) & 1)) i++; "
                    f"{self.wr(ins, ops[0], 'i')} }} }}"]
        if m in ("rdtsc",):
            return ["{ uint64_t t = guest_rdtsc(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }"]
        if m in ("cli", "sti"):
            return ["/* interrupt flag: ignored */"]
        if m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "cmpsb", "scasb") \
                or (m.startswith("rep") and len(m.split()) == 1 and ins.op_str == ""):
            return self.string_op(ins)
        if m.startswith("rep") or m.startswith("repe") or m.startswith("repne"):
            return self.string_op(ins)
        if m[0] == "f":
            return self.x87(ins)
        if m in SSE_OPS:
            return self.sse(ins)
        raise LiftError(f"unsupported {m} {ins.op_str} at {ins.address:#x}")

    def alu(self, ins):
        m, ops = ins.mnemonic, ins.operands
        w = 8 * ops[0].size
        mask = f"0x{(1 << w) - 1:x}u"
        a = self.rd(ins, ops[0])
        b = self.rd(ins, ops[1], ops[0].size)
        if ops[1].type == X.X86_OP_IMM:
            b = f"0x{ops[1].imm & ((1 << w) - 1):x}u"
        L = [f"{{ uint32_t a = {a}, b = {b};"]
        if m in ("add", "sub", "cmp"):
            op = "+" if m == "add" else "-"
            L.append(f"uint32_t r = (a {op} b) & {mask};")
            L.append(self.setf("F_ADD" if m == "add" else "F_SUB", w, "a", "b", "r"))
        elif m in ("adc", "sbb"):
            L.append(f"uint32_t ci = flag_cf(fop, fw, fa, fb, fr, fcf) ? 1u : 0u;")
            if m == "adc":
                L.append(f"uint64_t wide = (uint64_t)a + b + ci; uint32_t r = (uint32_t)wide & {mask};")
                L.append(f"fcf = (wide >> {w}) & 1; fof = sbit((a ^ r) & (b ^ r), {w});")
            else:
                L.append(f"uint32_t r = (a - b - ci) & {mask};")
                L.append(f"fcf = ((uint64_t)a < (uint64_t)b + ci) ? 1u : 0u; fof = sbit((a ^ b) & (a ^ r), {w});")
            L.append(f"fop = F_EAGER; fw = {w}; fr = r;")
        else:
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            L.append(f"uint32_t r = (a {op} b) & {mask};")
            L.append(self.setf("F_LOGIC", w, "0", "0", "r"))
        if m not in ("cmp", "test"):
            L.append(self.wr(ins, ops[0], "r"))
        L.append("}")
        return [" ".join(L)]

    def unary(self, ins):
        m, op = ins.mnemonic, ins.operands[0]
        w = 8 * op.size
        mask = f"0x{(1 << w) - 1:x}u"
        a = self.rd(ins, op)
        if m == "not":
            return [f"{{ uint32_t r = (~{a}) & {mask}; {self.wr(ins, op, 'r')} }}"]
        if m == "neg":
            return [f"{{ uint32_t a = {a}, r = (0u - a) & {mask}; {self.setf('F_NEG', w, 'a', '0', 'r')} {self.wr(ins, op, 'r')} }}"]
        delta = "+ 1u" if m == "inc" else "- 1u"
        kind = "F_INC" if m == "inc" else "F_DEC"
        return [f"{{ uint32_t a = {a}, r = (a {delta}) & {mask}; fcf = flag_cf(fop, fw, fa, fb, fr, fcf) ? 1u : 0u; "
                f"{self.setf(kind, w, 'a', '1', 'r')} {self.wr(ins, op, 'r')} }}"]

    def shift(self, ins):
        m, ops = ins.mnemonic, ins.operands
        w = 8 * ops[0].size
        mask = f"0x{(1 << w) - 1:x}u"
        a = self.rd(ins, ops[0])
        cnt = "1u" if len(ops) == 1 else (f"(0x{ops[1].imm & 0xff:x}u & 31u)" if ops[1].type == X.X86_OP_IMM
                                          else f"({self.rd(ins, ops[1])} & 31u)")
        L = [f"{{ uint32_t a = {a}, n = {cnt}; if (n) {{ uint32_t r, cf, of;"]
        if m in ("shl", "sal"):
            L.append(f"r = (uint32_t)(((uint64_t)a << n) & {mask}); cf = (uint32_t)(((uint64_t)a << n) >> {w}) & 1;"
                     f" of = sbit(r, {w}) ^ cf;")
        elif m == "shr":
            L.append(f"r = (a >> n) & {mask}; cf = (a >> (n - 1)) & 1; of = sbit(a, {w});")
        elif m == "sar":
            sx = {8: "(int32_t)(int8_t)a", 16: "(int32_t)(int16_t)a", 32: "(int32_t)a"}[w]
            L.append(f"int32_t s = {sx}; r = (uint32_t)(s >> (n > {w - 1} ? {w - 1} : n)) & {mask}; "
                     f"cf = (uint32_t)(s >> (n - 1 > {w - 1} ? {w - 1} : n - 1)) & 1; of = 0;")
        elif m in ("rol", "ror"):
            k = f"(n % {w})"
            if m == "rol":
                L.append(f"uint32_t k = {k}; r = k ? ((a << k) | (a >> ({w} - k))) & {mask} : a; cf = r & 1; "
                         f"of = sbit(r, {w}) ^ cf;")
            else:
                L.append(f"uint32_t k = {k}; r = k ? ((a >> k) | (a << ({w} - k))) & {mask} : a; cf = sbit(r, {w}); "
                         f"of = sbit(r, {w}) ^ ((r >> {w - 2}) & 1);")
        elif m in ("rcr", "rcl"):
            L.append(f"uint64_t v = ((uint64_t)(flag_cf(fop, fw, fa, fb, fr, fcf) ? 1u : 0u) << {w}) | a;"
                     f" uint32_t k = n % {w + 1};")
            if m == "rcr":
                L.append(f"v = k ? ((v >> k) | (v << ({w + 1} - k))) : v;")
            else:
                L.append(f"v = k ? ((v << k) | (v >> ({w + 1} - k))) : v;")
            L.append(f"r = (uint32_t)v & {mask}; cf = (uint32_t)(v >> {w}) & 1; of = sbit(r, {w}) ^ cf;")
        L.append(f"fop = F_EAGER; fw = {w}; fr = r; fcf = cf; fof = of; {self.wr(ins, ops[0], 'r')} }} }}")
        return [" ".join(L)]

    def shiftd(self, ins):
        m, ops = ins.mnemonic, ins.operands
        a, b = self.rd(ins, ops[0]), self.rd(ins, ops[1])
        cnt = f"(0x{ops[2].imm:x}u & 31u)" if ops[2].type == X.X86_OP_IMM else f"({self.rd(ins, ops[2])} & 31u)"
        if m == "shld":
            body = "r = (a << n) | (b >> (32 - n)); cf = (a >> (32 - n)) & 1;"
        else:
            body = "r = (a >> n) | (b << (32 - n)); cf = (a >> (n - 1)) & 1;"
        return [f"{{ uint32_t a = {a}, b = {b}, n = {cnt}; if (n) {{ uint32_t r, cf; {body} "
                f"fop = F_EAGER; fw = 32; fr = r; fcf = cf; fof = 0; {self.wr(ins, ops[0], 'r')} }} }}"]

    def muldiv(self, ins):
        m, ops = ins.mnemonic, ins.operands
        if m == "imul" and len(ops) >= 2:
            w = 8 * ops[0].size
            a = self.rd(ins, ops[1] if len(ops) == 3 else ops[0])
            b = self.rd(ins, ops[2] if len(ops) == 3 else ops[1], ops[0].size)
            sx = {16: "(int64_t)(int16_t)", 32: "(int64_t)(int32_t)"}[w]
            lo = {16: "(int64_t)(int16_t)r", 32: "(int64_t)(int32_t)r"}[w]
            return [f"{{ int64_t p = {sx}{a} * {sx}{b}; uint32_t r = (uint32_t)p & 0x{(1 << w) - 1:x}u;"
                    f" fop = F_EAGER; fw = {w}; fr = r; fcf = fof = (p != {lo}) ? 1u : 0u; {self.wr(ins, ops[0], 'r')} }}"]
        op = ops[0]
        s = op.size
        v = self.rd(ins, op)
        if s == 4:
            if m == "mul":
                return [f"{{ uint64_t p = (uint64_t)c->eax * {v}; c->eax = (uint32_t)p; c->edx = (uint32_t)(p >> 32);"
                        f" fop = F_EAGER; fw = 32; fr = c->eax; fcf = fof = c->edx != 0; }}"]
            if m == "imul":
                return [f"{{ int64_t p = (int64_t)(int32_t)c->eax * (int32_t){v}; c->eax = (uint32_t)p; c->edx = (uint32_t)((uint64_t)p >> 32);"
                        f" fop = F_EAGER; fw = 32; fr = c->eax; fcf = fof = (p != (int64_t)(int32_t)p); }}"]
            if m == "div":
                return [f"{{ uint32_t d = {v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                        f" uint64_t n = ((uint64_t)c->edx << 32) | c->eax; c->eax = (uint32_t)(n / d); c->edx = (uint32_t)(n % d); }}"]
            return [f"{{ int32_t d = (int32_t){v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                    f" int64_t n = (int64_t)(((uint64_t)c->edx << 32) | c->eax); int64_t q = n / d;"
                    f" c->eax = (uint32_t)q; c->edx = (uint32_t)(n % d); }}"]
        if s == 2:
            if m in ("div", "idiv"):
                if m == "div":
                    return [f"{{ uint32_t d = {v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                            f" uint32_t n = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff);"
                            f" c->eax = set16(c->eax, n / d); c->edx = set16(c->edx, n % d); }}"]
                return [f"{{ int32_t d = (int16_t){v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                        f" int32_t n = (int32_t)(((c->edx & 0xffff) << 16) | (c->eax & 0xffff));"
                        f" c->eax = set16(c->eax, (uint32_t)(n / d)); c->edx = set16(c->edx, (uint32_t)(n % d)); }}"]
            kind = "uint32_t" if m == "mul" else "int32_t"
            cast = "(uint16_t)" if m == "mul" else "(int16_t)"
            return [f"{{ {kind} p = ({kind}){cast}c->eax * ({kind}){cast}{v}; c->eax = set16(c->eax, (uint32_t)p);"
                    f" c->edx = set16(c->edx, (uint32_t)p >> 16); fop = F_EAGER; fw = 16; fr = (uint32_t)p;"
                    f" fcf = fof = 0; }}"]
        if m == "div":
            return [f"{{ uint32_t d = {v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                    f" uint32_t n = c->eax & 0xffff; c->eax = set16(c->eax, ((n % d) << 8) | ((n / d) & 0xff)); }}"]
        if m == "idiv":
            return [f"{{ int32_t d = (int8_t){v}; if (!d) fault(\"divide by zero\", 0x{ins.address:x}u);"
                    f" int32_t n = (int16_t)c->eax; c->eax = set16(c->eax, (((uint32_t)(n % d) & 0xff) << 8) | ((uint32_t)(n / d) & 0xff)); }}"]
        if m == "mul":
            return [f"{{ uint32_t p = (c->eax & 0xff) * {v}; c->eax = set16(c->eax, p); fop = F_EAGER; fw = 8; fr = p;"
                    f" fcf = fof = (p >> 8) != 0; }}"]
        return [f"{{ int32_t p = (int8_t)c->eax * (int8_t){v}; c->eax = set16(c->eax, (uint32_t)p); fop = F_EAGER; fw = 8;"
                f" fr = (uint32_t)p; fcf = fof = (p != (int8_t)p); }}"]

    def string_op(self, ins):
        m = ins.mnemonic
        rep = None
        parts = m.split()
        if len(parts) == 2:
            rep, m = parts
        elif m.startswith("rep") and not ins.op_str:
            raise LiftError(f"bare {m} at {ins.address:#x}")
        size = {"b": 1, "w": 2, "d": 4}[m[-1]]
        base = m[:-1]
        step = f"(c->df ? -{size} : {size})"
        R = {1: "R8", 2: "R16", 4: "R32"}[size]
        W = {1: "W8", 2: "W16", 4: "W32"}[size]
        acc = {1: "(c->eax & 0xffu)", 2: "(c->eax & 0xffffu)", 4: "c->eax"}[size]
        if base == "movs":
            one = f"{W}(c->edi, {R}(c->esi)); c->esi += {step}; c->edi += {step};"
        elif base == "stos":
            one = f"{W}(c->edi, {acc}); c->edi += {step};"
        elif base == "lods":
            one = (f"c->eax = {'set8lo' if size == 1 else 'set16' if size == 2 else ''}"
                   f"({'c->eax, ' if size < 4 else ''}{R}(c->esi)); c->esi += {step};")
        elif base in ("cmps", "scas"):
            w = 8 * size
            a, b = ((f"{R}(c->esi)", f"{R}(c->edi)") if base == "cmps" else (acc, f"{R}(c->edi)"))
            adv = (f"c->esi += {step}; c->edi += {step};" if base == "cmps" else f"c->edi += {step};")
            one = (f"{{ uint32_t a = {a}, b = {b}, r = (a - b) & 0x{(1 << w) - 1:x}u; {adv} "
                   f"fop = F_SUB; fw = {w}; fa = a; fb = b; fr = r; }}")
            if rep:
                stop = "!flag_zf(fw, fr)" if rep in ("rep", "repe", "repz") else "flag_zf(fw, fr)"
                return [f"while (c->ecx) {{ {one} c->ecx--; if ({stop}) break; }}"]
            return [one]
        else:
            raise LiftError(f"string op {ins.mnemonic} at {ins.address:#x}")
        if rep:
            return [f"while (c->ecx) {{ {one} c->ecx--; }}"]
        return [one]

    # --- x87 ---------------------------------------------------------------------------
    def fmem(self, ins, op):
        a = self.addr(ins, op)
        if op.size == 4:
            return f"(double)RF32({a})"
        if op.size == 8:
            return f"RF64({a})"
        raise LiftError(f"x87 m{op.size * 8} at {ins.address:#x}")

    def fimem(self, ins, op):
        a = self.addr(ins, op)
        return {2: f"(double)(int16_t)R16({a})", 4: f"(double)(int32_t)R32({a})",
                8: f"(double)(int64_t)R64({a})"}[op.size]

    def st(self, ins, op):
        n = ins.reg_name(op.reg)
        if not n.startswith("st"):
            raise LiftError(f"x87 operand {n} at {ins.address:#x}")
        return f"ST(c, {int(n[3]) if '(' in n else 0})"

    def x87(self, ins):
        m, ops = ins.mnemonic, ins.operands
        arith = {"fadd": "{d} + {s}", "fsub": "{d} - {s}", "fsubr": "{s} - {d}", "fmul": "{d} * {s}",
                 "fdiv": "{d} / {s}", "fdivr": "{s} / {d}"}
        base = m.rstrip("p") if m not in ("fcomp", "fucomp") else m
        if m in ("fld", "fild"):
            op = ops[0]
            if op.type == X.X86_OP_REG:
                return [f"{{ double v = {self.st(ins, op)}; fpush(c, v); }}"]
            if m == "fild":
                return [f"fpush(c, {self.fimem(ins, op)});"]
            if op.size == 10:
                return [f"fpush(c, f80_to_double(g_mem + {self.addr(ins, op)}));"]
            return [f"fpush(c, {self.fmem(ins, op)});"]
        if m in ("fldz", "fld1", "fldpi", "fldln2", "fldlg2", "fldl2e", "fldl2t"):
            v = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.141592653589793", "fldln2": "0.6931471805599453",
                 "fldlg2": "0.30102999566398120", "fldl2e": "1.4426950408889634",
                 "fldl2t": "3.3219280948873622"}[m]
            return [f"fpush(c, {v});"]
        if m in ("fst", "fstp"):
            op = ops[0]
            pop = " fpop(c);" if m == "fstp" else ""
            if op.type == X.X86_OP_REG:
                return [f"{self.st(ins, op)} = ST(c, 0);{pop}"]
            a = self.addr(ins, op)
            if op.size == 4:
                return [f"WF32({a}, (float)ST(c, 0));{pop}"]
            if op.size == 8:
                return [f"WF64({a}, ST(c, 0));{pop}"]
            if op.size == 10:
                return [f"double_to_f80(g_mem + {a}, ST(c, 0));{pop}"]
        if m in ("fist", "fistp"):
            op = ops[0]
            a = self.addr(ins, op)
            pop = " fpop(c);" if m == "fistp" else ""
            fn = {2: "W16({a}, (uint16_t)fist16(c, ST(c, 0)));", 4: "W32({a}, (uint32_t)fist32(c, ST(c, 0)));",
                  8: "W64({a}, (uint64_t)fist64(c, ST(c, 0)));"}[op.size].format(a=a)
            return [fn + pop]
        if base in arith and m not in ("fcomp",):
            pop = m.endswith("p")
            expr = arith[base]
            if not ops:
                d, s = "ST(c, 1)", "ST(c, 0)"
                return [f"{d} = {expr.format(d=d, s=s)}; fpop(c);"]
            if len(ops) == 1:
                op = ops[0]
                if op.type == X.X86_OP_MEM:
                    return [f"ST(c, 0) = {expr.format(d='ST(c, 0)', s=self.fmem(ins, op))};"]
                if pop:     # faddp st(i): st(i) = st(i) op st0, then pop
                    d, s = self.st(ins, op), "ST(c, 0)"
                    return [f"{d} = {expr.format(d=d, s=s)}; fpop(c);"]
                d, s = "ST(c, 0)", self.st(ins, op)     # fadd st(i): st0 = st0 op st(i)
                return [f"{d} = {expr.format(d=d, s=s)};"]
            d, s = self.st(ins, ops[0]), self.st(ins, ops[1])
            return [f"{d} = {expr.format(d=d, s=s)};" + (" fpop(c);" if pop else "")]
        if m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            expr = arith["f" + m[2:]]
            return [f"ST(c, 0) = {expr.format(d='ST(c, 0)', s=self.fimem(ins, ops[0]))};"]
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp", "ficom", "ficomp"):
            if m.startswith("fi"):
                other = self.fimem(ins, ops[0])
            elif not ops:
                other = "ST(c, 1)"
            elif ops[0].type == X.X86_OP_MEM:
                other = self.fmem(ins, ops[0])
            else:
                other = self.st(ins, ops[-1])
            pops = 2 if m.endswith("pp") else 1 if m.endswith("p") else 0
            return [f"fcom(c, ST(c, 0), {other});" + " fpop(c);" * pops]
        if m == "ftst":
            return ["fcom(c, ST(c, 0), 0.0);"]
        if m == "fnstsw":
            if ops[0].type == X.X86_OP_REG:
                return ["c->eax = set16(c->eax, fnstsw(c));"]
            return [f"W16({self.addr(ins, ops[0])}, fnstsw(c));"]
        if m == "fnstcw":
            return [f"W16({self.addr(ins, ops[0])}, c->fcw);"]
        if m == "fldcw":
            return [f"c->fcw = R16({self.addr(ins, ops[0])});"]
        if m == "fxch":
            o = self.st(ins, ops[-1]) if ops else "ST(c, 1)"
            return [f"{{ double t = ST(c, 0); ST(c, 0) = {o}; {o} = t; }}"]
        if m == "fchs":
            return ["ST(c, 0) = -ST(c, 0);"]
        if m == "fabs":
            return ["ST(c, 0) = std::fabs(ST(c, 0));"]
        if m == "fsqrt":
            return ["ST(c, 0) = std::sqrt(ST(c, 0));"]
        if m == "fsin":
            return ["ST(c, 0) = std::sin(ST(c, 0)); c->fsw_cc &= ~0x0400;"]
        if m == "fcos":
            return ["ST(c, 0) = std::cos(ST(c, 0)); c->fsw_cc &= ~0x0400;"]
        if m == "fpatan":
            return ["ST(c, 1) = std::atan2(ST(c, 1), ST(c, 0)); fpop(c);"]
        if m == "fyl2x":
            return ["ST(c, 1) = ST(c, 1) * std::log2(ST(c, 0)); fpop(c);"]
        if m == "frndint":
            return ["ST(c, 0) = fround(c, ST(c, 0));"]
        if m == "fnclex":
            return ["/* fnclex */"]
        raise LiftError(f"x87 {m} {ins.op_str} at {ins.address:#x}")

    # --- SSE / MMX ---------------------------------------------------------------------
    def xmm(self, ins, op):
        n = ins.reg_name(op.reg)
        if n.startswith("xmm"):
            return f"c->xmm[{n[3:]}]"
        if n.startswith("mm"):
            return f"c->mm[{n[2:]}]"
        raise LiftError(f"vector reg {n} at {ins.address:#x}")

    def sse(self, ins):
        m, ops = ins.mnemonic, ins.operands
        if m == "emms":
            return ["/* emms */"]
        d = ops[0]
        if m in ("movaps", "movups"):
            if d.type == X.X86_OP_MEM:
                return [f"std::memcpy(g_mem + {self.addr(ins, d)}, {self.xmm(ins, ops[1])}.b, 16);"]
            if ops[1].type == X.X86_OP_MEM:
                return [f"std::memcpy({self.xmm(ins, d)}.b, g_mem + {self.addr(ins, ops[1])}, 16);"]
            return [f"{self.xmm(ins, d)} = {self.xmm(ins, ops[1])};"]
        if m == "movss":
            if d.type == X.X86_OP_MEM:
                return [f"W32({self.addr(ins, d)}, {self.xmm(ins, ops[1])}.u[0]);"]
            if ops[1].type == X.X86_OP_MEM:
                x = self.xmm(ins, d)
                return [f"{x}.u[0] = R32({self.addr(ins, ops[1])}); {x}.u[1] = {x}.u[2] = {x}.u[3] = 0;"]
            return [f"{self.xmm(ins, d)}.u[0] = {self.xmm(ins, ops[1])}.u[0];"]
        if m in ("addps", "subps", "mulps"):
            o = {"addps": "+", "subps": "-", "mulps": "*"}[m]
            src = self.vsrc(ins, ops[1], 16)
            x = self.xmm(ins, d)
            return [f"{{ Cpu::Xmm s = {src}; for (int i = 0; i < 4; i++) {x}.f[i] = {x}.f[i] {o} s.f[i]; }}"]
        if m == "shufps":
            imm = ops[2].imm
            src = self.vsrc(ins, ops[1], 16)
            x = self.xmm(ins, d)
            return [f"{{ Cpu::Xmm a = {x}, s = {src}; {x}.u[0] = a.u[{imm & 3}]; {x}.u[1] = a.u[{(imm >> 2) & 3}];"
                    f" {x}.u[2] = s.u[{(imm >> 4) & 3}]; {x}.u[3] = s.u[{(imm >> 6) & 3}]; }}"]
        if m == "cvttss2si":
            src = (f"RF32({self.addr(ins, ops[1])})" if ops[1].type == X.X86_OP_MEM else f"{self.xmm(ins, ops[1])}.f[0]")
            return [self.wr(ins, d, f"(uint32_t)cvt_trunc32({src})")]
        if m == "cvtps2pi":
            src = self.vsrc(ins, ops[1], 8)
            return [f"{{ Cpu::Xmm s = {src}; {self.xmm(ins, d)}.u[0] = (uint32_t)cvt_round32(s.f[0]);"
                    f" {self.xmm(ins, d)}.u[1] = (uint32_t)cvt_round32(s.f[1]); }}"]
        if m == "movq":
            if d.type == X.X86_OP_MEM:
                return [f"W64({self.addr(ins, d)}, {self.xmm(ins, ops[1])}.q);"]
            if ops[1].type == X.X86_OP_MEM:
                return [f"{self.xmm(ins, d)}.q = R64({self.addr(ins, ops[1])});"]
            return [f"{self.xmm(ins, d)}.q = {self.xmm(ins, ops[1])}.q;"]
        if m == "movd":
            if d.type == X.X86_OP_REG and ins.reg_name(d.reg).startswith("mm"):
                return [f"{self.xmm(ins, d)}.q = {self.rd(ins, ops[1], 4)};"]
            return [self.wr(ins, d, f"{self.xmm(ins, ops[1])}.u[0]")]
        if m == "emms":
            return ["/* emms */"]
        if m == "pxor":
            return [f"{self.xmm(ins, d)}.q ^= {self.vsrc(ins, ops[1], 8)}.q;"]
        if m == "pextrw":
            return [self.wr(ins, d, f"{self.xmm(ins, ops[1])}.uw[{ops[2].imm & 3}]")]
        if m in ("packssdw", "packuswb", "paddw", "psraw", "punpcklbw", "punpckhbw", "pavgb", "pavgw", "pcmpgtd"):
            return [f"mmx_{m}(&{self.xmm(ins, d)}, "
                    + (f"{ops[1].imm}" if ops[1].type == X.X86_OP_IMM else f"{self.vsrc(ins, ops[1], 8)}")
                    + ");"]
        raise LiftError(f"vector {m} {ins.op_str} at {ins.address:#x}")

    def vsrc(self, ins, op, size):
        if op.type == X.X86_OP_MEM:
            return f"load_vec(g_mem + {self.addr(ins, op)}, {size})"
        return self.xmm(ins, op)

    # --- functions ---------------------------------------------------------------------
    def lift_function(self, entry):
        blocks, tables, starts = self.discover(entry)
        labels = set(starts)
        out = [f"void f_{entry:08x}(Cpu* c) {{",
               "    int fop = F_LOGIC, fw = 32; uint32_t fa = 0, fb = 0, fr = 1, fcf = 0, fof = 0;",
               "    (void)fa; (void)fb; (void)fcf; (void)fof;",
               f"    TJE_ENTER(0x{entry:x}u);"]
        self.cur_entry = entry
        order = sorted(blocks)
        if order[0] != entry:
            out.append(f"    goto L_{entry:08x};")
        for k, b in enumerate(order):
            insns, fall = blocks[b]
            following = order[k + 1] if k + 1 < len(order) else None
            out.append(f"L_{b:08x}:")
            for ins in insns:
                if ins is None:
                    break
                out.append(f"    // {ins.address:08x}  {ins.mnemonic} {ins.op_str}")
                out.extend("    " + line for line in self.lift_flow(ins, entry, tables))
            if b in self.extern_fall:
                out.append(f"    {self.call_target(self.extern_fall[b])}(c); return;   // falls into an HLE function")
            if fall is not None and fall != following:
                out.append(f"    goto L_{fall:08x};")
        out.append("}")
        return "\n".join(out), labels

    def call_target(self, t):
        if t in self.externs:
            return f"hle_{t:08x}"
        if t in self.wraps:
            return f"wrap_{t:08x}"
        return f"f_{t:08x}"

    def lift_flow(self, ins, entry, tables):
        m, ops = ins.mnemonic, ins.operands
        nxt = (ins.address + ins.size) & 0xffffffff
        if m == "call":
            push = f"c->esp -= 4; W32(c->esp, 0x{nxt:x}u);"
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                return [f"{push} {self.call_target(t)}(c);"]
            return [f"{{ uint32_t t = {self.rd(ins, op, 4)}; {push} call_indirect(c, t); }}"]
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xffffffff
                if t != entry and t in self.func_starts:
                    return [f"{self.call_target(t)}(c); return;   // tail call"]
                if t <= ins.address:
                    return [f"TJE_YIELD(); goto L_{t:08x};   // loop"]
                return [f"goto L_{t:08x};"]
            if ins.address in tables:
                cases = " ".join(f"case 0x{t:x}u: goto L_{t:08x};" for t in tables[ins.address])
                return [f"switch ({self.rd(ins, op, 4)}) {{ {cases} default: fault(\"jump table\", 0x{ins.address:x}u); }}"]
            return [f"{{ uint32_t t = {self.rd(ins, op, 4)}; call_indirect(c, t); return; }}   // indirect tail jump"]
        if m == "ret":
            n = ops[0].imm if ops else 0
            return [f"TJE_LEAVE(0x{entry:x}u); c->esp += {4 + n}; return;"]
        if m in JCC:
            t = ops[0].imm & 0xffffffff
            if t != entry and t in self.func_starts:
                return [f"if ({cond(m)}) {{ {self.call_target(t)}(c); return; }}"]
            if t <= ins.address:
                return [f"if ({cond(m)}) {{ TJE_YIELD(); goto L_{t:08x}; }}   // loop"]
            return [f"if ({cond(m)}) goto L_{t:08x};"]
        if m == "jecxz":
            return [f"if (c->ecx == 0) goto L_{ops[0].imm & 0xffffffff:08x};"]
        if m in ("loop", "loopne", "loope"):
            t = ops[0].imm & 0xffffffff
            extra = {"loop": "", "loopne": " && !flag_zf(fw, fr)", "loope": " && flag_zf(fw, fr)"}[m]
            return [f"if (--c->ecx != 0{extra}) {{ TJE_YIELD(); goto L_{t:08x}; }}"]
        if m in ("int3", "ud2", "hlt", "int"):
            return [f"fault(\"{m}\", 0x{ins.address:x}u);"]
        return self.lift_ins(ins, entry)


SSE_OPS = {"movaps", "movups", "movss", "addps", "subps", "mulps", "shufps", "cvttss2si", "cvtps2pi",
           "movq", "movd", "emms", "pxor", "packssdw", "packuswb", "paddw", "psraw", "punpcklbw",
           "punpckhbw", "pextrw", "pavgb", "pavgw", "pcmpgtd"}


def load_function_starts(root):
    starts = set()
    for line in open(os.path.join(root, "analysis", "function_map.tsv"), encoding="utf-8").read().splitlines()[1:]:
        starts.add(int(line.split("\t", 1)[0], 16))
    cg = os.path.join(root, "analysis", "decomp", "levelgen_callgraph.tsv")
    if os.path.exists(cg):
        for line in open(cg).read().splitlines():
            parts = line.split("\t")
            starts.add(int(parts[0], 16))
            starts.update(int(x, 16) for x in parts[2].split())
    return starts


def parse_addrs(args):
    out = []
    for a in args:
        if a.startswith("@"):
            out += [int(x.split()[0], 16) for x in open(a[1:]).read().split("\n") if x.strip()]
        else:
            out.append(int(a, 16))
    return out


def main():
    xbe, out_dir, *rest = sys.argv[1:]
    externs = []
    if "--extern" in rest:
        i = rest.index("--extern")
        externs = parse_addrs(rest[i + 1].split(","))
        rest = rest[:i] + rest[i + 2:]
    wraps = []
    if "--wrap" in rest:
        i = rest.index("--wrap")
        wraps = parse_addrs(a for a in rest[i + 1].split(",") if a)
        rest = rest[:i] + rest[i + 2:]
    root = os.path.join(HERE, "..", "..")
    lifter = Lifter(XbeImage(xbe), load_function_starts(root), externs, wraps)
    funcs = parse_addrs(rest)
    os.makedirs(out_dir, exist_ok=True)
    bodies, failed = [], []
    queue, queued = list(funcs), set(funcs)
    extra = sorted(lifter.data_code_pointers() - queued - lifter.externs)
    queue += extra                    # code addresses stored in data: possible indirect call targets
    queued |= set(extra)
    while queue:
        f = queue.pop(0)
        try:
            body, _ = lifter.lift_function(f)
            bodies.append((f, body))
        except LiftError as e:
            failed.append((f, str(e)))
        except Exception as e:          # a lifter bug: report it with the function
            failed.append((f, f"internal error {type(e).__name__}: {e}"))
        for t in sorted((lifter.callees | lifter.code_refs) - queued - lifter.externs):
            queued.add(t)
            queue.append(t)          # called or referenced, but not in the function list: lift it too
    lifted = {f for f, _ in bodies}
    import re
    referenced = set()
    for _, body in bodies:
        referenced |= {int(x, 16) for x in re.findall(r"\bf_([0-9a-f]{8})\(c\)", body)}
    missing = sorted(t for t in (lifter.callees | referenced)
                     if t not in lifted and t not in lifter.externs)
    with open(os.path.join(out_dir, "lifted.h"), "w", newline="\n") as h:
        h.write("// Generated by tools/recomp/lift.py from default.xbe. Do not edit.\n#pragma once\n"
                "#include \"cpu.h\"\nnamespace tje {\n")
        for f, _ in bodies:
            h.write(f"void f_{f:08x}(Cpu* c);\n")
        for f in sorted(lifter.externs):
            h.write(f"void hle_{f:08x}(Cpu* c);\n")
        for f in sorted(lifter.wraps):
            h.write(f"void wrap_{f:08x}(Cpu* c);\n")
        for f in sorted(set(missing) | {f for f, _ in failed}):
            h.write(f"void f_{f:08x}(Cpu* c);   // not lifted: faults if reached\n")
        h.write("}  // namespace tje\n")
    # Bodies go into chunks of about 1 MB so they compile in parallel.
    for old in os.listdir(out_dir):
        if old.startswith("lifted") and old.endswith(".cpp"):
            os.remove(os.path.join(out_dir, old))
    chunks, cur, size = [], [], 0
    for item in bodies:
        cur.append(item)
        size += len(item[1])
        if size > 1_000_000:
            chunks.append(cur)
            cur, size = [], 0
    if cur:
        chunks.append(cur)
    header = ("// Generated by tools/recomp/lift.py from default.xbe. Do not edit.\n"
              "#include \"lifted.h\"\n#include \"cpu_extra.h\"\nnamespace tje {\n\n")
    for k, chunk in enumerate(chunks):
        with open(os.path.join(out_dir, f"lifted_{k:03d}.cpp"), "w", newline="\n") as cpp:
            cpp.write(header)
            for _, body in chunk:
                cpp.write(body + "\n\n")
            cpp.write("}  // namespace tje\n")
    with open(os.path.join(out_dir, "lifted_table.cpp"), "w", newline="\n") as cpp:
        cpp.write(header)
        for f in sorted(set(missing) | {f for f, _ in failed}):
            cpp.write(f"void f_{f:08x}(Cpu* c) {{ (void)c; fault(\"function not lifted\", 0x{f:x}u); }}\n")
        cpp.write("\nconst LiftedEntry kLifted[] = {\n")
        for f, _ in sorted(bodies):
            cpp.write(f"    {{0x{f:08x}u, f_{f:08x}}},\n")
        cpp.write("};\nextern const unsigned kLiftedCount = sizeof(kLifted) / sizeof(kLifted[0]);\n")
        cpp.write("}  // namespace tje\n")
    print(f"lifted {len(bodies)} functions, {len(failed)} failed, {len(missing)} callees not lifted")
    for f, e in failed:
        print(f"  FAIL {f:08x}: {e}")
    if missing:
        print("  missing callees:", " ".join(f"{t:08x}" for t in missing[:40]))


if __name__ == "__main__":
    main()
