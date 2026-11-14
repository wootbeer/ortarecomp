#!/usr/bin/env python3
"""
Map Orta's hooks in src/hle/ onto pd1, and check every PROG_VA pair.

    python tools/program_map.py ORTA/default.xbe PD1/pd1.xbe          # write
    python tools/program_map.py ORTA/default.xbe PD1/pd1.xbe --check  # verify

src/hle/ (Direct3D, DirectSound, CRI, input: XDK and middleware code both
programs contain) is written once, with Orta's addresses, and compiled once
for each program. pd1's build force-includes src/pd1/orta_hooks.h, which
this writes: every sub_XXXXXXXX that src/hle/ names (overrides, _enter
hooks, _gen originals, direct calls) as pd1's function, and (through
src/pd1/hle_names.h, which pd1's own code in src/pd1/ includes too) the
globals src/hle/ defines under pd1_ names, so pd1's code calls pd1's copy. tools/recomp.py --program pd1 reads the
same header to tell the recompiler which of pd1's functions src/hle/
replaces.

A hook is mapped only to code that is the same: the pair xbe_match.py finds
for whole functions (identical, addresses masked), or else the one place in
pd1's code whose bytes match Orta's with addresses masked and whose calls
and jumps go to the mapped counterparts of Orta's targets (thunks: a jmp is
the same everywhere). Anything else stops the tool: a hook that silently
lands on whatever pd1 has at Orta's address would be the worst outcome.
Trace-only hooks (d3d_calltrace.c) with no counterpart are left out of pd1
instead, and ORTA_ONLY lists hooks checked by hand to be Orta's alone.

Data addresses in src/hle/ are written PROG_VA(orta, pd1). --check (and
every run) confirms each pd1 value: the address identical code uses in
pd1 where Orta's uses Orta's (xbe_match.py's data pairs), or one at the
same offset from such a pair (a field, a table index), or MANUAL below.
pd1 0 means Orta's own game code; it is listed, not checked.

Reads the XBEs and xboxrecomp's disassembly (cached in build/match-pd1/);
writes only addresses, which are facts about the binaries.
"""

import argparse
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import xbe_match  # noqa: E402

HLE_DIR = os.path.join(ROOT, "src", "hle")
OUT = os.path.join(ROOT, "src", "pd1", "orta_hooks.h")
OUT_NAMES = os.path.join(ROOT, "src", "pd1", "hle_names.h")
TRACE_ONLY = {"d3d_calltrace.c"}

# Orta hooks in src/hle/ that pd1 does not have, checked by hand.
ORTA_ONLY = {
    0x002C2446: "IDirectSound_DownloadEffectsImage: pd1 has no counterpart of the "
                "loader it calls (Orta 0x002C1483) and no effects image (dsstdfx)",
    0x002C2566: "IDirectSoundBuffer_SetMixBinVolumes: pd1 has the internals "
                "(0x0022D343, 0x0022D391) but nothing calls them, no wrapper",
}

# PROG_VA pairs checked by hand, where no identical code links the two.
MANUAL = {
    0x002AC080: (0x00219280, "end of game code: the section after .text (Orta MMATRIX, pd1 D3D)"),
}

# Names the generated code and the kernel layer use for the dispatch, as
# src/guest/orta_names.h and tools/recomp.py's DISPATCH_NAMES.
DISPATCH = ("recomp_lookup", "recomp_dispatch_init", "recomp_dispatch_flat_bytes",
            "recomp_get_count", "recomp_call_all", "recomp_lookup_manual")

SUB_TOKEN = re.compile(r"\bsub_([0-9A-F]{8})(_gen|_enter)?\b")
PROG_VA_RE = re.compile(r"PROG_VA\(\s*0x([0-9A-Fa-f]+)u?\s*,\s*(0x[0-9A-Fa-f]+|0)u?\s*\)")
FUNC_DEF = re.compile(r"^(?!static\b|extern\b|typedef\b|return\b|else\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$")


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def hle_sources():
    out = {}
    for name in sorted(os.listdir(HLE_DIR)):
        if name.endswith(".c"):
            with open(os.path.join(HLE_DIR, name), encoding="utf-8") as f:
                out[name] = f.read()
    return out


def hle_globals(sources):
    """Functions src/hle/ defines with external linkage (not hooks): in
    pd1's copy they become pd1_<name>."""
    names = set()
    for text in sources.values():
        lines = text.splitlines()
        for i, line in enumerate(lines):
            m = FUNC_DEF.match(line)
            if not m or m.group(1).startswith("sub_"):
                continue
            # A definition: its body opens on this line or the next.
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if line.rstrip().endswith("{") or nxt.startswith("{"):
                names.add(m.group(1))
    return sorted(names)


# ── Matching code with addresses masked ─────────────────────────────────

class Code:
    def __init__(self, img):
        import capstone
        self.img = img
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.code = [s for s in img.sections if s["name"] not in xbe_match.CODE_SECTIONS_SKIP]

    def insns(self, va, size=None):
        """The function's instructions: `size` bytes when the disassembler
        knows its size, else up to the first ret or jmp."""
        data = self.img.read(va, size or 0x200) or self.img.read(va, 16) or b""
        out = []
        for ins in self.md.disasm(data, va):
            out.append(ins)
            if not size and ins.mnemonic in ("ret", "jmp"):
                break
        return out

    def pattern(self, insns):
        """A regex over the bytes, 4-byte address fields masked; for each
        rel32 branch, its offset and target; for each masked address, its
        offset and value."""
        pat, branches, refs, at = b"", [], [], 0
        for ins in insns:
            raw = bytes(ins.bytes)
            mask = [False] * len(raw)
            for off, size in ((ins.disp_offset, ins.disp_size), (ins.imm_offset, ins.imm_size)):
                if off and size == 4:
                    v = int.from_bytes(raw[off:off + 4], "little")
                    if self.img.lo <= v < self.img.hi:
                        mask[off:off + 4] = [True] * 4
                        refs.append((at + off, v))
            if raw[0] in (0xE8, 0xE9) and len(raw) == 5:
                mask[1:5] = [True] * 4
                branches.append((at + 1, int(ins.op_str, 16)))
            elif raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F and len(raw) == 6:
                mask[2:6] = [True] * 4
                branches.append((at + 2, int(ins.op_str, 16)))
            pat += b"".join(b"." if mask[i] else re.escape(raw[i:i + 1]) for i in range(len(raw)))
            at += len(raw)
        return re.compile(pat, re.S), branches, refs

    def search(self, rx):
        hits = []
        for s in self.code:
            blob = self.img.data[s["raw"]:s["raw"] + s["rsize"]]
            for m in rx.finditer(blob):
                hits.append(s["va"] + m.start())
        return hits

    def rel_target(self, va, off):
        raw = self.img.read(va + off, 4)
        rel = int.from_bytes(raw, "little", signed=True)
        return (va + off + 4 + rel) & 0xFFFFFFFF


class Mapper:
    def __init__(self, orta, pd1, pairs, ofuncs, dmap):
        self.o, self.p = Code(orta), Code(pd1)
        self.pairs = pairs          # {orta va: pd1 va}, whole identical functions
        self.ofuncs = ofuncs        # {orta va: size}
        self.dmap = dmap            # {orta data va: (pd1 va, votes, total)}
        self.paired = {p: o for o, p in pairs.items()}
        self.memo = {}

    def refs_agree(self, c, refs, branches, depth):
        """The masked addresses of candidate c are the counterparts of
        Orta's: the same kernel import, the data identical code uses alike,
        the mapped function; and so are its calls' and jumps' targets.
        Look-alike thunks and leaves differ only there. What cannot be
        mapped (too far down, too big) does not count against it; a
        counterpart that is known and differs does. Returns (agrees, how
        many could not be checked)."""
        unknown = 0
        for off, v in refs:
            pv = int.from_bytes(self.p.img.read(c + off, 4), "little")
            if v in self.o.img.thunks:
                if self.p.img.thunks.get(pv) != self.o.img.thunks[v]:
                    return False, unknown
            elif v in self.dmap:
                if self.dmap[v][0] != pv:
                    return False, unknown
            elif v in self.ofuncs and depth < 4:
                want, _ = self.map(v, depth + 1)
                if want is None:
                    unknown += 1
                elif want != pv:
                    return False, unknown
            else:
                unknown += 1
        for off, t in branches:
            want = self.map(t, depth + 1)[0] if depth < 4 else None
            if want is None:
                unknown += 1
            elif self.p.rel_target(c, off) != want:
                return False, unknown
        return True, unknown

    def map(self, va, depth=0):
        """pd1's counterpart of Orta's code at va, with how; None if none."""
        if va in self.memo:
            return self.memo[va]
        self.memo[va] = (None, "in progress")
        res = self._map(va, depth)
        self.memo[va] = res
        return res

    def _map(self, va, depth):
        size = self.ofuncs.get(va)
        insns = self.o.insns(va, size if size and size <= 0x4000 else None)
        if not insns:
            return None, "no code"
        rx, branches, refs = self.o.pattern(insns)
        pair = self.pairs.get(va)
        if pair is not None and rx.match(self.p.img.read(pair, (size or 0x200) + 16) or b""):
            if self.refs_agree(pair, refs, branches, depth)[0]:
                return pair, "identical function"
        hits = self.p.search(rx)
        # Every candidate must agree on what it addresses: the same bytes
        # with other addresses are other code. One already paired with
        # another Orta function is that one's.
        checked = []
        for c in hits:
            if self.paired.get(c, va) != va:
                continue
            ok, unknown = self.refs_agree(c, refs, branches, depth)
            if ok:
                checked.append((c, unknown))
        # Taken only when nothing else could be it: the one place in pd1
        # with this code, or the one that every address checked out for.
        if len(checked) == 1 and (len(hits) == 1 or checked[0][1] == 0):
            c, unknown = checked[0]
            return c, ("same code, the only copy" if len(hits) == 1
                       else "same code, every target checked")
        if not checked:
            return None, "not found" if not hits else f"{len(hits)} look-alikes, none agree"
        return None, f"{len(checked)} of {len(hits)} look-alikes could be it"


# ── Main ────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("orta", help="Panzer Dragoon Orta default.xbe")
    ap.add_argument("pd1", help="pd1.xbe")
    ap.add_argument("--check", action="store_true",
                    help="verify src/pd1/orta_hooks.h and the PROG_VA pairs; write nothing")
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "match-pd1"))
    args = ap.parse_args()

    sources = hle_sources()
    hooks = collections.defaultdict(set)     # orta va -> {(file, suffix)}
    for name, text in sources.items():
        for m in SUB_TOKEN.finditer(text):
            hooks[int(m.group(1), 16)].add((name, m.group(2) or ""))

    work = os.path.abspath(args.work)
    print("== pd1")
    pimg = xbe_match.Image(args.pd1, work)
    print("== Orta")
    oimg = xbe_match.Image(args.orta, work)
    print("fingerprinting...", flush=True)
    pf, of = xbe_match.fingerprint_all(pimg), xbe_match.fingerprint_all(oimg)
    raw_pairs = xbe_match.match(pf, of, set(hooks))
    pairs = {o: p for p, (o, how) in raw_pairs.items() if how.startswith("exact")}
    dmap = xbe_match.data_map(raw_pairs, pf, of)     # {orta va: (pd1 va, votes, total)}
    mapper = Mapper(oimg, pimg, pairs, {va: s for va, (s, _) in oimg.functions.items()}, dmap)

    errors, lines, unmapped = [], [], []
    mapped = {}
    for va in sorted(hooks):
        files = sorted({f for f, _ in hooks[va]})
        p, how = mapper.map(va)
        if p is not None:
            mapped[va] = p
            lines.append(f"/* 0x{va:08X} -> 0x{p:08X}: {how} ({', '.join(files)}) */")
        elif va in ORTA_ONLY or set(files) <= TRACE_ONLY:
            unmapped.append(va)
            lines.append(f"/* 0x{va:08X}: none in pd1 ({how}); "
                         f"{ORTA_ONLY.get(va, 'trace only')} ({', '.join(files)}) */")
        else:
            errors.append(f"0x{va:08X} ({', '.join(files)}): {how}")

    # One pd1 function is the counterpart of one Orta function only.
    seen = collections.defaultdict(list)
    for va, p in mapped.items():
        seen[p].append(va)
    for p, vas in sorted(seen.items()):
        if len(vas) > 1:
            errors.append(f"0x{p:08X} in pd1 would be the counterpart of "
                          + ", ".join(f"0x{v:08X}" for v in vas))

    # PROG_VA pairs.
    va_lines = []
    for name, text in sources.items():
        for m in PROG_VA_RE.finditer(text):
            o, p = int(m.group(1), 16), int(m.group(2), 16)
            where = f"{name}:{text.count(chr(10), 0, m.start()) + 1}"
            if p == 0:
                va_lines.append(f"/* {where}: 0x{o:08X} is Orta's own game code */")
                continue
            if o in MANUAL:
                ok = MANUAL[o][0] == p
                why = MANUAL[o][1]
            elif o in dmap:
                ok, why = dmap[o][0] == p, f"used alike {dmap[o][1]}/{dmap[o][2]}"
            else:
                # A field or table entry: the nearest address identical code
                # uses that sits at the same distance in both programs.
                near = sorted((abs(o - k), k) for k in dmap if abs(o - k) < 0x400)
                agree = [k for _, k in near if dmap[k][0] - k == p - o]
                k = agree[0] if agree else None
                ok = k is not None
                if k is None:
                    why = "no evidence" if not near else \
                        f"0x{near[0][1]:08X} nearby is 0x{dmap[near[0][1]][0]:08X} in pd1"
                else:
                    why = (f"0x{o - k:X} after" if o >= k else f"0x{k - o:X} before") + \
                        f" 0x{k:08X}, used alike {dmap[k][1]}/{dmap[k][2]}"
            if not ok:
                errors.append(f"PROG_VA(0x{o:08X}, 0x{p:08X}) at {where}: {why}"
                              + (f", expected 0x{dmap[o][0]:08X}" if o in dmap and o not in MANUAL else ""))
            va_lines.append(f"/* {where}: 0x{o:08X} -> 0x{p:08X}: {why} */")

    if errors:
        print("\n".join(["", "not mapped / wrong:"] + ["  " + e for e in errors]), file=sys.stderr)
        fail(f"{len(errors)} problems; nothing written")

    names = {n: f"pd1_{n}" for n in DISPATCH}
    for n in hle_globals(sources):
        names[n] = f"pd1_{n}"
    renames = {}
    for va, p in mapped.items():
        for _, suffix in hooks[va]:
            renames[f"sub_{va:08X}{suffix}"] = f"pd1_sub_{p:08X}{suffix}"
    for va in unmapped:
        for _, suffix in hooks[va]:
            renames[f"sub_{va:08X}{suffix}"] = f"pd1_unmapped_sub_{va:08X}{suffix}"

    names_text = "\n".join([
        "/*",
        " * Generated by tools/program_map.py. Do not edit: rerun it.",
        " * pd1's names for what src/hle/ defines and the dispatch: force-included",
        " * into all of pd1's code (gen-pd1/, src/pd1/, its build of src/hle/), so",
        " * a call there reaches pd1's copy, never Orta's.",
        " */",
        "#pragma once"] + [f"#define {k} {names[k]}" for k in sorted(names)]) + "\n"
    out = ["/*",
           " * Generated by tools/program_map.py from Orta's default.xbe and pd1.xbe.",
           " * Do not edit: rerun it. Force-included into pd1's build of src/hle/:",
           " * Orta's hooks there as pd1's functions. Addresses only.",
           " */",
           "#pragma once",
           "#define PDO_PROGRAM_PD1 1",
           '#include "pd1/hle_names.h"',
           ""]
    out += [f"#define {k} {renames[k]}" for k in sorted(renames)]
    out += ["", "/* How each hook was matched. */"] + lines
    out += ["", "/* The PROG_VA pairs in src/hle/, checked. */"] + va_lines
    text = "\n".join(out) + "\n"

    print(f"\n{len(mapped)} hooks mapped, {len(unmapped)} left out (trace only / Orta only), "
          f"{len(va_lines)} PROG_VA pairs checked")
    files = ((OUT, text), (OUT_NAMES, names_text))
    if args.check:
        for path, want in files:
            have = open(path, encoding="utf-8").read() if os.path.isfile(path) else ""
            if have != want:
                fail(f"{os.path.relpath(path, ROOT)} is out of date: rerun without --check")
        print("src/pd1/orta_hooks.h and hle_names.h are up to date")
        return
    for path, want in files:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(want)
        print(f"wrote {os.path.relpath(path, ROOT)}")


if __name__ == "__main__":
    main()
