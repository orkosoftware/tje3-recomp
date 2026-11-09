"""Load default.xbe's sections at their virtual addresses (for emulation and disassembly)."""
import struct


class XbeImage:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[:4] != b"XBEH":
            raise ValueError("not an XBE")
        self.base = struct.unpack_from("<I", self.data, 0x104)[0]
        nsec, sec_hdrs = struct.unpack_from("<II", self.data, 0x11c)
        self.sections = []
        for i in range(nsec):
            o = sec_hdrs - self.base + i * 56
            flags, va, vsize, raw, rsize, name_addr = struct.unpack_from("<6I", self.data, o)
            n = name_addr - self.base
            name = self.data[n:self.data.index(b"\0", n)].decode("latin-1")
            self.sections.append((name, va, vsize, raw, rsize))

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def read(self, va, size):
        for name, sva, vsize, raw, rsize in self.sections:
            if sva <= va < sva + vsize:
                o = va - sva
                chunk = self.data[raw + o:raw + min(rsize, o + size)] if o < rsize else b""
                return chunk + b"\0" * (size - len(chunk))
        raise ValueError(f"address {va:#x} not in any section")

    def end(self):
        return max(va + vsize for _, va, vsize, _, _ in self.sections)
