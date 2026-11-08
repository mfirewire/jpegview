#!/usr/bin/env python3
"""Guard the line-ending / encoding policy declared in ../.gitattributes.

Run from the repository root:  python3 .github/scripts/check_eol.py

Rules checked on every push / pull request:
  1. No tracked file may have mixed EOL in the index or in the worktree.
  2. Every file with the `text` attribute must be LF-only in the index
     (git add normalises CRLF and stray LF lines to LF).
  3. Every project-owned text file outside the explicit whitelist must be
     valid UTF-8, and must be covered by .gitattributes (so future files with
     a known text extension cannot silently bypass the policy).

`extras/third_party/**` and binary files are deliberately left untouched.
"""
import subprocess
import sys
from pathlib import Path

TEXT_EXT = (
    ".c", ".cc", ".cpp", ".h", ".hh", ".hpp", ".inl", ".rc", ".rc2",
    ".idl", ".def", ".ini", ".tpl", ".txt", ".md", ".yml", ".yaml",
    ".xml", ".props", ".targets", ".vcxproj", ".filters", ".sln",
    ".wxs", ".wxl", ".wixproj", ".html", ".py", ".ps1", ".sh",
    ".bat", ".cmd", ".cmake", ".mak", ".patch",
)

# Files whose bytes must never be re-encoded or EOL-converted (legacy
# single-byte encodings, or stray non-UTF-8 bytes in source comments).
WHITELIST = {
    "CHANGELOG.txt",
    "HowToInstall_ru.txt",
    "src/JPEGView/Config/KeyMap-README_ru.html",
    "src/JPEGView/ZoomNavigator.h",
    "src/WICLoader/stdafx.cpp",
}

THIRD_PARTY = "extras/third_party/"


def git_text(*args):
    return subprocess.check_output(["git", *args], text=True, errors="replace")


def index_blob(path):
    return subprocess.check_output(["git", "cat-file", "blob", ":" + path])


def eol_stats(data):
    crlf = data.count(b"\r\n")
    return crlf, data.count(b"\n") - crlf


def main():
    errors = []

    # 1) git's own EOL report: mixed is forbidden everywhere, including the
    # vendored trees (the two upstream getopt.c files are normalised too).
    for line in git_text("ls-files", "--eol").splitlines():
        left, tab, path = line.partition("\t")
        if not tab:
            continue
        fields = left.split(None, 2)
        if len(fields) < 3:
            continue
        idx, wtk = fields[0], fields[1]
        if "mixed" in idx or "mixed" in wtk:
            errors.append("mixed EOL: %s (%s %s)" % (path, idx, wtk))

    files = [p for p in git_text("ls-files").splitlines() if p]

    # 2) Batch attribute lookup (text set / unset / unspecified).  Use -z with
    # bytes: on Windows, text mode turns "\n" into "\r\n" on stdin and Git then
    # quotes the trailing \r as part of the path, so a line-based query silently
    # reports "unset" for every file.
    payload = b"\0".join(p.encode("utf-8", "surrogateescape") for p in files) + b"\0"
    proc = subprocess.run(
        ["git", "check-attr", "text", "--stdin", "-z"],
        input=payload,
        capture_output=True,
        check=True,
    )
    parts = proc.stdout.split(b"\0")
    is_text = {}
    for i in range(0, len(parts) - 2, 3):
        key = parts[i].decode("utf-8", "surrogateescape")
        is_text[key] = parts[i + 2].decode("utf-8", "surrogateescape").strip()

    text_count = 0
    for path in files:
        attr = is_text.get(path, "unspecified")
        owned = not path.startswith(THIRD_PARTY)
        known_ext = path.lower().endswith(TEXT_EXT)

        if attr == "set":
            text_count += 1
            blob = index_blob(path)
            crlf, lf = eol_stats(blob)
            if crlf:
                errors.append("CRLF in index for text file: %s (%d CRLF lines)" % (path, crlf))
            work = Path(path).read_bytes() if Path(path).is_file() else b""
            w_crlf, w_lf = eol_stats(work)
            if w_crlf and w_lf:
                errors.append("mixed EOL in worktree: %s" % path)
            if owned and path not in WHITELIST and known_ext:
                try:
                    blob.decode("utf-8")
                except UnicodeDecodeError:
                    errors.append("non-UTF-8 text outside whitelist: %s" % path)
        elif owned and known_ext and path not in WHITELIST:
            errors.append("text file not covered by .gitattributes: %s" % path)

    print("checked %d tracked files, %d with the text attribute" % (len(files), text_count))
    if errors:
        print("EOL/ENCODING GUARD FAILED (%d issues):" % len(errors))
        for e in errors[:40]:
            print("  -", e)
        if len(errors) > 40:
            print("  ... %d more" % (len(errors) - 40))
        return 1
    print("EOL/ENCODING GUARD OK: no mixed EOL, text files LF-only in the index, UTF-8 clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
