#!/usr/bin/env python3
"""Doxygen INPUT_FILTER for the public headers (wired up via Doxyfile FILTER_PATTERNS).

The engine's house comment style is plain '//' blocks, which Doxygen ignores. This filter
promotes full-line '//' comments to '///' so the existing prose becomes the generated API
reference — no header rewrite, no second comment dialect to maintain. The banner block at
the top of each header additionally gets a '\\file' tag so Doxygen attaches it to the file
page instead of to whatever declaration happens to come first (usually the include guard).

Only full-line comments are promoted; trailing comments after code stay invisible to
Doxygen (promoting those would need '//!<' and risks mangling code that contains '//').
Promoted prose additionally gets '#', '<', '>' escaped outside `code` spans — the comments
predate Doxygen, so '#ifdefs' must not become a link request nor '<cstdint>' an HTML tag.
(Per-line backtick pairing is assumed: the headers contain no multi-line code fences.)
Usage: doxyfilter.py <file>   (prints the filtered source to stdout; never edits files)
"""
import re
import sys

LEAD = re.compile(r"^(\s*)//(?!/|!)")


def escape_prose(text):
    parts = text.split("`")
    for i in range(0, len(parts), 2):  # even indices are outside `code` spans
        parts[i] = parts[i].replace("#", r"\#").replace("<", r"\<").replace(">", r"\>")
    return "`".join(parts)


def main(path):
    in_banner = True      # scanning the top-of-file comment block
    banner_open = False   # emitted the \file tag for it
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            stripped = line.strip()
            if in_banner:
                if stripped.startswith("//"):
                    if not banner_open:
                        sys.stdout.write("/// \\file\n")
                        banner_open = True
                elif stripped:
                    in_banner = False
            m = LEAD.match(line)
            if m:
                line = "%s///%s" % (m.group(1), escape_prose(line[m.end():]))
            sys.stdout.write(line)


if __name__ == "__main__":
    main(sys.argv[1])
