"""Extract ebsrc's binary assets from the user's ROM (replacement for ebbinex).

usage: python -I tools/extract.py <ebsrc dir> <rom.sfc> <out dir> [yml name]

Writes <out>/data/<subdir>/<name>.<ext> for every dumpEntries item of earthbound.yml, with the
original ROM bytes. Compressed entries are written as <name>.<ext>.lzhal (the bytes exactly as
they sit in the ROM), so ca65 includes them unchanged and no recompressor is needed.
Also writes <out>/main.spc700.bin (the SPC700 driver image ebsrc builds with spcasm) from the
copy of the driver inside audio pack 1.
"""
import hashlib
import os
import sys

US_SHA1 = "d67a8ef36ef616bc39306aa1b486e1bd3047815a"


def parse_dump_entries(path):
    entries = []
    cur = None
    in_dump = False
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.startswith(" ") and not line.startswith("-"):
                in_dump = line.startswith("dumpEntries:")
                continue
            if not in_dump:
                continue
            s = line.strip()
            if s.startswith("- "):
                cur = {}
                entries.append(cur)
                s = s[2:]
            if ":" not in s or cur is None:
                continue
            key, val = s.split(":", 1)
            val = val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in "'\"":
                val = val[1:-1]
            cur[key.strip()] = val
    return entries


def parse_label_maps(path):
    """renameLabels: {block: {offset: name}} and flyoverLabels: {address: name}."""
    rename = {}
    flyover = {}
    section = None
    block = None
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip():
                continue
            indent = len(line) - len(line.lstrip(" "))
            s = line.strip()
            if s.startswith("#"):
                continue
            if " #" in s:
                s = s.split(" #", 1)[0].rstrip()
            if indent == 0:
                section = s.rstrip(":") if s.endswith(":") else None
                block = None
                continue
            if section == "flyoverLabels" and indent == 2:
                k, v = s.split(":", 1)
                flyover[int(k, 0)] = v.strip()
            elif section == "renameLabels":
                if indent == 2 and s.endswith(":"):
                    block = s[:-1].strip().strip("'\"")
                    rename[block] = {}
                elif indent == 4 and block is not None:
                    k, v = s.split(":", 1)
                    rename[block][int(k, 0)] = v.strip().strip("'\"")
    return rename, flyover


def labelled_source(data, labels, header):
    """ca65 source that assembles back to `data`, with `labels` ({offset: name}) in place."""
    lines = [f"; {header}"]
    cuts = sorted(set(o for o in labels if 0 <= o <= len(data)) | {0, len(data)})
    for a, b in zip(cuts, cuts[1:] + [None]):
        if a in labels:
            lines.append(f"{labels[a]}:")
        if b is None:
            break
        for i in range(a, b, 32):
            chunk = data[i:min(i + 32, b)]
            lines.append("\t.BYTE " + ",".join(f"${x:02X}" for x in chunk))
    return "\n".join(lines) + "\n"


def symbols_source(names):
    return "".join(f".GLOBAL {n}: far\n" for n in names)


def main():
    src, rom_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
    yml = sys.argv[4] if len(sys.argv) > 4 else "earthbound.yml"
    with open(rom_path, "rb") as f:
        rom = f.read()
    sha = hashlib.sha1(rom).hexdigest()
    if yml == "earthbound.yml" and sha != US_SHA1:
        sys.exit(f"ROM SHA-1 {sha} is not EarthBound (USA) {US_SHA1}")
    entries = parse_dump_entries(os.path.join(src, yml))
    rename, flyover_labels = parse_label_maps(os.path.join(src, yml))
    count = 0
    total = 0
    for e in entries:
        off = int(e["offset"], 0)
        size = int(e["size"], 0)
        ext = e["extension"]
        name = f"{e['name']}.{ext}"
        if e.get("compressed", "false") == "true":
            name += ".lzhal"
        path = os.path.join(out, "data", e["subdir"], name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data = rom[off:off + size]
        if len(data) != size:
            sys.exit(f"entry {e} runs past the end of the ROM")
        if ext == "ebtxt":
            # Text scripts: ebbinex disassembles them; raw bytes with the named labels at
            # their offsets assemble to the same ROM bytes.
            labels = rename.get(e["name"], {})
            with open(path, "w", newline="\n") as f:
                f.write(labelled_source(data, labels, f"{e['name']}: generated from ROM"))
            sym = os.path.join(out, "data", e["subdir"], e["name"] + ".symbols.asm")
            with open(sym, "w", newline="\n") as f:
                f.write(symbols_source(labels[o] for o in sorted(labels)))
        elif ext == "flyover":
            base = 0xC00000 + off  # HiROM: ROM offset -> $C0:0000 + offset
            labels = {a - base: n for a, n in flyover_labels.items() if base <= a < base + size}
            with open(path, "w", newline="\n") as f:
                # bank21 does not include flyovers.symbols.asm, so export the labels here.
                f.write(symbols_source(labels[o] for o in sorted(labels)))
                f.write(labelled_source(data, labels, f"{e['name']}: generated from ROM"))
            if labels:
                sym = os.path.join(out, "data", e["subdir"], e["name"] + ".symbols.asm")
                with open(sym, "w", newline="\n") as f:
                    f.write(symbols_source(labels[o] for o in sorted(labels)))
        else:
            with open(path, "wb") as f:
                f.write(data)
        count += 1
        total += size
    # main.spc700.bin: spcasm "plain" image from address 0; the ROM carries it from $0500 as the
    # third sub-pack of audio pack 1 ($0500 bytes of header/sub-packs precede it in the pack).
    with open(os.path.join(out, "data", "audiopacks", "1.ebm"), "rb") as f:
        pack = f.read()
    pos = 0
    pieces = []
    while True:
        length = pack[pos] | (pack[pos + 1] << 8)
        if length == 0:
            break
        dest = pack[pos + 2] | (pack[pos + 3] << 8)
        pieces.append((dest, pos + 4, length))
        pos += 4 + length
    dest, start, length = [p for p in pieces if p[0] == 0x0500][0]
    driver_len = 0x2B1F - start  # bank26.asm: driver bytes, then 1.ebm from $2B1F
    image = bytes(0x500) + pack[start:start + driver_len]
    with open(os.path.join(out, "main.spc700.bin"), "wb") as f:
        f.write(image)
    print(f"extracted {count} assets ({total} bytes); main.spc700.bin {len(image)} bytes "
          f"(driver {driver_len:#x} at pack offset {start:#x}, sub-pack length {length:#x})")


if __name__ == "__main__":
    main()
