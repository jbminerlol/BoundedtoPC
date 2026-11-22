"""Reassemble ebsrc (US) with ca65/ld65 and check it matches the user's ROM byte for byte.

usage: python -I tools/build_ebsrc.py [--jobs N]
Output: build/ebsrc/earthbound.sfc, earthbound.dbg (ld65 debug info: symbols, line spans),
earthbound.map, and per-bank listings in build/ebsrc/US/*.lst.
"""
import concurrent.futures
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ROOT = os.path.dirname(PORT)
EBSRC = os.path.join(ROOT, "ebsrc-main")
ROM = os.path.join(ROOT, "og roms", "EarthBound (USA).sfc")
TOOLS = os.environ.get("EB_TOOLS", r"F:\AI BS\tools")
CA65 = os.path.join(TOOLS, "cc65", "bin", "ca65.exe")
LD65 = os.path.join(TOOLS, "cc65", "bin", "ld65.exe")
OUT = os.path.join(PORT, "build", "ebsrc")
US_SHA1 = "d67a8ef36ef616bc39306aa1b486e1bd3047815a"


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def main():
    jobs = 2
    if "--jobs" in sys.argv:
        jobs = int(sys.argv[sys.argv.index("--jobs") + 1])
    os.makedirs(os.path.join(OUT, "US"), exist_ok=True)
    if not os.path.exists(os.path.join(OUT, "main.spc700.bin")):
        code, out = run([sys.executable, "-I", os.path.join(HERE, "extract.py"), EBSRC, ROM, OUT])
        print(out.strip())
        if code:
            sys.exit("extract failed")
    bankdir = os.path.join(EBSRC, "src", "bankconfig", "US")
    banks = sorted(f for f in os.listdir(bankdir) if f.endswith(".asm"))
    flags = ["-t", "none", "--cpu", "65816", "-g",
             "--include-dir", os.path.join(EBSRC, "src"),
             "--include-dir", os.path.join(EBSRC, "include"),
             "--include-dir", OUT, "--bin-include-dir", OUT, "-D", "USA"]

    def assemble(name):
        stem = name[:-4]
        obj = os.path.join(OUT, "US", stem + ".o")
        lst = os.path.join(OUT, "US", stem + ".lst")
        code, out = run([CA65] + flags + ["--listing", lst, "-o", obj, os.path.join(bankdir, name)])
        return name, code, out, obj

    objs = []
    failed = False
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for name, code, out, obj in ex.map(assemble, banks):
            if code or out.strip():
                print(f"--- {name} (exit {code})\n{out.strip()[:3000]}")
            if code:
                failed = True
            objs.append(obj)
    if failed:
        sys.exit("assembly failed")
    sfc = os.path.join(OUT, "earthbound.sfc")
    code, out = run([LD65, "-C", os.path.join(EBSRC, "snes.cfg"),
                     "--dbgfile", os.path.join(OUT, "earthbound.dbg"),
                     "--mapfile", os.path.join(OUT, "earthbound.map"),
                     "-o", sfc] + objs)
    if out.strip():
        print(out.strip()[:3000])
    if code:
        sys.exit("link failed")
    with open(sfc, "rb") as f:
        built = f.read()
    with open(ROM, "rb") as f:
        rom = f.read()
    sha = hashlib.sha1(built).hexdigest()
    if built == rom:
        print(f"OK: {sfc} matches the ROM ({sha})")
        return
    diffs = [i for i in range(min(len(built), len(rom))) if built[i] != rom[i]]
    print(f"MISMATCH: sha1 {sha}, size {len(built)} vs {len(rom)}, {len(diffs)} differing bytes")
    for i in diffs[:20]:
        print(f"  {i:06X}: built {built[i]:02X} rom {rom[i]:02X}")
    sys.exit(1)


if __name__ == "__main__":
    main()
