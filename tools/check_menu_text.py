#!/usr/bin/env python3
"""Checks that the options menu's text fits its panel (src/gfx/overlay.c).

The options panel is 34 characters wide: a label from column 2 and its
value right-aligned, with "< >" around the value of the selected item
(4 more columns), so label + value must stay within 24; a slider's label
ends before its bar at column 18 (15 characters). Help is word-wrapped on
two lines of 31. Values printed with %s or %d count as 8 and 3 characters.
Run after changing a label, value or help line; exits 1 on a misfit.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(ROOT, "src", "gfx", "overlay.c"), encoding="utf-8").read()


def body(name):
    i = src.index(name)
    return src[i:src.index("\n}\n", i)]


def wraps(text, cols=31, lines=2):
    n, cur = 0, ""
    for w in text.split(" "):
        if len(cur) + len(w) + (1 if cur else 0) <= cols:
            cur = cur + " " + w if cur else w
        else:
            n, cur = n + 1, w
    return n + 1 <= lines


labels = dict(re.findall(r'case (ITEM_\w+):\s*return "([^"]*)"', body("static const char *label_of")))
helps = dict(re.findall(r'case (ITEM_\w+):\s*return "([^"]*)"', body("static const char *help_of")))
links = set(re.findall(r"ITEM_\w+", body("static bool is_link")))
values = {}
for m in re.finditer(r"case (ITEM_\w+):(.*?)(?=\n    case ITEM_|\n    default|\Z)", body("static float value_of"), re.S):
    lits = [re.sub(r"%[-0-9.]*s", "X" * 8, re.sub(r"%[-0-9.]*[dug]", "100", x)).replace("%%", "%")
            for x in re.findall(r'"([^"]*)"', m.group(2)) if x]
    values[m.group(1)] = (lits, "return" in m.group(2) and "f;" in m.group(2))

bad = []
for item, text in helps.items():
    if not wraps(text):
        bad.append("help of %s does not fit two lines of 31: %r" % (item, text))
for item, label in labels.items():
    lits, slider = values.get(item, ([], False))
    widest = max([len(x) for x in lits] + [0])
    room = 24 if item not in links else 28
    if slider and len(label) > 15:
        bad.append("%s: label %r longer than 15 next to its slider" % (item, label))
    elif len(label) + widest > room:
        bad.append("%s: label %r and value %r wider than %d" % (item, label, max(lits, key=len), room))
for b in bad:
    print(b)
print("menu text: %d labels, %d help lines, %s" % (len(labels), len(helps), "all fit" if not bad else "%d misfit" % len(bad)))
sys.exit(1 if bad else 0)
