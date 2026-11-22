"""EarthBound 65816 -> C static recompiler.

usage: python -I tools/recomp.py [--report]

Inputs : build/ebsrc/program.pickle (tools/dbginfo.py) and the user's ROM.
Outputs: build/gen/eb_gen_NN.c, eb_gen.h (prototypes), eb_dispatch.c, recomp_report.txt

Design (see README "How the port works"):
  * ebsrc's debug info gives every instruction's address and length, so M/X widths of immediates
    are exact; other instructions get M/X from a per-function data-flow analysis (unknown -> code
    that tests cpu.M / cpu.XF at run time).
  * A C function per entry point (every global label at an instruction, plus labels used in
    pointer tables). Its body is all code reachable inside the same ebsrc source file; flow into
    another file becomes a tail call. Branches are gotos.
  * The emulated stack holds real return addresses (stack-relative code keeps working); RTS/RTL
    just drop them.
  * Flags are computed only where a later instruction can read them (backward liveness).
  * Every basic block adds its cycles to cpu.cyc; back edges call POLL() so the runtime can run
    the NMI (a real NMI may arrive between any two instructions) and present a frame.
"""
import collections
import os
import pickle
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ROOT = os.path.dirname(PORT)
DB = os.path.join(PORT, "build", "ebsrc", "program.pickle")
ROM = os.path.join(ROOT, "og roms", "EarthBound (USA).sfc")
OUT = os.path.join(PORT, "build", "gen")

# --------------------------------------------------------------------------------------------
# Opcode table
# --------------------------------------------------------------------------------------------
_TABLE = """
BRK imm8|ORA idpx|COP imm8|ORA sr|TSB dp|ORA dp|ASL dp|ORA ildp|PHP imp|ORA immM|ASL acc|PHD imp|TSB abs|ORA abs|ASL abs|ORA long
BPL rel|ORA idpy|ORA idp|ORA isry|TRB dp|ORA dpx|ASL dpx|ORA ildpy|CLC imp|ORA absy|INC acc|TCS imp|TRB abs|ORA absx|ASL absx|ORA longx
JSR abs|AND idpx|JSL long|AND sr|BIT dp|AND dp|ROL dp|AND ildp|PLP imp|AND immM|ROL acc|PLD imp|BIT abs|AND abs|ROL abs|AND long
BMI rel|AND idpy|AND idp|AND isry|BIT dpx|AND dpx|ROL dpx|AND ildpy|SEC imp|AND absy|DEC acc|TSC imp|BIT absx|AND absx|ROL absx|AND longx
RTI imp|EOR idpx|WDM imm8|EOR sr|MVP blk|EOR dp|LSR dp|EOR ildp|PHA imp|EOR immM|LSR acc|PHK imp|JMP abs|EOR abs|LSR abs|EOR long
BVC rel|EOR idpy|EOR idp|EOR isry|MVN blk|EOR dpx|LSR dpx|EOR ildpy|CLI imp|EOR absy|PHY imp|TCD imp|JML long|EOR absx|LSR absx|EOR longx
RTS imp|ADC idpx|PER rel16|ADC sr|STZ dp|ADC dp|ROR dp|ADC ildp|PLA imp|ADC immM|ROR acc|RTL imp|JMP iabs|ADC abs|ROR abs|ADC long
BVS rel|ADC idpy|ADC idp|ADC isry|STZ dpx|ADC dpx|ROR dpx|ADC ildpy|SEI imp|ADC absy|PLY imp|TDC imp|JMP iabsx|ADC absx|ROR absx|ADC longx
BRA rel|STA idpx|BRL rel16|STA sr|STY dp|STA dp|STX dp|STA ildp|DEY imp|BIT immM|TXA imp|PHB imp|STY abs|STA abs|STX abs|STA long
BCC rel|STA idpy|STA idp|STA isry|STY dpx|STA dpx|STX dpy|STA ildpy|TYA imp|STA absy|TXS imp|TXY imp|STZ abs|STA absx|STZ absx|STA longx
LDY immX|LDA idpx|LDX immX|LDA sr|LDY dp|LDA dp|LDX dp|LDA ildp|TAY imp|LDA immM|TAX imp|PLB imp|LDY abs|LDA abs|LDX abs|LDA long
BCS rel|LDA idpy|LDA idp|LDA isry|LDY dpx|LDA dpx|LDX dpy|LDA ildpy|CLV imp|LDA absy|TSX imp|TYX imp|LDY absx|LDA absx|LDX absy|LDA longx
CPY immX|CMP idpx|REP imm8|CMP sr|CPY dp|CMP dp|DEC dp|CMP ildp|INY imp|CMP immM|DEX imp|WAI imp|CPY abs|CMP abs|DEC abs|CMP long
BNE rel|CMP idpy|CMP idp|CMP isry|PEI dp|CMP dpx|DEC dpx|CMP ildpy|CLD imp|CMP absy|PHX imp|STP imp|JML ilabs|CMP absx|DEC absx|CMP longx
CPX immX|SBC idpx|SEP imm8|SBC sr|CPX dp|SBC dp|INC dp|SBC ildp|INX imp|SBC immM|NOP imp|XBA imp|CPX abs|SBC abs|INC abs|SBC long
BEQ rel|SBC idpy|SBC idp|SBC isry|PEA imm16|SBC dpx|INC dpx|SBC ildpy|SED imp|SBC absy|PLX imp|XCE imp|JSR iabsx|SBC absx|INC absx|SBC longx
"""
OPS = []
for row in _TABLE.strip().split("\n"):
    for cell in row.split("|"):
        mn, mode = cell.split()
        OPS.append((mn, mode))
assert len(OPS) == 256

MODE_SIZE = {"imp": 1, "acc": 1, "imm8": 2, "imm16": 3, "dp": 2, "dpx": 2, "dpy": 2, "idp": 2,
             "idpx": 2, "idpy": 2, "ildp": 2, "ildpy": 2, "sr": 2, "isry": 2, "rel": 2, "rel16": 3,
             "abs": 3, "absx": 3, "absy": 3, "long": 4, "longx": 4, "iabs": 3, "iabsx": 3,
             "ilabs": 3, "blk": 3}

A_WIDTH = set("ORA AND EOR ADC STA LDA CMP SBC BIT TSB TRB STZ PHA PLA TXA TYA".split())
A_RMW = set("ASL LSR ROL ROR INC DEC".split())        # memory or accumulator, M width
X_WIDTH = set("LDX LDY STX STY CPX CPY INX INY DEX DEY PHX PHY PLX PLY TAX TAY TSX TXY TYX".split())
BRANCH = {"BPL": ("N", 0), "BMI": ("N", 1), "BVC": ("V", 0), "BVS": ("V", 1),
          "BCC": ("C", 0), "BCS": ("C", 1), "BNE": ("Z", 0), "BEQ": ("Z", 1)}
FLAGS = ("N", "V", "Z", "C")


class Insn:
    __slots__ = ("addr", "op", "mn", "mode", "size", "arg", "file", "line", "macro", "group",
                 "m_ev", "x_ev", "target", "tbank")

    def __repr__(self):
        return f"{self.addr:06X} {self.mn} {self.mode}"


def bank_of(a):
    return a >> 16


def canon(a):
    """Canonical ROM address of a code address ($80:9470 -> $C0:9470), or None if not ROM."""
    bank, off = a >> 16, a & 0xFFFF
    if bank >= 0xC0:
        return a
    if 0x40 <= bank <= 0x7D:
        return ((0xC0 | (bank & 0x3F)) << 16) | off
    if (bank <= 0x3F or 0x80 <= bank <= 0xBF) and off >= 0x8000:
        return ((0xC0 | (bank & 0x3F)) << 16) | off
    return None


# --------------------------------------------------------------------------------------------
# Front end
# --------------------------------------------------------------------------------------------
def load():
    with open(DB, "rb") as f:
        db = pickle.load(f)
    with open(ROM, "rb") as f:
        rom = f.read()
    return db, rom


def rom_byte(rom, addr):
    # HiROM: $C0-$FF banks map linearly; ebsrc code lives at $C0:0000 and up.
    return rom[(addr - 0xC00000) & 0x3FFFFF]


def decode(db, rom, problems):
    insns = {}
    for addr, size, fid, ln, macro in db["insns"]:
        i = Insn()
        i.addr, i.size, i.file, i.line, i.macro = addr, size, fid, ln, macro
        i.op = rom_byte(rom, addr)
        i.mn, i.mode = OPS[i.op]
        i.m_ev = i.x_ev = None
        i.target = None
        i.tbank = None
        want = MODE_SIZE.get(i.mode)
        if i.mode == "immM":
            if size not in (2, 3):
                problems.append(f"{addr:06X} {i.mn} immM size {size}")
            i.m_ev = 1 if size == 2 else 0
        elif i.mode == "immX":
            if size not in (2, 3):
                problems.append(f"{addr:06X} {i.mn} immX size {size}")
            i.x_ev = 1 if size == 2 else 0
        elif want != size:
            problems.append(f"{addr:06X} {i.mn} {i.mode}: dbg size {size} != {want}")
        arg = 0
        for k in range(1, size):
            arg |= rom_byte(rom, addr + k) << (8 * (k - 1))
        i.arg = arg
        if i.mode == "rel":
            off = arg - 256 if arg >= 128 else arg
            i.target = (addr & 0xFF0000) | ((addr + 2 + off) & 0xFFFF)
        elif i.mode == "rel16":
            off = arg - 65536 if arg >= 32768 else arg
            i.target = (addr & 0xFF0000) | ((addr + 3 + off) & 0xFFFF)
        elif i.mn in ("JMP", "JSR") and i.mode == "abs":
            i.target = (addr & 0xFF0000) | arg
        elif i.mn in ("JML", "JSL") and i.mode == "long":
            i.tbank = arg >> 16
            i.target = canon(arg)
            if i.target is None:
                problems.append(f"{addr:06X} {i.mn} to non-ROM {arg:06X}")
        insns[addr] = i
    # ebsrc writes some 16-bit compares as a 2-byte immediate + "BRK" so the bytes match:
    # merge them back into one 3-byte instruction.
    for addr in sorted(insns):
        i = insns.get(addr)
        if i is None or i.mode not in ("immM", "immX") or i.size != 2:
            continue
        nxt = insns.get(addr + 2)
        if nxt is not None and nxt.mn == "BRK" and nxt.size == 1:
            i.size = 3
            i.arg |= rom_byte(rom, addr + 2) << 8
            if i.mode == "immM":
                i.m_ev = 0
            else:
                i.x_ev = 0
            del insns[addr + 2]
            problems[:] = [p for p in problems if not p.startswith(f"{addr + 2:06X} BRK")]
    # group = source file; macro-only instructions take their neighbour's file
    order = sorted(insns)
    prev = None
    for a in order:
        i = insns[a]
        if not i.macro:
            i.group = i.file
        elif prev is not None and prev.addr + prev.size == a:
            i.group = prev.group
        else:
            i.group = None
        prev = i
    for a in reversed(order):
        i = insns[a]
        if i.group is None:
            nxt = insns.get(a + i.size)
            i.group = nxt.group if nxt is not None and nxt.group is not None else ("m", a)
    return insns


UNCOND = set("BRA BRL JMP JML RTS RTL RTI STP".split())


def flow_successors(i):
    """Intra-procedural successors (not following calls)."""
    out = []
    if i.mn in BRANCH:
        out.append(i.target)
        out.append(i.addr + i.size)
    elif i.mn in ("BRA", "BRL"):
        out.append(i.target)
    elif i.mn == "JMP" and i.mode == "abs":
        out.append(i.target)
    elif i.mn == "JML" and i.mode == "long":
        out.append(i.target)
    elif i.mn in UNCOND or (i.mn in ("JMP", "JML")):
        pass
    else:
        out.append(i.addr + i.size)
    return out


class Func:
    def __init__(self, entry, name):
        self.entry = entry
        self.name = name
        self.body = []         # sorted instruction addresses
        self.tails = set()     # addresses jumped to in other functions
        self.jt_targets = []   # jump-table candidates (for JMP (abs,X) / JMP (abs) switches)
        self.state = {}        # addr -> (m, x) at instruction start
        self.depth = {}        # addr -> stack depth at instruction start
        self.exit_mx = None    # (m, x) at returns: 0/1/'E'/'T'
        self.live = {}         # addr -> set of flags live after the instruction
        self.anomalies = []
        self.conts = set()     # PEA'd return points (code reached by "PEA ret-1 ... RTS")
        self.entry_mx = (None, None)


def c_ident(name):
    out = []
    for ch in name:
        out.append(ch if ch.isalnum() or ch == "_" else "_")
    return "".join(out)


def build_functions(db, insns, problems):
    labels = collections.defaultdict(list)
    data_refd = set()
    for name, val, kind, parent, dref, cref in db["syms"]:
        if val not in insns:
            continue
        if kind == "lab":
            labels[val].append(name)
            if dref:
                data_refd.add(val)
        elif kind == "local" and dref:
            labels[val].append(f"{parent}{name.replace('@', '_')}" if parent else name[1:])
            data_refd.add(val)
    entries = {}
    for a, names in labels.items():
        good = sorted(names, key=lambda n: (n.startswith("UNKNOWN") or n.startswith("_"), n))
        entries[a] = c_ident(good[0])
    # vector entries are labels already (RESET_VECTOR etc.)
    used = collections.Counter(entries.values())
    for a in list(entries):
        if used[entries[a]] > 1:
            entries[a] = f"{entries[a]}_{a:06X}"
    by_group_data = collections.defaultdict(list)
    for a in data_refd:
        by_group_data[insns[a].group].append(a)

    funcs = {}
    pending = sorted(entries)
    while pending:
        e = pending.pop()
        if e in funcs:
            continue
        f = Func(e, entries[e])
        group = insns[e].group
        body = set()
        work = [e]
        while work:
            a = work.pop()
            if a in body:
                continue
            i = insns.get(a)
            if i is None:
                problems.append(f"{f.name}: flow into non-instruction {a:06X}")
                continue
            body.add(a)
            succ = flow_successors(i)
            if i.mn == "PEA":
                c = (a & 0xFF0000) | ((i.arg + 1) & 0xFFFF)
                ci = insns.get(c)
                if ci is not None and ci.group == group:
                    f.conts.add(c)
                    work.append(c)
            if i.mn in ("JMP", "JSR") and i.mode in ("iabsx", "iabs") or i.mn == "JML" and i.mode == "ilabs":
                if i.mn == "JMP" or i.mn == "JML":
                    f.jt_targets = sorted(set(f.jt_targets) | set(by_group_data.get(group, [])))
                    succ = succ + by_group_data.get(group, [])
            for s in succ:
                si = insns.get(s)
                if si is None:
                    problems.append(f"{f.name}: {i.addr:06X} {i.mn} flows to non-code {s:06X}")
                    continue
                if si.group == group:
                    work.append(s)
                else:
                    f.tails.add(s)
                    if s not in entries:
                        entries[s] = f"AUTO_{s:06X}"
                        pending.append(s)
        f.body = sorted(body)
        funcs[e] = f
    # call targets that are not entries yet
    for f in list(funcs.values()):
        for a in f.body:
            i = insns[a]
            if i.mn in ("JSR", "JSL") and i.target is not None and i.target not in entries:
                problems.append(f"{f.name}: call to non-entry {i.target:06X} ({i.mn} at {a:06X})")
    return funcs, entries


# --------------------------------------------------------------------------------------------
# Analyses
# --------------------------------------------------------------------------------------------
def join1(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return a if a == b else "T"


def mx_transfer(i, st, funcs, entries):
    """Return state after instruction i (for the fallthrough/branch edges)."""
    m, x, ps = st
    mn = i.mn
    if mn == "REP":
        if i.arg & 0x20:
            m = 0
        if i.arg & 0x10:
            x = 0
    elif mn == "SEP":
        if i.arg & 0x20:
            m = 1
        if i.arg & 0x10:
            x = 1
    elif mn == "PHP":
        ps = ps + ((m, x),)
    elif mn == "PLP":
        if ps:
            m, x = ps[-1]
            ps = ps[:-1]
        else:
            m = x = "T"
    elif mn == "XCE":
        m = x = "T"
    elif mn in ("JSR", "JSL"):
        callee = funcs.get(i.target) if i.target is not None else None
        if callee is not None and callee.exit_mx is not None:
            em, ex = callee.exit_mx
            m = m if em == "E" else em
            x = x if ex == "E" else ex
        else:
            m = x = "T"
    return (m, x, ps)


def analyze_mx(f, insns, funcs, entries):
    """Symbolic forward M/X analysis: 'E' = equal to the value at function entry."""
    body = set(f.body)
    state = {f.entry: ("E", "E", ())}
    for c in f.conts:
        if c != f.entry:
            state[c] = ("T", "T", ())
    work = [f.entry] + [c for c in f.conts if c != f.entry]
    while work:
        a = work.pop()
        i = insns[a]
        out = mx_transfer(i, state[a], funcs, entries)
        for s in flow_successors(i) + (f.jt_targets if i.mn in ("JMP", "JML") and i.mode in ("iabsx", "iabs", "ilabs") else []):
            if s not in body:
                continue
            old = state.get(s)
            if old is None:
                new = out
            else:
                ps = old[2] if old[2] == out[2] else ()
                new = (join1(old[0], out[0]), join1(old[1], out[1]), ps)
            if new != old:
                state[s] = new
                work.append(s)
    # resolve 'E' from immediate evidence
    em = ex = None
    conflict = False
    for a, (m, x, _ps) in state.items():
        i = insns[a]
        if i.m_ev is not None and m == "E":
            if em is None:
                em = i.m_ev
            elif em != i.m_ev:
                conflict = True
        if i.x_ev is not None and x == "E":
            if ex is None:
                ex = i.x_ev
            elif ex != i.x_ev:
                conflict = True
    if conflict:
        f.anomalies.append("conflicting width evidence at entry")
    f.entry_mx = (em, ex)
    res = {}
    for a, (m, x, ps) in state.items():
        if m == "E":
            m = em if em is not None else "E"
        if x == "E":
            x = ex if ex is not None else "E"
        res[a] = (m, x)
    f.state = res
    # exit summary (before resolution, so 'E' means "unchanged")
    em_exit = ex_exit = None
    for a, (m, x, ps) in state.items():
        if insns[a].mn in ("RTS", "RTL", "RTI"):
            em_exit = join1(em_exit, m)
            ex_exit = join1(ex_exit, x)
    new_exit = (em_exit if em_exit is not None else "T", ex_exit if ex_exit is not None else "T")
    changed = new_exit != f.exit_mx
    f.exit_mx = new_exit
    return changed


def check_evidence(f, insns, problems):
    for a in f.body:
        i = insns[a]
        m, x = f.state.get(a, ("T", "T"))
        if i.m_ev is not None and m in (0, 1) and m != i.m_ev:
            problems.append(f"{f.name}: {a:06X} {i.mn} imm says M={i.m_ev}, analysis {m}")
        if i.x_ev is not None and x in (0, 1) and x != i.x_ev:
            problems.append(f"{f.name}: {a:06X} {i.mn} imm says X={i.x_ev}, analysis {x}")


def width_of(i, mx):
    """Return the operand width for i given (m, x): 8, 16, 'M' (dynamic on M), 'X' (dynamic)."""
    m, x = mx
    if i.m_ev is not None:
        return 8 if i.m_ev else 16
    if i.x_ev is not None:
        return 8 if i.x_ev else 16
    if i.mn in A_WIDTH or (i.mn in A_RMW and i.mn not in ("INX", "INY", "DEX", "DEY")):
        return 8 if m == 1 else 16 if m == 0 else "M"
    if i.mn in X_WIDTH:
        return 8 if x == 1 else 16 if x == 0 else "X"
    return None


def stack_delta(i, mx):
    mn = i.mn
    w = width_of(i, mx)
    if mn in ("PHA", "PHX", "PHY"):
        return None if w in ("M", "X") else (1 if w == 8 else 2)
    if mn in ("PLA", "PLX", "PLY"):
        return None if w in ("M", "X") else (-1 if w == 8 else -2)
    if mn in ("PHP", "PHB", "PHK"):
        return 1
    if mn in ("PLP", "PLB"):
        return -1
    if mn in ("PHD", "PEA", "PEI", "PER"):
        return 2
    if mn == "PLD":
        return -2
    return 0


def analyze_stack(f, insns):
    body = set(f.body)
    depth = {f.entry: 0}
    for c in f.conts:
        if c != f.entry:
            depth[c] = "T"
    work = [f.entry] + [c for c in f.conts if c != f.entry]
    while work:
        a = work.pop()
        i = insns[a]
        d = depth[a]
        mx = f.state.get(a, ("T", "T"))
        if i.mn in ("TCS", "TXS"):
            nd = "T"
        else:
            dd = stack_delta(i, mx)
            nd = "T" if d == "T" or dd is None else d + dd
        if i.mn in ("PLA", "PLX", "PLY", "PLP", "PLB", "PLD") and nd != "T" and nd < 0:
            f.anomalies.append(f"{a:06X} {i.mn} pops below entry depth ({nd})")
        if i.mn in ("RTS", "RTL") and d != 0:
            f.anomalies.append(f"{a:06X} {i.mn} at stack depth {d}")
        if i.mode in ("sr", "isry") and d != "T" and i.arg > d:
            f.anomalies.append(f"{a:06X} {i.mn} ${i.arg:02X},S reaches above the frame (depth {d})")
        for s in flow_successors(i) + (f.jt_targets if i.mn in ("JMP", "JML") and i.mode in ("iabsx", "iabs", "ilabs") else []):
            if s not in body:
                continue
            old = depth.get(s)
            new = nd if old is None else (old if old == nd else "T")
            if new != old:
                depth[s] = new
                work.append(s)
    f.depth = depth


def flags_use_def(i):
    """(uses, defs) over N V Z C."""
    mn, mode = i.mn, i.mode
    ALL = set(FLAGS)
    if mn in BRANCH:
        return {BRANCH[mn][0]}, set()
    if mn in ("ADC", "SBC"):
        return {"C"}, {"N", "V", "Z", "C"}
    if mn in ("ROL", "ROR"):
        return {"C"}, {"N", "Z", "C"}
    if mn in ("ASL", "LSR", "CMP", "CPX", "CPY"):
        return set(), {"N", "Z", "C"}
    if mn == "BIT":
        return set(), ({"Z"} if mode == "immM" else {"N", "V", "Z"})
    if mn in ("TSB", "TRB"):
        return set(), {"Z"}
    if mn in ("LDA", "LDX", "LDY", "AND", "ORA", "EOR", "INC", "DEC", "INX", "INY", "DEX", "DEY",
              "TAX", "TAY", "TXA", "TYA", "TXY", "TYX", "TSX", "TSC", "TDC", "TCD", "PLA", "PLX",
              "PLY", "PLB", "PLD", "XBA"):
        return set(), {"N", "Z"}
    if mn in ("CLC", "SEC"):
        return set(), {"C"}
    if mn == "CLV":
        return set(), {"V"}
    if mn in ("REP", "SEP"):
        d = set()
        if i.arg & 0x80: d.add("N")
        if i.arg & 0x40: d.add("V")
        if i.arg & 0x02: d.add("Z")
        if i.arg & 0x01: d.add("C")
        return set(), d
    if mn == "PLP":
        return set(), ALL
    if mn == "PHP":
        return ALL, set()
    if mn == "XCE":
        return {"C"}, {"C"}
    if mn in ("JSR", "JSL"):
        return ALL, ALL
    if mn in ("RTS", "RTL", "RTI", "JMP", "JML", "BRK", "COP", "STP", "WAI", "MVN", "MVP"):
        return ALL, set()
    return set(), set()


def analyze_liveness(f, insns):
    body = set(f.body)
    preds = collections.defaultdict(list)
    succs = {}
    for a in f.body:
        i = insns[a]
        ss = [s for s in flow_successors(i) if s in body]
        if i.mn in ("JMP", "JML") and i.mode in ("iabsx", "iabs", "ilabs"):
            ss += [s for s in f.jt_targets if s in body]
        succs[a] = ss
        for s in ss:
            preds[s].append(a)
    ALL = set(FLAGS)
    live_in = {a: set() for a in f.body}
    live_out = {a: set() for a in f.body}
    ud = {a: flags_use_def(insns[a]) for a in f.body}
    work = list(f.body)
    while work:
        a = work.pop()
        i = insns[a]
        out = set()
        for s in succs[a]:
            out |= live_in[s]
        # leaving the function (tail call / fallthrough out) keeps everything live
        if any(s not in body for s in flow_successors(i)):
            out |= ALL
        uses, defs = ud[a]
        new_in = uses | (out - defs)
        live_out[a] = out
        if new_in != live_in[a]:
            live_in[a] = new_in
            work.extend(preds[a])
    f.live = live_out


# --------------------------------------------------------------------------------------------
# Cycle estimate (base cycles; good enough for frame pacing)
# --------------------------------------------------------------------------------------------
MODE_CYC = {"imp": 2, "acc": 2, "imm8": 2, "immM": 2, "immX": 2, "imm16": 5, "dp": 3, "dpx": 4,
            "dpy": 4, "idp": 5, "idpx": 6, "idpy": 6, "ildp": 6, "ildpy": 6, "sr": 4, "isry": 7,
            "rel": 3, "rel16": 4, "abs": 4, "absx": 5, "absy": 5, "long": 5, "longx": 5,
            "iabs": 5, "iabsx": 6, "ilabs": 6, "blk": 7}
MN_CYC = {"JSR": 6, "JSL": 8, "RTS": 6, "RTL": 6, "RTI": 7, "PHA": 3, "PHX": 3, "PHY": 3,
          "PLA": 4, "PLX": 4, "PLY": 4, "PHP": 3, "PLP": 4, "PHB": 3, "PLB": 4, "PHD": 4,
          "PLD": 5, "PHK": 3, "PEA": 5, "PEI": 6, "PER": 6, "XBA": 3, "REP": 3, "SEP": 3}


def cycles(i):
    c = MN_CYC.get(i.mn, MODE_CYC.get(i.mode, 3))
    if i.mn in A_RMW and i.mode != "acc":
        c += 2
    return c


# --------------------------------------------------------------------------------------------
# Code generation
# --------------------------------------------------------------------------------------------
class Gen:
    def __init__(self, insns, funcs, entries, files=None):
        self.insns = insns
        self.funcs = funcs
        self.entries = entries
        self.lines = []
        self.fbase = {fid: os.path.basename(p).lower() for fid, p in (files or {}).items()}
        self.patched = set()

    def patch_key(self, i):
        return (self.fbase.get(i.file, ""), i.line)

    def fname(self, addr):
        return "F_" + self.entries[addr]

    # ---- effective addresses ------------------------------------------------------------
    def ea(self, i):
        """(expr, kind) where kind 'b0' = 16-bit bank-0 address, 'l' = 24-bit address."""
        a = i.arg
        mode = i.mode
        if mode == "dp":
            return f"(uint16_t)(cpu.D + 0x{a:02X})", "b0"
        if mode == "dpx":
            return f"(uint16_t)(cpu.D + 0x{a:02X} + cpu.X)", "b0"
        if mode == "dpy":
            return f"(uint16_t)(cpu.D + 0x{a:02X} + cpu.Y)", "b0"
        if mode == "abs":
            return f"(DBASE | 0x{a:04X})", "l"
        if mode == "absx":
            return f"((DBASE + 0x{a:04X} + cpu.X) & 0xFFFFFF)", "l"
        if mode == "absy":
            return f"((DBASE + 0x{a:04X} + cpu.Y) & 0xFFFFFF)", "l"
        if mode == "long":
            return f"0x{a:06X}", "l"
        if mode == "longx":
            return f"((0x{a:06X} + cpu.X) & 0xFFFFFF)", "l"
        if mode == "idp":
            return f"(DBASE | rd16_b0((uint16_t)(cpu.D + 0x{a:02X})))", "l"
        if mode == "idpx":
            return f"(DBASE | rd16_b0((uint16_t)(cpu.D + 0x{a:02X} + cpu.X)))", "l"
        if mode == "idpy":
            return f"(((DBASE | rd16_b0((uint16_t)(cpu.D + 0x{a:02X}))) + cpu.Y) & 0xFFFFFF)", "l"
        if mode == "ildp":
            return f"rd24_b0((uint16_t)(cpu.D + 0x{a:02X}))", "l"
        if mode == "ildpy":
            return f"((rd24_b0((uint16_t)(cpu.D + 0x{a:02X})) + cpu.Y) & 0xFFFFFF)", "l"
        if mode == "sr":
            return f"(uint16_t)(cpu.S + 0x{a:02X})", "b0"
        if mode == "isry":
            return f"(((DBASE | rd16_b0((uint16_t)(cpu.S + 0x{a:02X}))) + cpu.Y) & 0xFFFFFF)", "l"
        raise ValueError(mode)

    def rd(self, i, w):
        if i.mode in ("immM", "immX", "imm8"):
            key = self.patch_key(i)
            if not i.macro and key in PATCH_IMM:
                self.patched.add(key)
                return PATCH_IMM[key]
            return f"0x{i.arg:0{2 if w == 8 else 4}X}"
        e, k = self.ea(i)
        if w == 8:
            return f"rd8({e})"
        return f"rd16_b0({e})" if k == "b0" else f"rd16({e})"

    def wr(self, i, w, val):
        e, k = self.ea(i)
        if w == 8:
            return f"wr8({e}, (uint8_t)({val}));"
        return f"wr16_b0({e}, (uint16_t)({val}));" if k == "b0" else f"wr16({e}, (uint16_t)({val}));"

    # ---- one instruction, fixed width -----------------------------------------------------
    def body_for(self, f, i, w, live):
        """C statements for instruction i at width w (8/16 or None)."""
        mn = i.mn
        nz = ("N" in live) or ("Z" in live)
        mask = "0xFF" if w == 8 else "0xFFFF"
        NZ = (lambda v: f" NZ{w}({v});" if nz else "")

        def setreg_a(v):
            if w == 8:
                return f"cpu.A = (cpu.A & 0xFF00) | (uint8_t)({v});"
            return f"cpu.A = (uint16_t)({v});"

        alow = "(cpu.A & 0xFF)" if w == 8 else "cpu.A"
        if mn == "LDA":
            return f"{{ uint16_t t = {self.rd(i, w)}; {setreg_a('t')}{NZ('t')} }}"
        if mn in ("LDX", "LDY"):
            r = "cpu.X" if mn == "LDX" else "cpu.Y"
            return f"{r} = {self.rd(i, w)};{NZ(r)}"
        if mn == "STA":
            return self.wr(i, w, alow)
        if mn in ("STX", "STY"):
            return self.wr(i, w, "cpu.X" if mn == "STX" else "cpu.Y")
        if mn == "STZ":
            return self.wr(i, w, "0")
        if mn in ("ORA", "AND", "EOR"):
            op = {"ORA": "|", "AND": "&", "EOR": "^"}[mn]
            return f"{{ uint16_t t = ({alow} {op} {self.rd(i, w)}) & {mask}; {setreg_a('t')}{NZ('t')} }}"
        if mn in ("ADC", "SBC"):
            fn = f"{'adc' if mn == 'ADC' else 'sbc'}{w}"
            vflag = "1" if ("V" in live or "C" in live or nz) else "1"
            return f"{fn}({self.rd(i, w)});"
        if mn in ("CMP", "CPX", "CPY"):
            reg = {"CMP": alow, "CPX": "cpu.X", "CPY": "cpu.Y"}[mn]
            parts = [f"uint32_t r = (uint32_t){reg} - (uint32_t){self.rd(i, w)};"]
            if "C" in live:
                parts.append("cpu.C = (r >> 31) ^ 1;" if False else f"cpu.C = r <= {mask};")
            if nz:
                parts.append(f"NZ{w}(r);")
            return "{ " + " ".join(parts) + " }"
        if mn == "BIT":
            v = self.rd(i, w)
            if i.mode == "immM":
                return f"cpu.Z = (({alow} & {v}) & {mask}) == 0;" if "Z" in live else ";"
            hb = 7 if w == 8 else 15
            return (f"{{ uint16_t t = {v}; cpu.Z = (({alow} & t) & {mask}) == 0; "
                    f"cpu.N = (t >> {hb}) & 1; cpu.V = (t >> {hb - 1}) & 1; }}")
        if mn in ("TSB", "TRB"):
            e, k = self.ea(i)
            rdf = "rd8" if w == 8 else ("rd16_b0" if k == "b0" else "rd16")
            wrf = "wr8" if w == 8 else ("wr16_b0" if k == "b0" else "wr16")
            newv = f"t | {alow}" if mn == "TSB" else f"t & ~{alow}"
            return (f"{{ uint32_t ea = {e}; uint16_t t = {rdf}(ea); "
                    f"cpu.Z = (t & {alow}) == 0; {wrf}(ea, ({newv}) & {mask}); }}")
        if mn in A_RMW and i.mode == "acc":
            return self.shift_op(mn, w, alow, None, live)
        if mn in A_RMW:
            return self.shift_op(mn, w, None, i, live)
        if mn in ("INX", "INY", "DEX", "DEY"):
            r = "cpu.X" if mn in ("INX", "DEX") else "cpu.Y"
            d = "+ 1" if mn in ("INX", "INY") else "- 1"
            return f"{r} = ({r} {d}) & {mask};{NZ(r)}"
        if mn in ("TAX", "TAY"):
            r = "cpu.X" if mn == "TAX" else "cpu.Y"
            return f"{r} = cpu.A & {mask};{NZ(r)}"
        if mn in ("TXA", "TYA"):
            src = "cpu.X" if mn == "TXA" else "cpu.Y"
            return f"{setreg_a(src)}{NZ('cpu.A')}"
        if mn in ("TXY", "TYX"):
            d, s = ("cpu.Y", "cpu.X") if mn == "TXY" else ("cpu.X", "cpu.Y")
            return f"{d} = {s};{NZ(d)}"
        if mn == "TSX":
            return f"cpu.X = cpu.S & {mask};{NZ('cpu.X')}"
        if mn == "PHA":
            return f"push{w}({alow});"
        if mn in ("PHX", "PHY"):
            return f"push{w}({'cpu.X' if mn == 'PHX' else 'cpu.Y'});"
        if mn == "PLA":
            return f"{{ uint16_t t = pull{w}(); {setreg_a('t')}{NZ('t')} }}"
        if mn in ("PLX", "PLY"):
            r = "cpu.X" if mn == "PLX" else "cpu.Y"
            return f"{r} = pull{w}();{NZ(r)}"
        raise ValueError(f"no width body for {mn}")

    def shift_op(self, mn, w, areg, i, live):
        mask = "0xFF" if w == 8 else "0xFFFF"
        hb = 7 if w == 8 else 15
        nz = ("N" in live) or ("Z" in live)
        if areg is not None:
            load = f"uint16_t t = {areg};"
            if w == 8:
                store = "cpu.A = (cpu.A & 0xFF00) | (uint8_t)r;"
            else:
                store = "cpu.A = (uint16_t)r;"
        else:
            e, k = self.ea(i)
            rdf = "rd8" if w == 8 else ("rd16_b0" if k == "b0" else "rd16")
            wrf = "wr8" if w == 8 else ("wr16_b0" if k == "b0" else "wr16")
            load = f"uint32_t ea = {e}; uint16_t t = {rdf}(ea);"
            store = f"{wrf}(ea, r);"
        if mn == "ASL":
            calc = f"uint16_t r = (t << 1) & {mask};"
            cf = f"cpu.C = (t >> {hb}) & 1;"
        elif mn == "LSR":
            calc = "uint16_t r = t >> 1;"
            cf = "cpu.C = t & 1;"
        elif mn == "ROL":
            calc = f"uint16_t r = ((t << 1) | cpu.C) & {mask};"
            cf = f"cpu.C = (t >> {hb}) & 1;"
        elif mn == "ROR":
            calc = f"uint16_t r = (t >> 1) | ((uint16_t)cpu.C << {hb});"
            cf = "cpu.C = t & 1;"
        elif mn == "INC":
            calc = f"uint16_t r = (t + 1) & {mask};"
            cf = ""
        elif mn == "DEC":
            calc = f"uint16_t r = (t - 1) & {mask};"
            cf = ""
        if cf and "C" not in live:
            cf = ""
        nzs = f" NZ{w}(r);" if nz else ""
        return f"{{ {load} {calc} {cf} {store}{nzs} }}"

    # ---- helpers ------------------------------------------------------------------------
    def emit(self, s):
        self.lines.append(s)

    def call_seq(self, f, i, ret_target):
        """Push the return address the way the CPU does, then call."""
        if i.mn == "JSR":
            ret = (i.addr + i.size - 1) & 0xFFFF
            return f"push16(0x{ret:04X});"
        ret = (i.addr + i.size - 1) & 0xFFFF
        return f"push8(0x{i.addr >> 16:02X}); push16(0x{ret:04X});"

    def gen_func(self, f):
        insns = self.insns
        body = f.body
        bodyset = set(body)
        # labels: targets of branches within the body, jump-table targets, and the entry
        targets = set()
        for a in body:
            i = insns[a]
            if i.target is not None and i.target in bodyset and (
                    i.mn in BRANCH or i.mn in ("BRA", "BRL") or
                    (i.mn == "JMP" and i.mode == "abs") or (i.mn == "JML" and i.mode == "long")):
                targets.add(i.target)
            # (fallthrough needs no label: the body is emitted in address order, so a+size,
            # when it is in the body, is the very next instruction emitted)
        targets |= set(t for t in f.jt_targets if t in bodyset)
        targets |= set(t for t in f.conts if t in bodyset)
        leaders = set(targets) | {f.entry}
        for a in body:
            i = insns[a]
            if i.mn in BRANCH or i.mn in ("JSR", "JSL") or i.mn in UNCOND:
                leaders.add(a + i.size)
        e = self.emit
        bank = f.entry >> 16
        e(f"/* {f.name}: ${f.entry:06X}  entry M={f.entry_mx[0]} X={f.entry_mx[1]} */")
        e(f"void {self.fname(f.entry)}(void) {{")
        e(f"  ENTER_FUNC(0x{f.entry:06X});")
        if f.name in HOOKS:
            e(f"  {HOOKS[f.name]}();")
        if f.name in SKIP_HOOKS:
            ret = "RET_RTL();" if any(insns[a].mn == "RTL" for a in body) else "RET_RTS();"
            e(f"  if ({SKIP_HOOKS[f.name]}()) {{ {ret} }}")
        if body[0] != f.entry:
            e(f"  goto L_{f.entry:06X};")
        prev_end = None
        # block cycle sums
        for idx, a in enumerate(body):
            i = insns[a]
            if prev_end is not None and prev_end != a and prev_fall:
                e(f"  goto L_{prev_end:06X};")
            if a in targets or a == f.entry:
                e(f"L_{a:06X}:;")
            if a in leaders:
                # sum cycles to the end of this block
                cyc = 0
                b = a
                while b in bodyset:
                    bi = insns[b]
                    cyc += cycles(bi)
                    if bi.mn in BRANCH or bi.mn in ("JSR", "JSL") or bi.mn in UNCOND or bi.mn in ("JMP", "JML"):
                        break
                    b = b + bi.size
                    if b in leaders:
                        break
                e(f"  CYC({cyc});")
            mx = f.state.get(a, ("T", "T"))
            live = f.live.get(a, set(FLAGS))
            code = self.gen_insn(f, i, mx, live, bodyset)
            e(f"  /* {a:06X} {i.mn} {i.mode} */ {code}")
            prev_end = a + i.size
            prev_fall = (prev_end in bodyset) and (prev_end in flow_successors(i)) and not (
                i.mn in UNCOND or i.mn in ("JMP", "JML"))
            if not prev_fall and (prev_end in flow_successors(i)) and prev_end not in bodyset:
                pass
        if prev_end is not None and prev_fall and body[-1] != body[-1]:
            pass
        e("}")
        e("")

    def goto_or_tail(self, f, target, bodyset, cur):
        if target in bodyset:
            back = target <= cur
            return f"{{ {f'POLL_AT(0x{cur:06X}); ' if back else ''}goto L_{target:06X}; }}"
        return f"{{ {self.fname(target)}(); return; }}"

    def gen_insn(self, f, i, mx, live, bodyset):
        mn = i.mn
        w = width_of(i, mx)
        # fallthrough out of the body (into another source file) -> tail call
        tail_fall = ""
        nxt = i.addr + i.size
        if nxt in flow_successors(i) and nxt not in bodyset and mn not in BRANCH:
            tail_fall = f" {self.fname(nxt)}(); return;"
        key = self.patch_key(i)
        if not i.macro and key in PATCH_CODE:
            self.patched.add(key)
            return PATCH_CODE[key] + tail_fall
        if w in ("M", "X"):
            flag = "cpu.M" if w == "M" else "cpu.XF"
            a8 = self.body_for(f, i, 8, live)
            a16 = self.body_for(f, i, 16, live)
            return f"if ({flag}) {{ {a8} }} else {{ {a16} }}{tail_fall}"
        if w in (8, 16):
            return self.body_for(f, i, w, live) + tail_fall
        code = self.gen_fixed(f, i, mx, live, bodyset)
        return code + tail_fall

    def gen_fixed(self, f, i, mx, live, bodyset):
        mn = i.mn
        nz = ("N" in live) or ("Z" in live)
        if mn in BRANCH:
            flag, val = BRANCH[mn]
            cond = f"cpu.{flag}" if val else f"!cpu.{flag}"
            tgt = self.goto_or_tail(f, i.target, bodyset, i.addr)
            fall = i.addr + i.size
            s = f"if ({cond}) {tgt}"
            if fall not in bodyset:
                s += f" {self.fname(fall)}(); return;"
            return s
        if mn in ("BRA", "BRL"):
            return self.goto_or_tail(f, i.target, bodyset, i.addr)
        if mn == "JMP" and i.mode == "abs":
            return self.goto_or_tail(f, i.target, bodyset, i.addr)
        if mn == "JML" and i.mode == "long":
            pb = f"cpu.PB = 0x{i.tbank:02X}; "
            if i.target in bodyset:
                back = i.target <= i.addr
                return f"{{ {pb}{f'POLL_AT(0x{i.addr:06X}); ' if back else ''}goto L_{i.target:06X}; }}"
            if i.target in self.entries:
                return f"{{ {pb}{self.fname(i.target)}(); return; }}"
            return f"{{ eb_jump(0x{i.tbank:02X}{i.target & 0xFFFF:04X}); return; }}"
        if mn in ("JSR", "JSL") and i.mode in ("abs", "long"):
            ret = (i.addr + i.size - 1) & 0xFFFF
            name = self.entries.get(i.target)
            if mn == "JSR":
                if name is None:
                    return f"JSR_IND(0x{ret:04X}, 0x{i.target:06X});"
                return f"JSR_(0x{ret:04X}, {self.fname(i.target)});"
            if name == "SETJMP":
                return f"EB_SETJMP_CALL(0x{i.tbank:02X}, 0x{ret:04X}, {self.fname(i.target)});"
            if name is None:
                return (f"{{ uint16_t sv_ = g_ret_s; push8(cpu.PB); push16(0x{ret:04X}); "
                        f"g_ret_s = cpu.S; eb_call(0x{i.tbank:02X}{i.target & 0xFFFF:04X}); "
                        f"g_ret_s = sv_; }}")
            return f"JSL_(0x{i.tbank:02X}, 0x{ret:04X}, {self.fname(i.target)});"
        if mn == "JSR" and i.mode == "iabsx":
            ret = (i.addr + i.size - 1) & 0xFFFF
            return (f"JSR_IND(0x{ret:04X}, PBASE | rd16(PBASE | (uint16_t)(0x{i.arg:04X} + cpu.X)));")
        if mn == "JMP" and i.mode in ("iabs", "iabsx"):
            if i.mode == "iabs":
                ptr = f"rd16_b0(0x{i.arg:04X})"
            else:
                ptr = f"rd16(PBASE | (uint16_t)(0x{i.arg:04X} + cpu.X))"
            cases = "".join(f"case 0x{t & 0xFFFF:04X}: goto L_{t:06X}; "
                            for t in f.jt_targets if t in bodyset)
            if cases:
                return (f"{{ uint16_t t = {ptr}; POLL_AT(0x{i.addr:06X}); switch (t) {{ {cases}"
                        f"default: eb_jump(PBASE | t); return; }} }}")
            return f"{{ uint16_t t = {ptr}; eb_jump(PBASE | t); return; }}"
        if mn == "JML" and i.mode == "ilabs":
            cases = "".join(f"case 0x{t:06X}: goto L_{t:06X}; " for t in f.jt_targets if t in bodyset)
            ptr = f"rd24_b0(0x{i.arg:04X})"
            if cases:
                return (f"{{ uint32_t t = {ptr}; POLL_AT(0x{i.addr:06X}); switch (eb_canon(t)) {{ {cases}"
                        f"default: eb_jump(t); return; }} }}")
            return f"{{ eb_jump({ptr}); return; }}"
        if mn in ("RTS", "RTL"):
            conts = sorted(c for c in f.conts if c in bodyset)
            macro = "RET_RTS();" if mn == "RTS" else "RET_RTL();"
            if not conts:
                return macro
            n = 2 if mn == "RTS" else 3
            pb = "" if mn == "RTS" else " cpu.PB = rd8(cpu.S);"
            cases = "".join(f"case 0x{(c - 1) & 0xFFFF:04X}: goto L_{c:06X}; " for c in conts)
            trick = "eb_rts_trick" if mn == "RTS" else "eb_rtl_trick"
            return (f"if (EB_LIKELY(cpu.S == g_ret_s)) {{ cpu.S += {n};{pb} return; }} "
                    f"{{ uint16_t c_ = {trick}(); switch (c_) {{ {cases}default: break; }} return; }}")
        if mn == "RTI":
            return "RET_RTI();"
        if mn == "REP":
            return f"op_rep(0x{i.arg:02X});"
        if mn == "SEP":
            return f"op_sep(0x{i.arg:02X});"
        if mn == "CLC":
            return "cpu.C = 0;"
        if mn == "SEC":
            return "cpu.C = 1;"
        if mn == "CLV":
            return "cpu.V = 0;"
        if mn == "CLI":
            return "cpu.I = 0;"
        if mn == "SEI":
            return "cpu.I = 1;"
        if mn == "CLD":
            return "cpu.DF = 0;"
        if mn == "SED":
            return "cpu.DF = 1;"
        if mn == "XCE":
            return "op_xce();"
        if mn in ("NOP", "WDM"):
            return ";"
        if mn == "WAI":
            return "eb_wai();"
        if mn in ("STP", "BRK", "COP"):
            return f"eb_fatal(\"{mn} at {i.addr:06X}\");"
        if mn == "XBA":
            return "cpu.A = (uint16_t)((cpu.A >> 8) | (cpu.A << 8));" + (" NZ8(cpu.A);" if nz else "")
        if mn == "TCD":
            return "cpu.D = cpu.A;" + (" NZ16(cpu.D);" if nz else "")
        if mn == "TDC":
            return "cpu.A = cpu.D;" + (" NZ16(cpu.A);" if nz else "")
        if mn == "TCS":
            return "cpu.S = cpu.A;"
        if mn == "TSC":
            return "cpu.A = cpu.S;" + (" NZ16(cpu.A);" if nz else "")
        if mn == "TXS":
            return "cpu.S = cpu.X;"
        if mn == "PHB":
            return "push8(cpu.DB);"
        if mn == "PLB":
            return "cpu.DB = pull8();" + (" NZ8(cpu.DB);" if nz else "")
        if mn == "PHD":
            return "push16(cpu.D);"
        if mn == "PLD":
            return "cpu.D = pull16();" + (" NZ16(cpu.D);" if nz else "")
        if mn == "PHK":
            return "push8(cpu.PB);"
        if mn == "PHP":
            return "push8(get_p());"
        if mn == "PLP":
            return "set_p(pull8());"
        if mn == "PEA":
            return f"push16(0x{i.arg:04X});"
        if mn == "PEI":
            return f"push16(rd16_b0((uint16_t)(cpu.D + 0x{i.arg:02X})));"
        if mn == "PER":
            return f"push16(0x{i.target & 0xFFFF:04X});"
        if mn in ("MVN", "MVP"):
            dst, src = i.arg & 0xFF, i.arg >> 8
            return f"op_{mn.lower()}(0x{src:02X}, 0x{dst:02X});"
        raise ValueError(f"unhandled {mn} {i.mode} at {i.addr:06X}")


# --------------------------------------------------------------------------------------------
def dump(name, db, insns, funcs, entries):
    """Print a function listing: address, state, stack depth, source line."""
    files = db["files"]
    texts = {}

    def src(i):
        fid = i.file
        if fid not in texts:
            try:
                with open(files[fid], encoding="latin-1") as fh:
                    texts[fid] = fh.read().split("\n")
            except OSError:
                texts[fid] = []
        t = texts[fid]
        return t[i.line - 1].strip() if 0 < i.line <= len(t) else "?"

    for f in funcs.values():
        if f.name != name and f"{f.entry:06X}" != name.upper():
            continue
        print(f"== {f.name} ${f.entry:06X} entry M/X={f.entry_mx} exit={f.exit_mx} "
              f"tails={[entries.get(t) for t in sorted(f.tails)]}")
        for a in f.body:
            i = insns[a]
            m, x = f.state.get(a, ("?", "?"))
            d = f.depth.get(a, "?")
            print(f"  {a:06X} m={m} x={x} d={d!s:>2}  {i.mn} {i.mode} {i.arg:X}   ; {src(i)}"
                  f"{'  [macro]' if i.macro else ''}")
        for x in f.anomalies:
            print("  !", x)


def main():
    report = "--report" in sys.argv
    problems = []
    db, rom = load()
    insns = decode(db, rom, problems)
    funcs, entries = build_functions(db, insns, problems)
    print(f"instructions={len(insns)} functions={len(funcs)}")
    if "--dump" in sys.argv:
        names = sys.argv[sys.argv.index("--dump") + 1].split(",")
        for rnd in range(6):
            if not sum(analyze_mx(funcs[e], insns, funcs, entries) for e in sorted(funcs)):
                break
        for f in funcs.values():
            analyze_stack(f, insns)
        for n in names:
            dump(n, db, insns, funcs, entries)
        return
    # interprocedural M/X fixpoint (callee summaries)
    for rnd in range(6):
        changed = 0
        for e in sorted(funcs):
            if analyze_mx(funcs[e], insns, funcs, entries):
                changed += 1
        print(f"  mx round {rnd}: {changed} summaries changed")
        if not changed:
            break
    unknown_w = 0
    for f in funcs.values():
        check_evidence(f, insns, problems)
        analyze_stack(f, insns)
        analyze_liveness(f, insns)
        for a in f.body:
            w = width_of(insns[a], f.state.get(a, ("T", "T")))
            if w in ("M", "X"):
                unknown_w += 1
    print(f"dynamic-width instruction instances: {unknown_w}")
    anomalies = [(f.name, x) for f in funcs.values() for x in f.anomalies]
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "recomp_report.txt"), "w") as rf:
        rf.write(f"problems ({len(problems)}):\n")
        for p in problems:
            rf.write("  " + p + "\n")
        rf.write(f"stack anomalies ({len(anomalies)}):\n")
        for n, x in sorted(anomalies):
            rf.write(f"  {n}: {x}\n")
        mncount = collections.Counter(insns[a].mn for a in insns)
        rf.write("opcode use: " + ", ".join(f"{k}={v}" for k, v in sorted(mncount.items())) + "\n")
    print(f"problems={len(problems)} anomalies={len(anomalies)} (see build/gen/recomp_report.txt)")
    if report:
        return
    global g_syms, g_files
    g_syms = db["syms"]
    g_files = db["files"]
    gen_files(insns, funcs, entries)


def write_text_table():
    """eb_texttable.h: the game's character codes -> ASCII (from ebsrc earthbound.yml)."""
    yml = os.path.join(ROOT, "ebsrc-main", "earthbound.yml")
    table = ["?"] * 256
    sect = False
    with open(yml, encoding="utf-8") as fh:
        for line in fh:
            if not line.startswith(" "):
                sect = line.startswith("textTable:")
                continue
            if not sect or ":" not in line:
                continue
            k, v = line.strip().split(":", 1)
            v = v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
                v = v[1:-1].replace("''", "'")
            try:
                code = int(k, 0)
            except ValueError:
                continue
            if 0 <= code < 256 and len(v) == 1 and 32 <= ord(v) < 127:
                table[code] = v
    with open(os.path.join(OUT, "eb_texttable.h"), "w", newline="\n") as h:
        h.write("/* generated by tools/recomp.py from ebsrc earthbound.yml - do not edit */\n")
        h.write("#pragma once\n")
        h.write("static const char eb_text_ascii[256] = {")
        h.write(",".join(str(ord(c)) for c in table))
        h.write("};\n")


# Functions implemented by hand in src/ (the generated body is skipped).
NATIVE = {"LONGJMP", "IS_ENTITY_SPRITE_ONSCREEN", "UNKNOWN_C08CD5", "UNKNOWN_C01B96"}
# Functions that call a C hook on entry (debug/test logging, enhancement taps).
# Functions whose C hook decides on entry whether the call is skipped (nonzero = return at once).
SKIP_HOOKS = {
    # battle letterbox (HDMA to TM/TS hiding the background above and below a band)
    "ENABLE_LETTERBOX_HDMA": "eb_skip_letterbox_hdma",
}

HOOKS = {"PRINT_LETTER": "eb_hook_print_letter", "PRINT_NEWLINE": "eb_hook_print_newline",
         "OAM_CLEAR": "eb_hook_oam_clear"}

# Enhancement patches, keyed by (ebsrc source file, line). g_ws_ext / g_ws_cols are the
# widescreen margin in pixels / 8 px tile columns (0 unless widescreen is active on the map).
# PATCH_IMM replaces an immediate operand with a C expression; PATCH_CODE replaces the whole
# instruction.
PATCH_IMM = {
    # DRAW_ENTITIES: entities are drawn when -64 <= screen x < 320; widen by the margin
    ("draw_entities.asm", 38): "(uint16_t)(0x0140 + g_ws_ext)",
    ("draw_entities.asm", 40): "(uint16_t)(0xFFC0 - g_ws_ext)",
    # LOAD_MAP_ROW_VRAM writes MAP_RESOLUTION_WIDTH (34) tiles of each 64-tile row; the rest of
    # its row buffer is uninitialised heap that lands in the margins. Write 34 + 2*cols tiles.
    ("load_map_row_vram.asm", 142): "(uint16_t)(34 + 2 * g_ws_cols)",
    # REFRESH_MAP_AT_POSITION: the VRAM column streamed in on the right is SCREEN_LEFT_X + 32
    ("refresh_map_at_position.asm", 81): "(uint16_t)(0x0020 + g_ws_cols)",
}
PATCH_CODE = {
    # ... and on the left SCREEN_LEFT_X - 1 (a DEC in 16-bit mode)
    ("refresh_map_at_position.asm", 121): "{ uint16_t t = (uint16_t)(cpu.A - 1 - g_ws_cols); cpu.A = t; NZ16(t); }",
    # ... and its rows start at SCREEN_LEFT_X - 1 (DEC): start cols further left
    ("load_map_row_vram.asm", 34): "{ uint16_t t = (uint16_t)(cpu.A - 1 - g_ws_cols); cpu.A = t; NZ16(t); }",
}


def gen_files(insns, funcs, entries):
    g = Gen(insns, funcs, entries, g_files)
    order = sorted(funcs)
    chunks = []
    cur = []
    size = 0
    for e in order:
        cur.append(e)
        size += len(funcs[e].body)
        if size > 2500:
            chunks.append(cur)
            cur = []
            size = 0
    if cur:
        chunks.append(cur)
    with open(os.path.join(OUT, "eb_gen.h"), "w", newline="\n") as h:
        h.write("/* generated by tools/recomp.py - do not edit */\n#pragma once\n")
        for e in order:
            h.write(f"void F_{entries[e]}(void); /* ${e:06X} */\n")
        h.write(f"#define EB_NUM_FUNCS {len(order)}\n")
        for hk in sorted(set(HOOKS.values())):
            h.write(f"void {hk}(void);\n")
        for hk in sorted(set(SKIP_HOOKS.values())):
            h.write(f"int {hk}(void);\n")
    for n, chunk in enumerate(chunks):
        g.lines = ["/* generated by tools/recomp.py - do not edit */",
                   '#include "eb_runtime.h"', '#include "eb_gen.h"', ""]
        for e in chunk:
            if funcs[e].name in NATIVE:
                g.lines.append(f"/* {funcs[e].name}: native implementation in src/ */")
                continue
            g.gen_func(funcs[e])
        with open(os.path.join(OUT, f"eb_gen_{n:02d}.c"), "w", newline="\n") as cf:
            cf.write("\n".join(g.lines))
    # RAM symbols for the hand-written game glue (src/game.c etc.)
    with open(os.path.join(OUT, "eb_syms.h"), "w", newline="\n") as h:
        h.write("/* generated by tools/recomp.py from ebsrc labels - do not edit */\n#pragma once\n")
        seen = set()
        for name, val, kind, parent, dref, cref in sorted(g_syms, key=lambda s: s[1]):
            if kind not in ("lab", "equ") or name.startswith("@") or name in seen:
                continue
            if 0x7E0000 <= val <= 0x7FFFFF or (kind == "lab" and val < 0x2000 and val > 0):
                seen.add(name)
                h.write(f"#define SYM_{c_ident(name)} 0x{(val & 0x1FFFF) if val >= 0x7E0000 else val:05X}u\n")
    write_text_table()
    with open(os.path.join(OUT, "eb_dispatch.c"), "w", newline="\n") as d:
        d.write('/* generated by tools/recomp.py - do not edit */\n#include "eb_runtime.h"\n'
                '#include "eb_gen.h"\n\nconst EbFuncEntry eb_func_table[EB_NUM_FUNCS] = {\n')
        for e in order:
            d.write(f'  {{0x{e:06X}, F_{entries[e]}, "{entries[e]}"}},\n')
        d.write("};\n")
    print(f"wrote {len(chunks)} C files to {OUT}")
    missing = (set(PATCH_IMM) | set(PATCH_CODE)) - g.patched
    print(f"patches applied: {len(g.patched)}" + (f", NOT APPLIED: {sorted(missing)}" if missing else ""))


if __name__ == "__main__":
    main()
