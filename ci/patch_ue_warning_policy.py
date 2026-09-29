#!/usr/bin/env python3
"""Stop Unreal's header from escalating C4582/C4583 to errors.

Unreal's WindowsPlatformCompilerSetup.hpp raises a long list of MSVC warnings to
errors, including 4582 and 4583 ("constructor/destructor is not implicitly
called"). Both are level-4 warnings that MSVC leaves off by default, and they
fire on constructs inside the third-party headers this mod compiles through
Unreal's own: fmt holds a std::locale in an anonymous union inside
detail::get_locale, and fmt 11 keeps a string_view in detail::arg_ref and a
monostate in detail::value the same way.

Suppressing them from the mod is not possible: the #pragma in this header runs
after the command line and after any header included earlier, so a /wd flag or
an earlier #pragma is overridden. Removing just these two numbers from the
escalation list restores MSVC's default (off) and leaves every other Unreal
warning rule untouched.

Usage: patch_ue_warning_policy.py <path/to/WindowsPlatformCompilerSetup.hpp>
"""

import pathlib
import sys

MARKER = "scum-simple-rcon: stop escalating C4582/C4583"
PREFIX = "#pragma warning (error:"
DROP = {"4582", "4583"}


def patch(path: pathlib.Path) -> str:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return "already patched"

    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    edits = []
    removed = 0

    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith(PREFIX) or not stripped.endswith(")"):
            continue
        tokens = stripped.partition(":")[2].rstrip(")").split()
        kept = [token for token in tokens if token not in DROP]
        if len(kept) == len(tokens):
            continue
        removed += len(tokens) - len(kept)
        indent = line[: len(line) - len(line.lstrip())]
        rebuilt = f"{indent}{PREFIX} {' '.join(kept)} ){newline}"
        edits.append((index, f"{indent}// {MARKER}{newline}", rebuilt))

    if removed != len(DROP):
        raise SystemExit(
            f"{path}: expected to drop {sorted(DROP)}, dropped {removed} entries"
        )

    for index, comment, rebuilt in sorted(edits, reverse=True):
        lines[index] = rebuilt
        lines.insert(index, comment)

    patched = "".join(lines)
    path.write_text(patched, encoding="utf-8", newline="")
    return "patched"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__)
        return 2
    path = pathlib.Path(argv[1])
    if not path.is_file():
        print(f"error: {path} does not exist", file=sys.stderr)
        return 1
    print(f"{path}: {patch(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
