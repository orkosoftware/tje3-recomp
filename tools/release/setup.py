"""Sets the game up on the player's PC from the player's own disc.

Usage: python setup.py --source <disc image or folder> [--dest <folder>] [--dlc <folder>]...
       python setup.py --check-source <disc image or folder>
       python setup.py --check-dlc <folder> [--check-dlc <folder>]...

Run by TJE3.exe from the setup folder of an unpacked release. A release holds
no game code or data: this script copies the disc files, translates the
game's program to C++ and compiles it here, so that output only ever exists
on the PC of someone who has the disc. Lines starting with "@P" report
progress, and "@R 1" or "@R 0" give the answer to a check.
"""
import argparse
import concurrent.futures
import glob
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALL = os.path.dirname(HERE)
SRC = os.path.join(HERE, "src")
TOOLCHAIN = os.path.join(HERE, "toolchain")
ANDROID = os.path.join(HERE, "android")

sys.path.insert(0, os.path.join(SRC, "tools"))
sys.path.insert(0, os.path.join(SRC, "tools", "recomp"))
sys.path.insert(0, os.path.join(SRC, "tools", "emu"))
sys.path.insert(0, os.path.join(SRC, "tools", "android"))
from xdvdfs_extract import ATTR_DIR, MAGIC, SECTOR, walk_tree  # noqa: E402
import android_build  # noqa: E402
import build_game  # noqa: E402
from xbe_image import XbeImage  # noqa: E402
import xbox_texture  # noqa: E402

XBE_SHA1 = "eb00994059d1b7d357810a1d80c8878e8f14d10b"     # USA release
TITLE_ID = 0x5345000F
OFFER_NAMES = {1: "Suteki", 2: "EarlBot", 3: "GeekJam", 0x10: "Hades Environment", 0x20: "Funkadelic Environment"}
# Where the game partition starts: extracted-style images, then full disc dumps.
ISO_OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)
RUNTIME_SOURCES = ("main", "runtime", "kernel", "trace", "sched", "settings", "audio_out")
SYSTEM_LIBS = ("winmm", "kernel32", "user32", "gdi32", "shell32", "ole32", "oleaut32", "uuid", "advapi32")
BUILD_SPACE = 2 << 30                # room for the compiler's temporary files
MARKER = "installed_by_tje3_setup.txt"
# Google's own download, with the size and SHA-1 it publishes. Never bundled: its terms don't allow that.
NDK_URL = "https://dl.google.com/android/repository/android-ndk-r27d-windows.zip"
NDK_SIZE = 781506724
NDK_SHA1 = "56607cbccd3642d4a1991f6bb3114a00f884f426"
ANDROID_SPACE = 7 << 30
ANDROID_STEPS = """tje3.apk contains the game and any downloadable content you added, built from your own disc.
It's for your own device: please don't share it.

1. Copy tje3.apk to your Android device and open it to install it.
   Android may ask you to allow installs from that app.
2. Open ToeJam & Earl III. The first launch unpacks the game, with a progress bar. This takes a minute or two.
   Later launches start straight away. Keep about 6 GB free on the device.

Your saved games are in Internal storage > Android > data > com.orkosoftware.tje3 > files.
Uninstalling the app deletes them, so copy that folder somewhere safe first.
"""


class SetupError(Exception):
    pass


def progress(percent, text):
    print(f"@P {percent} {text}", flush=True)


def wrong_copy():
    return SetupError("This isn't the supported copy of the game. Only the USA Xbox release works.")


class Iso:
    def __init__(self, path):
        self.f = open(path, "rb")
        for offset in ISO_OFFSETS:
            self.f.seek(offset + 32 * SECTOR)
            vd = self.f.read(SECTOR)
            if vd[:20] == MAGIC and vd[0x7EC:0x800] == MAGIC:
                self.offset = offset
                self.root = struct.unpack_from("<II", vd, 20)
                return
        raise SetupError("That file isn't an Xbox disc image. It must be an uncompressed .iso.")

    def files(self):
        out, dirs = [], [("", *self.root)]
        while dirs:
            path, sector, size = dirs.pop()
            if not size:
                continue
            self.f.seek(self.offset + sector * SECTOR)
            entries = []
            walk_tree(self.f.read(size), 0, entries, set())
            for name, start, fsize, attr in entries:
                if not name or name in (".", "..") or "/" in name or "\\" in name or ":" in name:
                    raise SetupError(f"The disc image has an invalid file name: {name!r}")
                rel = f"{path}/{name}" if path else name
                if attr & ATTR_DIR:
                    dirs.append((rel, start, fsize))
                else:
                    out.append((rel, start, fsize))
        return out

    def chunks(self, start, size):
        self.f.seek(self.offset + start * SECTOR)
        while size:
            chunk = self.f.read(min(size, 1 << 22))
            if not chunk:
                raise SetupError("The disc image ended early; it looks incomplete.")
            size -= len(chunk)
            yield chunk


def folder_files(path):
    return [os.path.join(root, n) for root, _dirs, names in os.walk(path) for n in names]


def inspect_source(source):
    """Confirms that source is the supported game and returns its size in bytes."""
    if os.path.isdir(source):
        xbe = os.path.join(source, "default.xbe")
        if not os.path.isfile(xbe):
            raise SetupError("That folder has no default.xbe, so it isn't an extracted copy of the game.")
        with open(xbe, "rb") as f:
            if hashlib.sha1(f.read()).hexdigest() != XBE_SHA1:
                raise wrong_copy()
        return sum(os.path.getsize(f) for f in folder_files(source))
    iso = Iso(source)
    files = iso.files()
    xbe = next((f for f in files if f[0].lower() == "default.xbe"), None)
    if not xbe:
        raise SetupError("That disc image has no default.xbe.")
    if hashlib.sha1(b"".join(iso.chunks(xbe[1], xbe[2]))).hexdigest() != XBE_SHA1:
        raise wrong_copy()
    return sum(f[2] for f in files)


def copy_from_iso(path, dest):
    iso = Iso(path)
    files = iso.files()
    total = sum(f[2] for f in files) or 1
    done = 0
    for rel, start, size in files:
        target = os.path.join(dest, *rel.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as o:
            for chunk in iso.chunks(start, size):
                o.write(chunk)
                done += len(chunk)
                progress(2 + 18 * done // total, "Copying the game files from your disc image")


def copy_from_folder(path, dest):
    files = folder_files(path)
    total = sum(os.path.getsize(f) for f in files) or 1
    done = 0
    for source in files:
        target = os.path.join(dest, os.path.relpath(source, path))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(source, target)
        done += os.path.getsize(source)
        progress(2 + 18 * done // total, "Copying the game files from your folder")


def inside(path, folder):
    path, folder = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(folder))
    try:
        return os.path.commonpath([path, folder]) == folder
    except ValueError:                      # different drives
        return False


def prepare_destination(game, source, size):
    """Stops before anything is copied if the folder can't take the game."""
    data = os.path.join(game, "data")
    marker = os.path.join(game, MARKER)
    if inside(source, data):
        raise SetupError("Your copy of the game is inside the install folder. Choose a different install folder.")
    if os.path.isdir(data) and not os.path.exists(marker) and os.listdir(data):
        raise SetupError(f"{game} already holds a folder named data that isn't from this game. "
                         "Choose a different install folder.")
    existing = game
    while not os.path.isdir(existing):
        parent = os.path.dirname(existing)
        if parent == existing:
            raise SetupError(f"The drive for {game} isn't available.")
        existing = parent
    need = size + BUILD_SPACE
    free = shutil.disk_usage(existing).free
    if free < need:
        raise SetupError(f"That drive needs about {need / (1 << 30):.1f} GB free and has {free / (1 << 30):.1f} GB. "
                         "Free some space or choose another drive.")
    try:
        os.makedirs(game, exist_ok=True)
        with open(marker, "w", encoding="utf-8") as f:
            f.write("This folder was set up from the player's own copy of the game. Please don't share it.\n")
    except OSError:
        raise SetupError(f"Windows won't allow writing to {game}. "
                         "Choose a folder outside Program Files, such as one in Documents or on another drive.")


def copy_game_data(game, source):
    dest = os.path.join(game, "data")
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    if os.path.isdir(source):
        copy_from_folder(source, dest)
    else:
        copy_from_iso(source, dest)


def find_packs(folder):
    """Pack folders at or under folder, with their offer ids."""
    packs = []
    for root, dirs, names in os.walk(folder):
        meta = next((n for n in names if n.lower() == "contentmeta.xbx"), None)
        if not meta:
            continue
        dirs[:] = []
        with open(os.path.join(root, meta), "rb") as f:
            header = f.read(0x6c)
        if len(header) == 0x6c and header[0x14:0x18] == b"XCSF" and struct.unpack_from("<I", header, 0x24)[0] == TITLE_ID:
            packs.append((root, struct.unpack_from("<I", header, 0x28)[0]))
        else:
            print(f"Skipping {root}: not content for this game.", flush=True)
    return packs


def all_packs(folders):
    """Offer id -> pack folder, one each, whichever folder it came from."""
    return {offer: path for folder in folders for path, offer in find_packs(folder)}


def copy_dlc(game, folders):
    root = os.path.join(game, "DLC")
    os.makedirs(root, exist_ok=True)
    packs = all_packs(folders)
    for offer, path in sorted(packs.items()):
        name = f"{TITLE_ID:08X}{offer:08X}"
        print(f"Adding downloadable content {name}.", flush=True)
        shutil.copytree(path, os.path.join(root, name), dirs_exist_ok=True)
    return len(packs)


TOOL_ENV = dict(os.environ, PATH=os.path.join(TOOLCHAIN, "bin") + os.pathsep + os.environ.get("PATH", ""))


def run_tool(args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       env=TOOL_ENV)
    return r.returncode, r.stdout


def compile_game(game, work):
    os.environ["PATH"] = TOOL_ENV["PATH"]
    cxx = os.path.join(TOOLCHAIN, "bin", "clang++.exe")
    runtime = os.path.join(SRC, "runtime")
    base = [cxx, "-c", "-O2", "-DNDEBUG", "-std=gnu++17", "-I", runtime, "-I", work,
            "-I", os.path.join(SRC, "third_party", "include")]
    sources = [(os.path.join(runtime, n + ".cpp"), []) for n in RUNTIME_SOURCES]
    sources += [(f, []) for f in glob.glob(os.path.join(runtime, "hle", "*.cpp"))]
    sources += [(f, []) for f in glob.glob(os.path.join(runtime, "gpu", "*.cpp"))]
    sources.append((os.path.join(work, "hle_libs_generated.cpp"), []))
    sources += [(f, ["-w"]) for f in glob.glob(os.path.join(work, "lifted_*.cpp"))]

    objects, jobs = [], []
    for i, (source, extra) in enumerate(sources):
        obj = os.path.join(work, f"o{i:03d}.o")
        objects.append(obj)
        jobs.append(base + extra + [source, "-o", obj])
    done = 0
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 2) as pool:
        for code, output in pool.map(run_tool, jobs):
            if code:
                raise SetupError("The compiler reported a problem:\n" + output[-3000:])
            done += 1
            progress(45 + 50 * done // len(jobs), f"Compiling the game ({done} of {len(jobs)})")

    progress(96, "Linking the game")
    rsp = os.path.join(work, "link.rsp")
    with open(rsp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objects))
    code, output = run_tool([cxx, "-s", "-static", "@" + rsp, "-o", os.path.join(game, "tje3.exe")] +
                            ["-l" + lib for lib in SYSTEM_LIBS])
    if code:
        raise SetupError("Linking failed:\n" + output[-3000:])


def halve(rgba, size):
    """Half-size image: each pixel averages four, ignoring transparent ones' colour."""
    out = bytearray()
    for y in range(0, size, 2):
        for x in range(0, size, 2):
            px = [rgba[(y + j) * size * 4 + (x + i) * 4:(y + j) * size * 4 + (x + i) * 4 + 4] for j in (0, 1) for i in (0, 1)]
            alpha = sum(p[3] for p in px)
            out += bytes(sum(p[c] * p[3] for p in px) // alpha if alpha else 0 for c in range(3))
            out.append(alpha // 4)
    return bytes(out)


def make_icon(game):
    """Writes tje3.ico from the title image inside the player's default.xbe."""
    size, rgba = xbox_texture.title_image(XbeImage(os.path.join(game, "data", "default.xbe")))
    images = []
    temp = os.path.join(game, "_icon.png")
    while size >= 16:
        xbox_texture.write_png(temp, size, size, rgba)
        with open(temp, "rb") as f:
            images.append((size, f.read()))
        rgba = halve(rgba, size)
        size //= 2
    os.remove(temp)
    offset = 6 + 16 * len(images)
    with open(os.path.join(game, "tje3.ico"), "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for s, png in images:
            f.write(struct.pack("<BBBBHHII", s & 0xff, s & 0xff, 0, 0, 1, 32, len(png), offset))
            offset += len(png)
        for _s, png in images:
            f.write(png)


def fetch_ndk():
    """Downloads Google's NDK, once, and unpacks the parts the build needs."""
    root = os.path.join(ANDROID, "ndk")
    if os.path.isfile(os.path.join(android_build.ndk_bin(root), "clang++.exe")):
        return root
    zip_path = os.path.join(ANDROID, "ndk-download.zip")
    print(f"Downloading the Android NDK from Google: {NDK_URL}", flush=True)
    digest = hashlib.sha1()
    done = 0
    with urllib.request.urlopen(NDK_URL, timeout=60) as response, open(zip_path, "wb") as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            progress(100 * done // NDK_SIZE, f"Android: downloading tools from Google ({done >> 20} of {NDK_SIZE >> 20} MB)")
    if digest.hexdigest() != NDK_SHA1:
        os.remove(zip_path)
        raise SetupError("The Android NDK download was damaged. Try again.")
    progress(100, "Android: unpacking tools")
    wanted = ("toolchains/llvm/prebuilt/windows-x86_64/", "sources/android/native_app_glue/")
    with zipfile.ZipFile(zip_path) as z:
        prefix = z.namelist()[0].split("/")[0] + "/"
        for item in z.infolist():
            rel = item.filename[len(prefix):]
            if item.is_dir() or not rel.startswith(wanted) or "/python3/" in rel:
                continue
            target = os.path.join(root, *rel.split("/"))
            if not inside(target, root):
                raise SetupError("The Android NDK download has an invalid file name.")
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(item) as src, open(target, "wb") as out:
                shutil.copyfileobj(src, out)
    os.remove(zip_path)
    return root


def build_android(game, work, ndk):
    os.makedirs(ANDROID, exist_ok=True)
    ndk = ndk or fetch_ndk()
    build = os.path.join(work, "android")
    libs = sorted(glob.glob(os.path.join(ANDROID, "shaderc", "*.a")))
    includes = [os.path.join(SRC, "third_party", "include")]

    def report(done, total):
        progress(100 * done // total, f"Android: compiling the game ({done} of {total})")
    so = android_build.compile_library(ndk, work, SRC, includes, libs, build, report)
    progress(100, "Android: packaging the app")
    size, rgba = xbox_texture.title_image(XbeImage(os.path.join(game, "data", "default.xbe")))
    icon = os.path.join(build, "icon.png")
    xbox_texture.write_png(icon, size, size, rgba)
    with open(icon, "rb") as f:
        png = f.read()
    out = os.path.join(game, "Android")
    os.makedirs(out, exist_ok=True)
    key = android_build.load_key(os.path.join(out, "signing_key.json"))

    def report_packing(done, total):
        progress(100 * done // total, "Android: packing the game into the app")
    assets = [("data", os.path.join(game, "data")), ("DLC", os.path.join(game, "DLC"))]
    android_build.package(os.path.join(ANDROID, "skeleton.apk"), so, png, key, os.path.join(out, "tje3.apk"),
                          assets, report_packing)
    with open(os.path.join(out, "How to install on Android.txt"), "w", encoding="utf-8") as f:
        f.write(ANDROID_STEPS)
    print(f"Android app written to {out}. See 'How to install on Android.txt' in that folder.", flush=True)


def install(source, game, dlc, android=False, ndk=None, rebuild=False):
    """rebuild: a newer release was unpacked over an install. The game files are already
    in place, so only the program is translated and compiled again; saves, options and
    downloadable content stay as they are."""
    progress(1, "Checking your copy of the game")
    exe = os.path.join(game, "tje3.exe")
    if rebuild:
        inspect_source(os.path.join(game, "data"))
    else:
        size = inspect_source(source)
        prepare_destination(game, source, size + (ANDROID_SPACE if android else 0))
    if os.path.exists(exe):
        os.remove(exe)
    if not rebuild:
        copy_game_data(game, source)
    if dlc:
        progress(21, "Adding downloadable content")
        print(f"{copy_dlc(game, dlc)} downloadable content pack(s) added.", flush=True)
    os.makedirs(os.path.join(game, "DLC"), exist_ok=True)

    progress(24, "Translating the game's program (this takes a few minutes)")
    work = os.path.join(game, "_build")
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    log_path = os.path.join(work, "translate.log")
    try:
        with open(log_path, "w", encoding="utf-8") as log:
            build_game.generate(os.path.join(game, "data", "default.xbe"), work, log=log)
    except subprocess.CalledProcessError:
        raise SetupError(f"Translating the game's program failed. Details are in {log_path}")
    progress(45, "Compiling the game")
    compile_game(game, work)
    shutil.copy2(os.path.join(HERE, "shaderc_shared.dll"), game)
    try:
        make_icon(game)
    except Exception as e:                  # a missing icon shouldn't lose a finished install
        print(f"No icon was made: {e}", flush=True)
    if android:                             # the Windows game is finished: a problem here can't lose it
        try:
            build_android(game, work, ndk)
        except (SetupError, android_build.BuildError, OSError) as e:
            print(f"The Windows game is ready, but the Android app couldn't be built: {e}", flush=True)
    shutil.rmtree(work)
    shutil.copyfile(os.path.join(HERE, "version.txt"), os.path.join(game, "kit_version.txt"))
    progress(100, "Done")


def check_source(source):
    try:
        size = inspect_source(source)
    except (SetupError, OSError) as e:
        print(f"@R 0 {e}", flush=True)
        return
    print(f"@R 1 Good: this is the USA Xbox release ({size / (1 << 30):.1f} GB).", flush=True)


def check_dlc(folders):
    packs = all_packs(folders)
    if not packs:
        print("@R 0 No DLC packs found there." if folders else "@R 1 ", flush=True)
        return
    names = ", ".join(OFFER_NAMES.get(o, f"pack {o:08X}") for o in sorted(packs))
    files = sum(len(folder_files(p)) for p in packs.values())
    print(f"@R 1 {len(packs)} DLC pack{'s' if len(packs) != 1 else ''} found, {files} files: {names}.", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source")
    parser.add_argument("--dest", default=os.path.join(INSTALL, "game"))
    parser.add_argument("--dlc", action="append", default=[])
    parser.add_argument("--android", action="store_true", help="also build an APK (the player has accepted Google's terms)")
    parser.add_argument("--ndk", help="an Android NDK already on this PC, instead of downloading one")
    parser.add_argument("--rebuild", action="store_true", help="compile again from the game files already installed")
    parser.add_argument("--check-source")
    parser.add_argument("--check-dlc", action="append")
    args = parser.parse_args()
    if args.check_source:
        check_source(args.check_source)
        return 0
    if args.check_dlc is not None:
        check_dlc(args.check_dlc)
        return 0
    if not args.source and not args.rebuild:
        parser.error("--source is required")
    try:
        install(args.source, os.path.abspath(args.dest), args.dlc, args.android, args.ndk, args.rebuild)
    except (SetupError, OSError, subprocess.CalledProcessError) as e:
        print(f"Setup stopped: {e}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
