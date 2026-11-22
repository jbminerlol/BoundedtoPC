"""Turn ld65's earthbound.dbg into a compact program database for the recompiler.

usage: python -I tools/dbginfo.py [dbg] [out.pickle]

The database (pickle) holds:
  insns:  sorted list of (addr24, size, file_id, line_no, macro)   one per assembled instruction
  files:  {file_id: path}
  syms:   list of (name, addr24, kind, parent_name, data_ref, code_ref)
            kind: 'lab' global label, 'local' cheap @label, 'equ'
            data_ref: referenced from a data line (pointer tables), code_ref: from an instruction
  datas:  sorted list of (addr24, size) for typed (data) spans in ROM
"""
import os
import pickle
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
DEFAULT_DBG = os.path.join(PORT, "build", "ebsrc", "earthbound.dbg")
DEFAULT_OUT = os.path.join(PORT, "build", "ebsrc", "program.pickle")

MNEMONICS = set("""
ADC AND ASL BCC BCS BEQ BIT BMI BNE BPL BRA BRK BRL BVC BVS CLC CLD CLI CLV CMP COP CPX CPY
DEC DEX DEY EOR INC INX INY JML JMP JSL JSR LDA LDX LDY LSR MVN MVP NOP ORA PEA PEI PER PHA
PHB PHD PHK PHP PHX PHY PLA PLB PLD PLP PLX PLY REP ROL ROR RTI RTL RTS SBC SEC SED SEI SEP
STA STP STX STY STZ TAX TAY TCD TCS TDC TRB TSB TSC TSX TXA TXS TXY TYA TYX WAI WDM XBA XCE
""".split())

KV = re.compile(r"(\w+)=(\"[^\"]*\"|[^,]*)")


def parse_kv(rest):
    return {k: v for k, v in KV.findall(rest)}


def main():
    dbg = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DBG
    out = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    segs = {}
    spans = {}
    files = {}
    lines = {}       # line id -> (file, line, type, [span ids])
    syms = []
    with open(dbg, encoding="latin-1") as f:
        for raw in f:
            tag, _, rest = raw.rstrip("\n").partition("\t")
            if tag == "span":
                d = parse_kv(rest)
                spans[int(d["id"])] = (int(d["seg"]), int(d["start"]), int(d["size"]), "type" in d)
            elif tag == "line":
                d = parse_kv(rest)
                sp = [int(x) for x in d["span"].split("+")] if "span" in d else []
                lines[int(d["id"])] = (int(d["file"]), int(d["line"]), int(d.get("type", 0)), sp)
            elif tag == "sym":
                syms.append(parse_kv(rest))
            elif tag == "seg":
                d = parse_kv(rest)
                segs[int(d["id"])] = (d["name"].strip('"'), int(d["start"], 16), int(d["size"], 16),
                                      "oname" in d)
            elif tag == "file":
                d = parse_kv(rest)
                files[int(d["id"])] = d["name"].strip('"')
    print(f"segs={len(segs)} spans={len(spans)} lines={len(lines)} syms={len(syms)} files={len(files)}")

    # Source text cache (to tell instructions from untyped directives such as .RES/.INCBIN).
    text_cache = {}

    def source_line(fid, ln):
        t = text_cache.get(fid)
        if t is None:
            try:
                with open(files[fid], encoding="latin-1") as fh:
                    t = fh.read().split("\n")
            except OSError:
                t = []
            text_cache[fid] = t
        return t[ln - 1] if 0 < ln <= len(t) else ""

    def first_token(text):
        s = text.split(";", 1)[0].strip()
        # strip a leading label "NAME:" or "@NAME:"
        m = re.match(r"^[@A-Za-z_][\w@]*:\s*(.*)$", s)
        if m:
            s = m.group(1).strip()
        return s.split(None, 1)[0].upper() if s else ""

    span_lines = {}  # span id -> [(file, line, macro)]
    line_is_data = {}
    for lid, (fid, ln, ltype, sp) in lines.items():
        typed = any(spans[s][3] for s in sp if s in spans)
        line_is_data[lid] = typed
        for s in sp:
            span_lines.setdefault(s, []).append((fid, ln, ltype == 2))

    insns = []
    datas = []
    rejected = {}
    for sid, (seg, start, size, typed) in spans.items():
        sname, sstart, ssize, in_rom = segs[seg]
        if not in_rom:
            continue
        addr = sstart + start
        if typed:
            datas.append((addr, size))
            continue
        infos = span_lines.get(sid)
        if not infos:
            rejected["noline"] = rejected.get("noline", 0) + 1
            continue
        # An instruction if any of its source lines (the macro body line for macro expansions)
        # is a mnemonic. Attribute it to the outermost (non-macro) line for function grouping.
        is_insn = any(first_token(source_line(fid, ln)) in MNEMONICS for fid, ln, _m in infos)
        outer = [i for i in infos if not i[2]]
        fid, ln, macro = (outer or infos)[0]
        if is_insn and 1 <= size <= 4:
            insns.append((addr, size, fid, ln, not outer))
        else:
            datas.append((addr, size))
            tok = first_token(source_line(fid, ln))
            rejected[tok] = rejected.get(tok, 0) + 1
    insns.sort()
    # overlapping/duplicate instruction spans would mean two lines share bytes
    dup = sum(1 for a, b in zip(insns, insns[1:]) if b[0] < a[0] + a[1])
    # Macro invocation lines get one span over their whole expansion; drop untyped spans whose
    # bytes are all instruction bytes, report ones that are only partly covered.
    code_bytes = set()
    for a, s, *_ in insns:
        code_bytes.update(range(a, a + s))
    kept = []
    partial = []
    for a, s in datas:
        n = sum(1 for x in range(a, a + s) if x in code_bytes)
        if n == 0:
            kept.append((a, s))
        elif n < s:
            partial.append((a, s, n))
    datas = sorted(set(kept))
    print(f"instructions={len(insns)} data spans={len(datas)} overlaps={dup} "
          f"partly-code spans={len(partial)}")
    for a, s, n in partial[:10]:
        print(f"  partly code: {a:06X} size {s} ({n} code bytes)")
    print("untyped non-instruction spans by first token:",
          sorted(rejected.items(), key=lambda kv: -kv[1])[:15])

    sym_by_id = {int(d["id"]): d for d in syms}
    out_syms = []
    for d in syms:
        if d.get("type") not in ("lab", "equ") or "val" not in d:
            continue
        name = d["name"].strip('"')
        val = int(d["val"], 16)
        kind = d["type"]
        parent = None
        if name.startswith("@"):
            kind = "local"
            p = sym_by_id.get(int(d["parent"])) if "parent" in d else None
            parent = p["name"].strip('"') if p else None
        refs = [int(x) for x in d["ref"].split("+")] if d.get("ref") else []
        data_ref = any(line_is_data.get(r, False) for r in refs)
        code_ref = any(not line_is_data.get(r, True) for r in refs)
        out_syms.append((name, val, kind, parent, data_ref, code_ref))
    print(f"symbols kept={len(out_syms)} "
          f"(labels={sum(1 for s in out_syms if s[2] == 'lab')}, "
          f"locals={sum(1 for s in out_syms if s[2] == 'local')})")
    with open(out, "wb") as f:
        pickle.dump({"insns": insns, "files": files, "syms": out_syms, "datas": datas,
                     "segs": segs}, f, protocol=pickle.HIGHEST_PROTOCOL)
    print("wrote", out)


if __name__ == "__main__":
    main()
