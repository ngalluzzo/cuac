#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import sys


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit("usage: read-release-pin.py PINS_JSON KEY [KEY ...]")
    value: object = json.loads(pathlib.Path(sys.argv[1]).resolve(strict=True).read_text())
    for key in sys.argv[2:]:
        if not isinstance(value, dict) or key not in value:
            raise KeyError(".".join(sys.argv[2:]))
        value = value[key]
    if not isinstance(value, (str, int, float)) or isinstance(value, bool):
        raise TypeError("release pin is not a scalar")
    rendered = str(value)
    if "\n" in rendered or "\r" in rendered:
        raise ValueError("release pin contains a line break")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
