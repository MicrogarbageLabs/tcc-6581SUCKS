#!/usr/bin/env python3
"""sst_fetch.py -- a slice of Tom Harte's 65816 single-step tests, as text.

The suite (github.com/SingleStepTests/65816, MIT) has 512 JSON files of
about 6 MB. This takes the first BYTES of each with an HTTP range request,
keeps the whole tests in it, and writes them to sst/<op>.<mode>.txt for
sst_run, one line per record:

  T <name>
  I pc s p a x y dbr d pbr e n addr val ...     the initial state and RAM
  F pc s p a x y dbr d pbr e n addr val ...     the final state and RAM
  C <cycles>

  python3 sst_fetch.py [bytes per file, default 262144]

Public domain (CC0). No warranty.
"""
import json
import os
import sys
import urllib.request

BASE = "https://raw.githubusercontent.com/SingleStepTests/65816/main/v1/"
BYTES = int(sys.argv[1]) if len(sys.argv) > 1 else 262144
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sst")
REGS = ("pc", "s", "p", "a", "x", "y", "dbr", "d", "pbr", "e")


def tests_in(text):
    """the complete test objects at the start of a truncated JSON array"""
    out, depth, start, in_str, esc = [], 0, None, False, False
    for i, ch in enumerate(text):
        if in_str:
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
        elif ch == "{":
            if depth == 1:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 1 and start is not None:
                out.append(json.loads(text[start:i + 1]))
                start = None
        elif ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
    return out


def state(tag, s):
    vals = [str(s[r]) for r in REGS]
    ram = s["ram"]
    vals.append(str(len(ram)))
    for a, v in ram:
        vals += [str(a), str(v)]
    return tag + " " + " ".join(vals)


def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for op in range(256):
        for mode in ("n", "e"):
            name = "%02x.%s" % (op, mode)
            dst = os.path.join(OUT, name + ".txt")
            if os.path.exists(dst):
                continue
            req = urllib.request.Request(BASE + name + ".json", headers={"Range": "bytes=0-%d" % (BYTES - 1)})
            with urllib.request.urlopen(req) as r:
                text = r.read().decode("utf-8", "replace")
            tests = tests_in(text)
            with open(dst, "w") as f:
                for t in tests:
                    f.write("T " + t["name"].replace(" ", "_") + "\n")
                    f.write(state("I", t["initial"]) + "\n")
                    f.write(state("F", t["final"]) + "\n")
                    f.write("C %d\n" % len(t["cycles"]))
            total += len(tests)
            print(name, len(tests), flush=True)
    print("done:", total, "tests")


if __name__ == "__main__":
    main()
