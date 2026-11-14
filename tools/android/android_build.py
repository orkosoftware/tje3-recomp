"""Builds and signs the Android APK. Used by setup on the player's PC and by build_apk.py.

The player needs only Google's NDK, which they download themselves: the APK's
manifest and resources come ready-made in the release, and packaging and
signing are done here. The result holds the game's translated code and must
never be shared.
"""
import concurrent.futures
import glob
import hashlib
import json
import math
import os
import secrets
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
API = 28
ICON_ENTRY = "res/drawable-nodpi-v4/icon_image.png"
RUNTIME_SOURCES = ("main", "runtime", "kernel", "trace", "sched", "settings", "audio_out")


class BuildError(Exception):
    pass


def run(args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return r.returncode, r.stdout


def checked(args):
    code, out = run(args)
    if code:
        raise BuildError(f"{os.path.basename(args[0])} failed:\n{out[-3000:]}")
    return out


# ---------------------------------------------------------------- compiling

def ndk_bin(ndk):
    return os.path.join(ndk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin")


def compile_library(ndk, lifted, root, includes, shaderc_libs, out_dir, progress=None, extra_flags=()):
    """Compiles the game for 64-bit ARM; returns the path of libtje3.so."""
    tools = ndk_bin(ndk)
    cxx, cc = os.path.join(tools, "clang++.exe"), os.path.join(tools, "clang.exe")
    glue = os.path.join(ndk, "sources", "android", "native_app_glue")
    objects_dir = os.path.join(out_dir, "obj")
    os.makedirs(objects_dir, exist_ok=True)
    runtime = os.path.join(root, "runtime")
    target = f"--target=aarch64-linux-android{API}"
    flags = [target, "-c", "-fPIC", "-O2", "-DNDEBUG", "-fsigned-char", "-I", runtime, "-I", lifted, "-I", glue]
    for inc in includes:
        flags += ["-I", inc]
    flags += list(extra_flags)
    sources = [(os.path.join(runtime, n + ".cpp"), []) for n in RUNTIME_SOURCES]
    sources += [(f, []) for f in glob.glob(os.path.join(runtime, "hle", "*.cpp"))]
    sources += [(f, []) for f in glob.glob(os.path.join(runtime, "gpu", "*.cpp"))]
    sources.append((os.path.join(lifted, "hle_libs_generated.cpp"), []))
    sources += [(f, ["-w"]) for f in glob.glob(os.path.join(lifted, "lifted_*.cpp"))]
    headers = max(os.path.getmtime(f) for f in glob.glob(os.path.join(runtime, "**", "*.h"), recursive=True))
    jobs, objects = [], []
    for i, (src, extra) in enumerate(sources):
        obj = os.path.join(objects_dir, f"o{i:03d}.o")
        objects.append(obj)
        if not (os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(src), headers)):
            jobs.append([cxx, "-std=gnu++17"] + flags + extra + [src, "-o", obj])
    glue_obj = os.path.join(objects_dir, "glue.o")
    jobs.append([cc] + flags + ["-w", os.path.join(glue, "android_native_app_glue.c"), "-o", glue_obj])
    objects.append(glue_obj)
    done = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        for code, output in pool.map(run, jobs):
            if code:
                raise BuildError("The Android compiler reported a problem:\n" + output[-3000:])
            done += 1
            if progress:
                progress(done, len(jobs))
    rsp = os.path.join(objects_dir, "link.rsp")
    with open(rsp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objects))
    so = os.path.join(out_dir, "libtje3.so")
    checked([cxx, target, "-shared", "-s", "-static-libstdc++", "-u", "ANativeActivity_onCreate",
             "-Wl,-z,max-page-size=16384", "@" + rsp, "-Wl,--start-group"] + list(shaderc_libs) +
            ["-Wl,--end-group", "-landroid", "-llog", "-laaudio", "-ldl", "-lm", "-o", so])
    return so


# ---------------------------------------------------------------- the ready-made part

def make_skeleton(aapt2, android_jar, out_dir, icon_writer):
    """Manifest and resources (release time, needs Google's aapt2). icon_writer(path) writes a PNG."""
    res = os.path.join(out_dir, "res")
    if os.path.isdir(res):
        shutil.rmtree(res)
    for d in ("drawable-nodpi", "mipmap-anydpi-v26", "drawable", "values"):
        os.makedirs(os.path.join(res, d))
    icon_writer(os.path.join(res, "drawable-nodpi", "icon_image.png"))
    ns = 'xmlns:android="http://schemas.android.com/apk/res/android"'

    def write(rel, text):
        with open(os.path.join(res, rel), "w", encoding="utf-8", newline="\n") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n' + text + "\n")
    write("values/icon.xml", '<resources><color name="icon_background">#000000</color></resources>')
    write("drawable/icon_foreground.xml", f'<inset {ns} android:drawable="@drawable/icon_image" android:inset="16.67%"/>')
    write("mipmap-anydpi-v26/ic_launcher.xml",
          f'<adaptive-icon {ns}><background android:drawable="@color/icon_background"/>'
          '<foreground android:drawable="@drawable/icon_foreground"/></adaptive-icon>')
    compiled = os.path.join(out_dir, "res.zip")
    checked([aapt2, "compile", "--no-crunch", "--dir", res, "-o", compiled])
    skeleton = os.path.join(out_dir, "skeleton.apk")
    checked([aapt2, "link", "-I", android_jar, "--manifest", os.path.join(HERE, "AndroidManifest.xml"),
             "--auto-add-overlay", "-R", compiled, "--min-sdk-version", str(API), "--target-sdk-version", "33",
             "-o", skeleton])
    return skeleton


# ---------------------------------------------------------------- signing

def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def der(tag, *parts):
    body = b"".join(parts)
    return bytes([tag]) + der_len(len(body)) + body


def der_int(v):
    b = v.to_bytes(v.bit_length() // 8 + 1, "big")
    return der(0x02, b)


SHA256_RSA = der(0x30, bytes.fromhex("06092a864886f70d01010b"), b"\x05\x00")
RSA_KEY = der(0x30, bytes.fromhex("06092a864886f70d010101"), b"\x05\x00")
DIGEST_INFO = bytes.fromhex("3031300d060960864801650304020105000420")


def is_prime(n):
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(40):
        x = pow(secrets.randbelow(n - 3) + 2, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = x * x % n
            if x == n - 1:
                break
        else:
            return False
    return True


def random_prime(bits, e):
    while True:
        p = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if math.gcd(p - 1, e) == 1 and is_prime(p):
            return p


def sign_rsa(key, digest):
    size = (key["n"].bit_length() + 7) // 8
    t = DIGEST_INFO + digest
    block = b"\x00\x01" + b"\xff" * (size - len(t) - 3) + b"\x00" + t
    return pow(int.from_bytes(block, "big"), key["d"], key["n"]).to_bytes(size, "big")


def public_key_der(key):
    rsa = der(0x30, der_int(key["n"]), der_int(key["e"]))
    return der(0x30, RSA_KEY, der(0x03, b"\x00" + rsa))


def make_key():
    """A new signing key and self-signed certificate, unique to this PC."""
    e = 65537
    p, q = random_prime(1024, e), random_prime(1024, e)
    while p == q:
        q = random_prime(1024, e)
    key = {"n": p * q, "e": e, "d": pow(e, -1, (p - 1) * (q - 1))}
    name = der(0x30, der(0x31, der(0x30, bytes.fromhex("0603550403"), der(0x0c, b"tje3 personal build"))))
    validity = der(0x30, der(0x17, b"250101000000Z"), der(0x18, b"20991231235959Z"))
    tbs = der(0x30, der(0xa0, der_int(2)), der_int(secrets.randbits(63) + 1), SHA256_RSA, name, validity, name,
              public_key_der(key))
    signature = sign_rsa(key, hashlib.sha256(tbs).digest())
    key["cert"] = der(0x30, tbs, SHA256_RSA, der(0x03, b"\x00" + signature)).hex()
    return key


def load_key(path):
    """The key in path, or a new one saved there, so a rebuilt APK can update the installed app."""
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            k = json.load(f)
        return {"n": int(k["n"], 16), "e": k["e"], "d": int(k["d"], 16), "cert": k["cert"]}
    key = make_key()
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"n": f"{key['n']:x}", "e": key["e"], "d": f"{key['d']:x}", "cert": key["cert"]}, f)
    return key


def lp(*parts):
    body = b"".join(parts)
    return struct.pack("<I", len(body)) + body


def apk_digest(f, entries_len, central, tail):
    """APK Signature Scheme v2: SHA-256 over 1 MB chunks of each section."""
    def pieces():
        f.seek(0)
        left = entries_len
        while left:
            piece = f.read(min(1 << 20, left))
            left -= len(piece)
            yield piece
        for section in (central, tail):
            for at in range(0, len(section), 1 << 20):
                yield section[at:at + (1 << 20)]
    chunks = [hashlib.sha256(b"\xa5" + struct.pack("<I", len(p)) + p).digest() for p in pieces()]
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def signing_block(f, entries_len, central, tail, key):
    alg = 0x0103                               # RSASSA-PKCS1-v1_5 with SHA-256
    digest = apk_digest(f, entries_len, central, tail)
    cert = bytes.fromhex(key["cert"])
    digests = lp(lp(struct.pack("<I", alg) + lp(digest)))
    certs = lp(lp(cert))
    signed = digests + certs + lp()
    signature = sign_rsa(key, hashlib.sha256(signed).digest())
    signatures = lp(lp(struct.pack("<I", alg) + lp(signature)))
    signer = lp(signed) + signatures + lp(public_key_der(key))
    value = lp(lp(signer))
    pair = struct.pack("<QI", 4 + len(value), 0x7109871a) + value
    size = len(pair) + 8 + 16                  # counts everything after the first size field
    return struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"


def deflated(data):
    """(crc, stored bytes, deflated?) for one file; data that won't shrink stays as it is."""
    crc = zlib.crc32(data)
    c = zlib.compressobj(6, zlib.DEFLATED, -15)
    packed = c.compress(data) + c.flush()
    return (crc, packed, True) if len(packed) < len(data) else (crc, data, False)


class ApkWriter:
    """A plain zip. Python's zipfile switches to ZIP64 past 2 GB, which Android can't read."""

    def __init__(self, f):
        self.f = f
        self.central = bytearray()
        self.count = 0

    def add(self, name, crc, body, size, is_deflated):
        raw = name.encode()
        offset = self.f.tell()
        if offset + len(body) > 0xFFFF0000 or self.count >= 0xFFFF:
            raise BuildError("The game is too large to fit in one APK.")
        method = 8 if is_deflated else 0
        pad = b"" if is_deflated else b"\0" * ((4 - (offset + 30 + len(raw)) % 4) % 4)    # 4-byte boundary
        self.f.write(struct.pack("<IHHHHHIIIHH", 0x04034b50, 20, 0, method, 0, 0x21, crc, len(body), size, len(raw), len(pad)))
        self.f.write(raw + pad)
        self.f.write(body)
        self.central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 20, 20, 0, method, 0, 0x21, crc, len(body), size,
                                    len(raw), 0, 0, 0, 0, 0, offset) + raw
        self.count += 1

    def add_data(self, name, data, compress):
        crc, body, is_deflated = deflated(data) if compress else (zlib.crc32(data), data, False)
        self.add(name, crc, body, len(data), is_deflated)

    def finish(self, key):
        entries_len = self.f.tell()
        central = bytes(self.central)
        tail = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, self.count, self.count, len(central), entries_len, 0)
        self.f.flush()
        block = signing_block(self.f, entries_len, central, tail, key)
        self.f.seek(entries_len)
        self.f.write(block)
        self.f.write(central)
        self.f.write(tail[:16] + struct.pack("<I", entries_len + len(block)) + tail[20:])


def game_files(assets):
    """(path inside the game folder, file on disk) for everything under the asset folders."""
    out = []
    for prefix, folder in assets:
        for root, _dirs, names in os.walk(folder):
            for n in names:
                path = os.path.join(root, n)
                out.append((prefix + "/" + os.path.relpath(path, folder).replace("\\", "/"), path))
    return sorted(out)


def add_game_files(w, assets, progress):
    files = game_files(assets)
    sizes = [os.path.getsize(p) for _rel, p in files]
    listing = "".join(f"{s}\t{rel}\n" for s, (rel, _p) in zip(sizes, files)).encode()
    w.add_data("assets/game_files.txt", listing, False)

    def pack(item):
        with open(item[1], "rb") as f:
            data = f.read()
        return (item[0], len(data)) + deflated(data)
    total = sum(sizes) or 1
    done = at = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        while at < len(files):
            batch, used = [], 0
            while at < len(files) and (not batch or used + sizes[at] < (1 << 29)):   # at most 512 MB in memory
                batch.append(files[at])
                used += sizes[at]
                at += 1
            for rel, size, crc, body, is_deflated in pool.map(pack, batch):
                w.add("assets/game/" + rel, crc, body, size, is_deflated)
                done += size
                if progress:
                    progress(done, total)


def package(skeleton, so, icon_png, key, out_apk, assets=(), progress=None):
    """Writes the signed APK. assets: (name in the game folder, folder on disk) pairs to carry inside it."""
    temp = out_apk + ".tmp"
    with open(temp, "w+b") as out, zipfile.ZipFile(skeleton) as src:
        w = ApkWriter(out)
        for item in src.infolist():
            name = item.filename
            data = icon_png if name == ICON_ENTRY else src.read(name)
            w.add_data(name, data, not (name == "resources.arsc" or name.endswith(".png")))
        with open(so, "rb") as f:
            w.add_data("lib/arm64-v8a/libtje3.so", f.read(), True)
        if assets:
            add_game_files(w, assets, progress)
        w.finish(key)
    os.replace(temp, out_apk)
    return out_apk
