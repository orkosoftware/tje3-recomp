"""Lift default.xbe and build the recompiled game (build/game/tje3.exe).

Usage (emu-venv): python -I build_game.py <default.xbe> [--no-lift]

1. lift.py: every GAME / LIBC / XAPI / CRI / XGRAPHIC / UNKNOWN function into
   build/recomp_all (D3D8, DSOUND and XPP functions stay external: HLE);
2. gen_hle_stubs.py: default stubs for externs without a hand-written HLE;
3. CMake + Ninja with clang.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
LIFT_LABELS = {"GAME", "LIBC", "XAPI", "CRI", "XGRAPHIC", "UNKNOWN"}
HLE_LABELS = {"D3D8", "DSOUND", "XPP"}
# Library functions simple enough to lift as they are (no hardware access).
LIFT_ANYWAY = {"0016b490", "0016b3a0"}


def generate(xbe, out, lift=True, log=None):
    """Steps 1 and 2: write the lifted C++ and the HLE stubs into out (tool output to log, if given)."""
    os.makedirs(out, exist_ok=True)
    lift_list, externs = [], []
    for line in open(os.path.join(ROOT, "analysis", "function_map.tsv"), encoding="utf-8").read().splitlines()[1:]:
        addr, _size, sec, label, evidence = line.split("\t")[:5]
        # HLE only real library code: in a library section, or identified by name.
        # Heuristically labelled functions in .text are lifted.
        hle = label in HLE_LABELS and (sec != ".text" or evidence == "name")
        if hle and addr not in LIFT_ANYWAY:
            externs.append(addr)
        elif label in LIFT_LABELS or label in HLE_LABELS or addr in LIFT_ANYWAY:
            lift_list.append(addr)
    import re
    hle_dir = os.path.join(ROOT, "runtime", "hle")
    wraps = []
    for f in os.listdir(hle_dir):                  # hand-written HLE replaces lifting
        if f.endswith(".cpp"):
            text = open(os.path.join(hle_dir, f)).read()
            for a in re.findall(r"void hle_([0-9a-f]{8})\(Cpu\* c\)", text):
                if a in lift_list:
                    lift_list.remove(a)
                if a not in externs:
                    externs.append(a)
            wraps += re.findall(r"void wrap_([0-9a-f]{8})\(Cpu\* c\)", text)   # wrap_: runs around the lifted code
    with open(os.path.join(out, "lift_list.txt"), "w") as f:
        f.write("\n".join(lift_list) + "\n")
    with open(os.path.join(out, "extern_list.txt"), "w") as f:
        f.write(",".join(externs) + "\n")
    py = sys.executable
    if lift:
        subprocess.run([py, "-I", os.path.join(HERE, "lift.py"), xbe, out, "@" + os.path.join(out, "lift_list.txt"),
                        "--extern", ",".join(externs), "--wrap", ",".join(wraps)], check=True, stdout=log, stderr=log)
    subprocess.run([py, "-I", os.path.join(HERE, "gen_hle_stubs.py"), xbe, os.path.join(out, "extern_list.txt"),
                    os.path.join(out, "hle_libs_generated.cpp"), os.path.join(ROOT, "runtime", "hle")],
                   check=True, stdout=log, stderr=log)


def main():
    generate(os.path.abspath(sys.argv[1]), os.path.join(ROOT, "build", "recomp_all"), "--no-lift" not in sys.argv)
    bdir = os.path.join(ROOT, "build", "game")
    if not os.path.exists(os.path.join(bdir, "build.ninja")):
        subprocess.run(["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja", "-DCMAKE_CXX_COMPILER=clang++",
                        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"], check=True)
    subprocess.run(["cmake", "--build", bdir], check=True)


if __name__ == "__main__":
    main()
