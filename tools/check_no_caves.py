#!/usr/bin/env python3
"""
Build-verification check: fail if any active reference to the removed
cave system remains in the source tree.

Scans src/ and assets/shaders/ for cave identifiers. Lines that are pure
comments explicitly noting the removal (containing "no cave" / "removed")
are ignored, as are matches inside this script itself.

Exit code 0 = clean, 1 = cave references found.
Run from the project root:  python tools/check_no_caves.py
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCAN_DIRS = ["src", "assets/shaders"]
SCAN_EXT = (".cpp", ".h", ".hpp", ".vert", ".frag", ".glsl")

# Identifiers that must not appear as active references anymore.
PATTERNS = [
    r"\bcaveProgram\b",
    r"\bcaveVAO\b",
    r"\bcaveVBO\b",
    r"\bcaveEBO\b",
    r"\bcaveIndicesCount\b",
    r"\bdetectAndBuildCaves\b",
    r"cave\.vert",
    r"cave\.frag",
    r"\bstruct\s+Cave\b",
]

# A comment line that merely documents the removal is allowed.
ALLOW_SUBSTR = ("no cave", "removed", "without cave", "no-cave")


def is_allowed_comment(line: str) -> bool:
    low = line.lower()
    stripped = low.lstrip()
    is_comment = stripped.startswith("//") or stripped.startswith("#") or stripped.startswith("*")
    return is_comment and any(s in low for s in ALLOW_SUBSTR)


def main() -> int:
    regexes = [re.compile(p) for p in PATTERNS]
    hits = []
    for d in SCAN_DIRS:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for fname in files:
                if not fname.endswith(SCAN_EXT):
                    continue
                path = os.path.join(dirpath, fname)
                try:
                    with open(path, encoding="utf-8", errors="ignore") as fh:
                        for n, line in enumerate(fh, 1):
                            if is_allowed_comment(line):
                                continue
                            for rx in regexes:
                                if rx.search(line):
                                    rel = os.path.relpath(path, ROOT)
                                    hits.append(f"{rel}:{n}: {line.strip()}")
                                    break
                except OSError as e:
                    print(f"warn: could not read {path}: {e}", file=sys.stderr)

    if hits:
        print("CAVE REFERENCES FOUND (build verification failed):")
        for h in hits:
            print("  " + h)
        return 1
    print("check_no_caves: OK — no active cave references found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
