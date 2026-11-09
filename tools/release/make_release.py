"""Assemble a release: the public source folder and the Windows package.

Usage: python make_release.py --out <dir> --vulkan <Vulkan SDK dir> [--version v1.0.0] [--no-zip]

Only files named here are copied, so nothing else in a working copy can get in.
Afterwards every file is scanned for the game's bytes and for private names,
and any hit stops the release. Private names come from this PC (git identity,
Windows user, and optionally userdata/private_names.txt, one per line), so
they never appear in this script.
"""
import argparse
import fnmatch
import getpass
import glob
import os
import random
import shutil
import subprocess
import sys
import zipfile

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
GAME_DATA = os.path.join(REPO, "original", "xiso_extracted")

TOOLS = ["tools/recomp/build_game.py", "tools/recomp/lift.py", "tools/recomp/gen_hle_stubs.py",
         "tools/emu/xbe_image.py", "tools/xdvdfs_extract.py", "tools/xbox_texture.py",
         "tools/android/android_build.py", "tools/android/build_apk.py", "tools/android/AndroidManifest.xml"]
RELEASE_TOOLS = ["setup.py", "launcher.cpp", "launcher.rc", "launcher.manifest", "make_release.py", "README_PUBLIC.md"]
NEVER_ANYWHERE = ["*.xbe", "*.iso", "*.xbx", "*.dmp", "lifted_*", "hle_libs_generated*"]
NEVER_OURS = ["*.o", "*.obj", "*.pdb"]      # the compiler's own start-up objects are fine
PYTHON_DLLS = ["_ctypes.pyd", "libffi-8.dll", "_queue.pyd", "select.pyd",
               "_socket.pyd", "_ssl.pyd", "libssl-3.dll", "libcrypto-3.dll",
               "unicodedata.pyd"]               # the last five: fetching Google's NDK
PYTHON_SKIP_LIB = {"test", "idlelib", "tkinter", "turtledemo", "ensurepip", "venv", "site-packages", "pydoc_data",
                   "__pycache__", "__phello__", "wsgiref", "xmlrpc", "dbm", "sqlite3", "curses"}
TOOLCHAIN_DIRS = ["include", "x86_64-w64-mingw32"]
SKIP_CLANG_TOOLS = ("clang-tidy", "clang-format", "clang-query", "clang-scan-deps", "clang-apply", "clang-include",
                    "clang-repl", "clangd", "clang-check", "clang-doc", "clang-move", "clang-reorder", "clang-rename")


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def tracked(prefix):
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", prefix],
                         cwd=REPO, capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b"\0") if p]


def public_function_map(dst):
    """Addresses and labels only; the lifter needs no names, and the level
    generator's call graph is folded in as extra function starts."""
    lines = open(os.path.join(REPO, "analysis", "function_map.tsv"), encoding="utf-8").read().splitlines()
    rows = ["\t".join(lines[0].split("\t")[:5] + ["name"])]
    rows += ["\t".join(line.split("\t")[:5] + [""]) for line in lines[1:]]
    starts = set()
    for line in open(os.path.join(REPO, "analysis", "decomp", "levelgen_callgraph.tsv")).read().splitlines():
        parts = line.split("\t")
        starts.add(int(parts[0], 16))
        starts.update(int(x, 16) for x in parts[2].split())
    rows += [f"{a:08x}\t0\t.text\tSTART\tcallgraph\t" for a in sorted(starts)]
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")


def source_tree(dst):
    for p in tracked("runtime") + TOOLS:
        copy(os.path.join(REPO, p), os.path.join(dst, p))
    public_function_map(os.path.join(dst, "analysis", "function_map.tsv"))


def public_repo(dst):
    source_tree(dst)
    copy(os.path.join(REPO, "CMakeLists.txt"), os.path.join(dst, "CMakeLists.txt"))
    for f in RELEASE_TOOLS:
        copy(os.path.join(REPO, "tools", "release", f), os.path.join(dst, "tools", "release", f))
    copy(os.path.join(REPO, "tools", "release", "README_PUBLIC.md"), os.path.join(dst, "README.md"))
    with open(os.path.join(dst, ".gitignore"), "w", newline="\n") as f:
        f.write("/build/\n/original/\n/userdata/\n*.iso\n*.xbe\n*.xbx\n__pycache__/\n")


def build_launcher(dst):
    here = os.path.join(REPO, "tools", "release")
    tmp = os.path.join(os.path.dirname(dst), "launcher_res.o")
    subprocess.run(["llvm-windres", "launcher.rc", "-o", tmp], cwd=here, check=True)
    subprocess.run(["clang++", "-O2", "-s", "-municode", "-mwindows", "-static", "-std=gnu++17",
                    f"-ffile-prefix-map={REPO}=.", "launcher.cpp", tmp, "-lcomctl32", "-lcomdlg32", "-lshell32",
                    "-lole32", "-luuid", "-o", dst], cwd=here, check=True)
    os.remove(tmp)


def add_python(dst, licenses):
    base = sys.base_prefix
    for f in ("python.exe", "python314.dll", "python3.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
        copy(os.path.join(base, f), os.path.join(dst, f))
    for f in PYTHON_DLLS:
        copy(os.path.join(base, "DLLs", f), os.path.join(dst, "DLLs", f))
    lib = os.path.join(base, "Lib")
    for root, dirs, names in os.walk(lib):
        dirs[:] = [d for d in dirs if d not in PYTHON_SKIP_LIB]
        for n in names:
            if not n.endswith(".pyc"):
                copy(os.path.join(root, n), os.path.join(dst, "Lib", os.path.relpath(root, lib), n))
    import capstone
    cap = os.path.dirname(capstone.__file__)
    for root, dirs, names in os.walk(cap):
        dirs[:] = [d for d in dirs if d != "__pycache__" and d != "include"]
        for n in names:
            if not n.endswith((".pyc", ".lib", ".a")):
                copy(os.path.join(root, n), os.path.join(dst, "Lib", "site-packages", "capstone", os.path.relpath(root, cap), n))
    with open(os.path.join(dst, "python314._pth"), "w", newline="\n") as f:
        f.write("Lib\nDLLs\nLib/site-packages\n")
    copy(os.path.join(base, "LICENSE.txt"), os.path.join(licenses, "python.txt"))
    site = os.path.dirname(cap)
    for d in os.listdir(site):
        if d.lower().startswith("capstone-") and d.endswith(".dist-info"):
            for root, _dirs, names in os.walk(os.path.join(site, d)):
                for n in names:
                    if "licen" in n.lower():
                        copy(os.path.join(root, n), os.path.join(licenses, "capstone.txt"))


def add_toolchain(dst, licenses):
    base = os.path.dirname(os.path.dirname(shutil.which("clang++")))
    for n in os.listdir(os.path.join(base, "bin")):
        low = n.lower()
        keep = low.endswith(".cfg") or low in ("lld.exe", "ld.lld.exe", "ld.exe") or \
            (low.endswith(".dll") and not low.startswith(("liblldb", "libpython"))) or \
            (low.startswith("clang") and low.endswith(".exe") and not low.startswith(SKIP_CLANG_TOOLS))
        if keep:
            copy(os.path.join(base, "bin", n), os.path.join(dst, "bin", n))
    for d in TOOLCHAIN_DIRS:
        shutil.copytree(os.path.join(base, d), os.path.join(dst, d))
    clang = os.path.join(base, "lib", "clang")
    for root, _dirs, names in os.walk(clang):
        rel = os.path.relpath(root, clang)
        for n in names:
            parts = rel.split(os.sep)
            if len(parts) > 1 and parts[1] == "lib" and (os.path.basename(root) != "windows" or "x86_64" not in n):
                continue
            copy(os.path.join(root, n), os.path.join(dst, "lib", "clang", rel, n))
    copy(os.path.join(base, "LICENSE.TXT"), os.path.join(licenses, "llvm-mingw.txt"))


def add_third_party(setup, vulkan, licenses):
    inc = os.path.join(vulkan, "Include")
    out = os.path.join(setup, "src", "third_party", "include")
    for sub in ("vulkan", "shaderc", "vk_video"):
        for n in os.listdir(os.path.join(inc, sub)):
            if n.endswith(".h"):
                copy(os.path.join(inc, sub, n), os.path.join(out, sub, n))
    copy(os.path.join(vulkan, "Bin", "shaderc_shared.dll"), os.path.join(setup, "shaderc_shared.dll"))
    copy(os.path.join(vulkan, "Licenses", "LICENSE.txt"), os.path.join(licenses, "vulkan-sdk.txt"))
    with open(os.path.join(licenses, "NOTICE.txt"), "w", newline="\n") as f:
        f.write("shaderc_shared.dll: Copyright The Shaderc Authors, Apache License 2.0.\n"
                "Vulkan headers: Copyright The Khronos Group Inc., Apache License 2.0 or MIT.\n"
                "The text of the Apache License 2.0 is in llvm-mingw.txt and at "
                "https://www.apache.org/licenses/LICENSE-2.0\n")


def add_android(setup, sdk, shaderc, licenses):
    """What setup needs for an APK, apart from the NDK the player fetches: the
    open-source shader compiler, built ahead, and the app's manifest and resources."""
    sys.path[:0] = [os.path.join(REPO, "tools", "android"), os.path.join(REPO, "tools")]
    import android_build
    import xbox_texture
    newest = lambda pattern: sorted(glob.glob(pattern))[-1]
    ndk = newest(os.path.join(sdk, "ndk", "*"))
    strip = os.path.join(android_build.ndk_bin(ndk), "llvm-strip.exe")
    out = os.path.join(setup, "android")
    os.makedirs(os.path.join(out, "shaderc"))
    for lib in glob.glob(os.path.join(shaderc, "*.a")):
        dst = os.path.join(out, "shaderc", os.path.basename(lib))
        shutil.copyfile(lib, dst)
        subprocess.run([strip, "--strip-debug", dst], check=True)
    work = os.path.join(os.path.dirname(setup), "..", "android_skeleton")
    os.makedirs(work, exist_ok=True)
    skeleton = android_build.make_skeleton(os.path.join(newest(os.path.join(sdk, "build-tools", "*")), "aapt2.exe"),
                                           os.path.join(newest(os.path.join(sdk, "platforms", "android-*")), "android.jar"),
                                           work, lambda p: xbox_texture.write_png(p, 1, 1, bytes(4)))
    shutil.copyfile(skeleton, os.path.join(out, "skeleton.apk"))
    shutil.rmtree(work)
    third = os.path.join(ndk, "sources", "third_party", "shaderc")
    for pattern, name in ((os.path.join(third, "third_party", "glslang", "LICENSE*"), "glslang.txt"),
                          (os.path.join(third, "third_party", "spirv-tools", "LICENSE*"), "spirv-tools.txt")):
        found = glob.glob(pattern)
        if found:
            copy(found[0], os.path.join(licenses, name))
    with open(os.path.join(licenses, "NOTICE.txt"), "a", newline="\n") as f:
        f.write("setup/android/shaderc: shaderc, glslang and SPIRV-Tools, built from their open-source code; "
                "see their licence files here.\n")


def package(dst, vulkan, android_sdk, shaderc):
    setup = os.path.join(dst, "setup")
    licenses = os.path.join(dst, "licenses")
    os.makedirs(licenses)
    build_launcher(os.path.join(dst, "TJE3.exe"))
    copy(os.path.join(REPO, "tools", "release", "setup.py"), os.path.join(setup, "setup.py"))
    copy(os.path.join(REPO, "tools", "release", "README_PUBLIC.md"), os.path.join(dst, "README.txt"))
    source_tree(os.path.join(setup, "src"))
    add_third_party(setup, vulkan, licenses)
    add_python(os.path.join(setup, "python"), licenses)
    add_toolchain(os.path.join(setup, "toolchain"), licenses)
    add_android(setup, android_sdk, shaderc, licenses)


# ---------------------------------------------------------------- the audit

def walk(top):
    for root, dirs, names in os.walk(top):
        dirs[:] = [d for d in dirs if d != ".git"]
        for n in names:
            yield os.path.join(root, n)


def is_third_party(path):
    p = path.replace("\\", "/")
    return any(s in p for s in ("/setup/python/", "/setup/toolchain/", "/setup/src/third_party/", "/licenses/",
                                "/setup/shaderc_shared.dll", "/setup/android/shaderc/"))


def private_names():
    """(names that must appear nowhere, names that must not appear in our own files)"""
    everywhere = {getpass.getuser(), REPO, REPO.replace("\\", "/")}
    for key in ("user.name", "user.email"):
        r = subprocess.run(["git", "config", key], cwd=REPO, capture_output=True, text=True)
        if r.stdout.strip():
            everywhere.add(r.stdout.strip())
    own = set(everywhere)
    extra = os.path.join(REPO, "userdata", "private_names.txt")
    if os.path.exists(extra):
        own |= {line.strip() for line in open(extra, encoding="utf-8") if line.strip()}
    return [s.lower().encode() for s in everywhere], [s.lower().encode() for s in own]


def game_samples():
    """Slices of the game's program and data, to look for in every file."""
    rng = random.Random(3)
    samples = []
    def take(path, count):
        data = open(path, "rb").read()
        tries = 0
        got = 0
        while got < count and tries < count * 50 and len(data) > 64:
            tries += 1
            o = rng.randrange(0, len(data) - 48)
            s = data[o:o + 48]
            if len(set(s)) >= 24:
                samples.append(s)
                got += 1
    take(os.path.join(GAME_DATA, "default.xbe"), 60)
    files = sorted(f for f in walk(GAME_DATA) if not f.endswith(".xbe") and os.path.getsize(f) < (1 << 26))
    for f in rng.sample(files, min(30, len(files))):
        take(f, 1)
    return samples


def xbe_windows():
    data = open(os.path.join(GAME_DATA, "default.xbe"), "rb").read()
    start = int.from_bytes(data[0x108:0x10c], "little")      # past the header, which holds the game's title
    return {data[o:o + 16]: o for o in range(start, len(data) - 16, 8) if len(set(data[o:o + 16])) >= 8}


def copied_run(data, windows):
    """True if data holds 48 or more bytes in a row from the game's program.
    Shorter matches are shared text such as API names or a table of hex digits."""
    for o in range(max(0, len(data) - 39)):
        x = windows.get(data[o:o + 16])
        if x is not None and all(windows.get(data[o + k:o + k + 16]) == x + k for k in (8, 16, 24)):
            return True
    return False


def audit(tops):
    everywhere, own_names = private_names()
    samples = game_samples()
    windows = xbe_windows()
    problems, files, bytes_total = [], 0, 0
    for top in tops:
        for path in walk(top):
            name = os.path.basename(path).lower()
            third = is_third_party(path)
            if any(fnmatch.fnmatch(name, pat) for pat in (NEVER_ANYWHERE if third else NEVER_ANYWHERE + NEVER_OURS)):
                problems.append(f"forbidden file type: {path}")
            data = open(path, "rb").read()
            files += 1
            bytes_total += len(data)
            low = data.lower()
            for s in everywhere if third else own_names:
                if s in low:
                    problems.append(f"private name found in {path}")
            if any(s in data for s in samples):
                problems.append(f"game data found in {path}")
            if not third and copied_run(data, windows):
                problems.append(f"game code bytes found in {path}")
    print(f"audit: {files} files, {bytes_total >> 20} MB, {len(samples)} game samples, {len(windows)} code windows")
    return problems


def make_zip(folder, path):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for f in walk(folder):
            z.write(f, os.path.relpath(f, folder))


def main():
    a = argparse.ArgumentParser()
    a.add_argument("--out", required=True)
    a.add_argument("--vulkan", required=True)
    a.add_argument("--android-sdk", required=True)
    a.add_argument("--shaderc", required=True, help="folder of shaderc .a files built for arm64-v8a")
    a.add_argument("--version", default="v1.0.0")
    a.add_argument("--no-zip", action="store_true")
    args = a.parse_args()
    out = os.path.abspath(args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    repo, pkg = os.path.join(out, "repo"), os.path.join(out, "package")
    public_repo(repo)
    package(pkg, args.vulkan, args.android_sdk, args.shaderc)
    problems = audit([repo, pkg])
    if problems:
        print("\n".join(problems[:50]))
        sys.exit(f"RELEASE STOPPED: {len(problems)} problem(s)")
    print("audit passed")
    if not args.no_zip:
        path = os.path.join(out, f"TJE3-Recomp-Windows-x64-{args.version}.zip")
        make_zip(pkg, path)
        print(f"{path}: {os.path.getsize(path) >> 20} MB")


if __name__ == "__main__":
    main()
