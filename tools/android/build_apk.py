"""Build the Android APK by hand, from the C++ that build_game.py generated.

Usage: python build_apk.py <lifted dir> <out dir> --sdk <Android SDK> --shaderc <dir of built .a files>
                           --vulkan-include <dir> --xbe <default.xbe>

Players get the same thing through setup. The APK holds the game's translated
code, so it must never be shared.
"""
import argparse
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path[:0] = [HERE, os.path.join(ROOT, "tools"), os.path.join(ROOT, "tools", "emu")]
import android_build  # noqa: E402
from xbe_image import XbeImage  # noqa: E402
import xbox_texture  # noqa: E402


def newest(pattern):
    return sorted(glob.glob(pattern))[-1]


def main():
    a = argparse.ArgumentParser()
    a.add_argument("lifted")
    a.add_argument("out")
    a.add_argument("--sdk", required=True)
    a.add_argument("--shaderc", required=True)
    a.add_argument("--vulkan-include", required=True)
    a.add_argument("--xbe", required=True, help="the player's default.xbe, for the icon")
    args = a.parse_args()
    ndk = newest(os.path.join(args.sdk, "ndk", "*"))
    build_tools = newest(os.path.join(args.sdk, "build-tools", "*"))
    android_jar = os.path.join(newest(os.path.join(args.sdk, "platforms", "android-*")), "android.jar")
    libs = sorted(glob.glob(os.path.join(args.shaderc, "**", "arm64-v8a", "*.a"), recursive=True))
    includes = [os.path.join(ndk, "sources", "third_party", "shaderc", "libshaderc", "include"), args.vulkan_include]
    os.makedirs(args.out, exist_ok=True)
    so = android_build.compile_library(ndk, args.lifted, ROOT, includes, libs, args.out)

    size, rgba = xbox_texture.title_image(XbeImage(args.xbe))
    icon = os.path.join(args.out, "icon.png")
    xbox_texture.write_png(icon, size, size, rgba)
    skeleton = android_build.make_skeleton(os.path.join(build_tools, "aapt2.exe"), android_jar, args.out,
                                           lambda p: xbox_texture.write_png(p, 1, 1, bytes(4)))
    with open(icon, "rb") as f:
        png = f.read()
    key = android_build.load_key(os.path.join(args.out, "signing_key.json"))
    apk = android_build.package(skeleton, so, png, key, os.path.join(args.out, "tje3-test.apk"))
    print(f"{apk}: {os.path.getsize(apk) >> 20} MB")


if __name__ == "__main__":
    main()
