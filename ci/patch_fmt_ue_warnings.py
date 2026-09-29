#!/usr/bin/env python3
"""Guard fmt::detail::get_locale against Unreal's warning policy.

Unreal's WindowsPlatformCompilerSetup.hpp escalates C4582 and C4583 to errors
("constructor/destructor is not implicitly called"). fmt holds a std::locale in
an anonymous union inside detail::get_locale, which is exactly the pattern those
diagnostics fire on, so any translation unit that reaches fmt/chrono.h after the
Unreal headers fails to compile.

Running the mod with /wd4582 would not help: the pragma in the Unreal header
comes later on the command line's terms and re-arms both diagnostics. The guard
therefore has to sit in the fmt header itself, after the Unreal pragma has run,
which is also how JetBrains fixed the fmt copy bundled in RiderLink.

Usage: patch_fmt_ue_warnings.py <path/to/fmt/include/fmt/chrono.h>
"""

import pathlib
import sys

MARKER = "scum-simple-rcon: UE escalates C4582/C4583"
PUSH = [
    "#if defined(_MSC_VER)",
    "#pragma warning(push)",
    "#pragma warning(disable : 4582 4583)  // " + MARKER,
    "#endif",
]
POP = [
    "#if defined(_MSC_VER)",
    "#pragma warning(pop)",
    "#endif",
]


def patch(path: pathlib.Path) -> str:
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return "already patched"

    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)

    def line_index(predicate, description):
        for index, line in enumerate(lines):
            if predicate(line.strip()):
                return index
        raise SystemExit(f"{path}: cannot find {description}")

    start = line_index(lambda line: line == "class get_locale {", "class get_locale {")
    after = line_index(
        lambda line: line.startswith("struct duration_formatter {"),
        "struct duration_formatter {",
    )
    # The class is followed by its template header and a blank line, so the
    # closing brace is the first "};" above duration_formatter.
    close = next(
        (index for index in range(after - 1, start, -1) if lines[index].strip() == "};"),
        None,
    )
    if close is None:
        raise SystemExit(f"{path}: cannot find the brace closing get_locale")

    lines[start:start] = [entry + newline for entry in PUSH] + [newline]
    close += len(PUSH) + 1
    lines[close + 1:close + 1] = [newline] + [entry + newline for entry in POP]

    patched = "".join(lines)
    for expected in ("warning(push)", "warning(pop)"):
        if expected not in patched:
            raise SystemExit(f"{path}: {expected} did not land")
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
