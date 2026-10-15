#!/usr/bin/env python3
"""Translation completeness check, run by ctest.

For every booru-explorer_<locale>.ts: each message must have a non-empty
translation, printf-style placeholders (%1..%9) and & accelerators must
match the source, and the file must declare its language.
Usage: check_translations.py <translations-dir>
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

PLACEHOLDER = re.compile(r"%[1-9]")
# A real mnemonic is & followed by a non-space ("Browse & downloads" keeps a
# literal-style & before a space, matching how Qt renders the source).
MNEMONIC = re.compile(r"&(?!\s)")


def failures(translations_dir: Path) -> list[str]:
    problems: list[str] = []
    files = sorted(translations_dir.glob("booru-explorer_*.ts"))
    if not files:
        return ["no locale .ts files found"]
    for path in files:
        root = ET.parse(path).getroot()
        lang = root.get("language", "")
        if not lang:
            problems.append(f"{path.name}: missing language attribute")
        count = 0
        for ctx in root.iter("context"):
            name = ctx.findtext("name")
            for msg in ctx.iter("message"):
                if msg.get("numerus") == "yes":
                    continue
                src = (msg.findtext("source") or "")
                tr_el = msg.find("translation")
                tr = (tr_el.text or "") if tr_el is not None else ""
                count += 1
                if not tr.strip():
                    problems.append(f"{path.name} [{name}]: empty: {src[:60]!r}")
                    continue
                if set(PLACEHOLDER.findall(src)) != set(PLACEHOLDER.findall(tr)):
                    problems.append(
                        f"{path.name} [{name}]: placeholder mismatch: "
                        f"{src[:60]!r} -> {tr[:60]!r}")
                if len(MNEMONIC.findall(src)) != len(MNEMONIC.findall(tr)):
                    problems.append(
                        f"{path.name} [{name}]: accelerator count mismatch: "
                        f"{src[:60]!r} -> {tr[:60]!r}")
        if count == 0:
            problems.append(f"{path.name}: no messages found")
    return problems


def main() -> int:
    problems = failures(Path(sys.argv[1]))
    for p in problems:
        print("FAIL:", p)
    if problems:
        print(f"{len(problems)} translation problem(s)")
        return 1
    print("translations OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
