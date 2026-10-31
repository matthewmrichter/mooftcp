#!/bin/sh
# check_pcrel.sh <objdump> <linked .flt.gdb> <object dir>
# The cdev code must run without Retro68Relocate (cdev_start.s). Fails if
#  1. an object file of ours has an absolute relocation (R_68K_32 / R_68K_16 / R_68K_8), or
#  2. the linked block has an absolute relocation (code or data) in any function or object that
#     the code reachable from _start uses (library code such as memcpy or libgcc's helpers
#     included). Retro68's runtime and Interface glue are linked into every flat block but are never
#     reached from _start, so their relocations are harmless.
set -e
OBJDUMP="$1"; ELF="$2"; DIR="$3"
bad=0
for o in $(find "$DIR" -name '*.obj' -o -name '*.o' | sort); do
  if "$OBJDUMP" -r "$o" | grep -E 'R_68K_(32|16|8) ' >/dev/null; then
    echo "check_pcrel: absolute relocation in $o:" >&2
    "$OBJDUMP" -r "$o" | grep -E 'R_68K_(32|16|8) ' >&2
    bad=1
  fi
done
tmp="$ELF.chk"
"$OBJDUMP" -r "$ELF" > "$tmp.r"
"$OBJDUMP" -t "$ELF" > "$tmp.t"
"$OBJDUMP" -d "$ELF" > "$tmp.d"
"$OBJDUMP" -h "$ELF" > "$tmp.h"
if ! python3 -I - "$tmp.r" "$tmp.t" "$tmp.d" "$tmp.h" <<'PY'
import re, sys
# symbols of the allocated sections, sorted: each address belongs to the last symbol at or below it
syms = []
for l in open(sys.argv[2]):
    p = l.split()
    if len(p) >= 4 and re.fullmatch(r"[0-9a-f]{8}", p[0]) and (".text" in p or ".data" in p or ".bss" in p):
        name = p[-1]
        if name.startswith(".") or name in ("_sdata", "_edata", "_etext", "__bss_start", "_end"):
            continue
        syms.append((int(p[0], 16), name))
syms.sort()
def owner(a):
    best = "?"
    for (s, n) in syms:
        if s <= a: best = n
        else: break
    return best
# call/reference graph from the disassembly: any <sym> operand is an edge
edges, cur = {}, None
for l in open(sys.argv[3]):
    m = re.match(r"^([0-9a-f]+) <([^>]+)>:", l)
    if m:
        cur = owner(int(m.group(1), 16)); edges.setdefault(cur, set()); continue
    # edges: branches and calls, and PC-relative operands (an absolute operand such as 0x210 is a
    # low-memory global, which objdump also prints as <symbol> because the block is linked at 0)
    if cur and (re.search(r"\t(j?bsr|jsr|jmp|bra|b[a-z][a-z]|db[a-z]+)[a-z.]*\s", l) or "%pc" in l):
        for t in re.findall(r"\b([0-9a-f]+) <[^>]+>", l):
            edges[cur].add(owner(int(t, 16)))
seen, todo = set(), ["_start"]
while todo:
    n = todo.pop()
    if n in seen: continue
    seen.add(n)
    todo.extend(edges.get(n, ()))
# relocation offsets are section-relative in objdump -r: add the section's address (objdump -h)
vma, sec, bad = {}, None, 0
for l in open(sys.argv[4]):
    p = l.split()
    if len(p) >= 4 and p[0].isdigit():
        vma[p[1]] = int(p[3], 16)
for l in open(sys.argv[1]):
    m = re.match(r"RELOCATION RECORDS FOR \[(.*)\]", l)
    if m:
        sec = m.group(1); continue
    p = l.split()
    if sec in (".text", ".data") and len(p) >= 2 and re.fullmatch(r"[0-9a-f]{8}", p[0]) and re.fullmatch(r"R_68K_(32|16|8)", p[1]):
        a = int(p[0], 16)
        if a < vma.get(sec, 0): a += vma[sec]
        o = owner(a)
        if o in seen:
            print("check_pcrel: absolute relocation at %s in %s (reachable from _start)" % (p[0], o), file=sys.stderr)
            bad = 1
print("check_pcrel: %d functions/objects reachable from _start: %s" % (len(seen), " ".join(sorted(seen))))
sys.exit(bad)
PY
then bad=1; fi
rm -f "$tmp.r" "$tmp.t" "$tmp.d" "$tmp.h"
[ "$bad" = 0 ] || { echo "check_pcrel: the cdev code is not position independent" >&2; exit 1; }
echo "check_pcrel: no absolute relocation in code reachable from _start"
