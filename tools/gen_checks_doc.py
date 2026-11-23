#!/usr/bin/env python3
"""Writes docs/checks.md from the vehicle profiles, so the reference cannot drift from what runs.

    python3 tools/gen_checks_doc.py            rewrite docs/checks.md
    python3 tools/gen_checks_doc.py --check    exit 1 if docs/checks.md is out of date (used in CI)

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = [
    ("generic-obd2", "Any car", "standard OBD-II readings"),
    ("vw-ea888-gen3", "Volkswagen EA888 Gen 3", "readings specific to the 1.8 and 2.0 TSI"),
]
ORDER = ["Boost", "Fuel mixture", "Fuel delivery", "Ignition", "Valve timing", "Cooling", "Lubrication", "Electrical", "Exhaust", "Diagnostics"]
STATS = {
    "mean": "the average of",
    "min": "the lowest value of",
    "max": "the highest value of",
    "p50": "the median of",
    "p90": "the 90th percentile of",
    "p95": "the 95th percentile of",
    "p99": "the 99th percentile of",
    "last": "the latest value of",
    "first": "the first value of",
    "delta": "the change over the drive in",
    "range": "the spread between the highest and lowest",
    "time": "the time spent with",
    "fraction": "the share of the time with",
    "time_to": "the time until",
    "count": "the number of times",
}
SEVERITIES = ["advisory", "warning", "critical"]


def number(v):
    return ("%g" % v).replace("-", "−")


def limits(check, low):
    """One table row: the advisory, warning and critical limits on one side."""
    cells = []
    for sev in SEVERITIES:
        hit = [l for l in check["levels"] if l["severity"] == sev and (("lt" in l or "le" in l) == low)]
        if not hit:
            cells.append("")
            continue
        l = hit[0]
        word = "below" if low else "above"
        value = l.get("gt", l.get("ge", l.get("lt", l.get("le"))))
        unit = check.get("unit", "")
        unit = unit.replace("Δ", "")
        if unit == "s":
            cells.append(f"{word} {seconds(value)}")
            continue
        cells.append(f"{word} {number(value)}{'' if unit in ('%', '°') else ' '}{unit}".rstrip())
    return cells


def seconds(s):
    s = float(s)
    if s >= 120 and s % 60 == 0:
        return f"{int(s // 60)} minutes"
    return f"{number(s)} seconds"


def render():
    out = [
        "# Checks",
        "",
        "Every health check CANary ships with, generated from the vehicle profiles by",
        "`tools/gen_checks_doc.py`. Limits are in the units the profiles use (metric); the web page",
        "and Glovebox show them in the units you chose.",
        "",
        "A check counts only while its conditions hold, and gives a verdict once they have held for",
        "long enough in total. Before that it is listed as waiting. If the car does not report a",
        "reading the check needs, it is listed as not available.",
        "",
        "The limits are starting points drawn from service literature and owner experience, set so",
        "that a healthy engine passes with room to spare. They are not manufacturer test",
        "specifications. If your car proves one of them wrong, please",
        "[open an issue](https://github.com/au12/CANary/issues).",
        "",
    ]
    checks = []
    for profile_id, who, what in SOURCES:
        doc = json.loads((ROOT / "profiles" / f"{profile_id}.json").read_text(encoding="utf-8"))
        for m in doc.get("monitors", []):
            checks.append((m, profile_id, who))
    categories = sorted({m.get("category", "General") for m, _, _ in checks}, key=lambda c: (ORDER.index(c) if c in ORDER else 99, c))

    out += ["| Check | Reports | Runs on | Worst level |", "| --- | --- | --- | --- |"]
    for cat in categories:
        for m, profile_id, who in checks:
            if m.get("category", "General") != cat:
                continue
            worst = max((SEVERITIES.index(l["severity"]) for l in m["levels"]), default=0)
            reports = m.get("alert", m["title"])
            if "low" in m and m["low"].get("alert"):
                reports += " / " + m["low"]["alert"]
            out.append(f"| [{m['title']}](#{anchor(m['title'])}) | {reports} | {who} | {SEVERITIES[worst].capitalize()} |")
    out.append("")

    for cat in categories:
        out += [f"## {cat}", ""]
        for m, profile_id, who in checks:
            if m.get("category", "General") != cat:
                continue
            out += [f"### {m['title']}", ""]
            alert = m.get("alert", m["title"])
            two_sided = "low" in m
            if two_sided:
                out.append(f"Reports **{alert}** on the high side and **{m['low'].get('alert', alert)}** on the low side.")
            else:
                out.append(f"Reports **{alert}**.")
            out.append("")
            stat = m.get("stat", "mean")
            out += ["| | |", "| --- | --- |"]
            out.append(f"| Counts while | {m.get('needs', 'the engine is running')}: `{cell(m.get('when', '1'))}` |")
            if stat == "time_to":
                out.append(f"| Measures | {STATS[stat]} `{cell(m['value'])}` holds |")
            else:
                out.append(f"| Measures | {STATS.get(stat, stat)} `{cell(m['value'])}` |")
            wait = []
            if float(m.get("settle_s", 0)) > 0:
                wait.append(f"skips the first {seconds(m['settle_s'])} each time the conditions begin")
            wait.append(f"needs {seconds(m.get('min_time_s', 10))} in total")
            out.append(f"| Verdict | {'; '.join(wait)} |")
            high = limits(m, False)
            if any(high):
                out.append(f"| Advisory, warning, critical | {', '.join(c if c else 'none' for c in high)} |")
            if two_sided or any(("lt" in l or "le" in l) for l in m["levels"]):
                low = limits(m, True)
                if any(low):
                    label = "Low side" if any(high) else "Advisory, warning, critical"
                    out.append(f"| {label} | {', '.join(c if c else 'none' for c in low)} |")
            out.append(f"| Profile | `{profile_id}`, id `{m['id']}` |")
            out.append("")
            if m.get("detail"):
                out += [m["detail"], ""]
            if m.get("causes"):
                out += ["Common causes: " + "; ".join(lower_first(c) for c in m["causes"]) + ".", ""]
            if two_sided:
                low = m["low"]
                if low.get("detail"):
                    out += [f"On the low side: {low['detail']}", ""]
                if low.get("causes"):
                    out += ["Common causes: " + "; ".join(lower_first(c) for c in low["causes"]) + ".", ""]
    return "\n".join(out).rstrip() + "\n"


def cell(expr):
    """An expression inside a table cell: a bare | would end the cell."""
    return expr.replace("|", "\\|")


def anchor(title):
    keep = "".join(ch for ch in title.lower() if ch.isalnum() or ch in " -")
    return keep.replace(" ", "-")


def lower_first(text):
    # keep acronyms and proper names: only lower a plain capitalised first word
    first = text.split(" ", 1)[0]
    if first.isupper() or any(ch.isdigit() for ch in first) or first in ("PCV", "Volkswagen"):
        return text
    return text[0].lower() + text[1:]


def main():
    path = ROOT / "docs" / "checks.md"
    text = render()
    if "--check" in sys.argv:
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            print("docs/checks.md is out of date: run python3 tools/gen_checks_doc.py", file=sys.stderr)
            return 1
        return 0
    path.write_text(text, encoding="utf-8")
    print(f"wrote {path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
