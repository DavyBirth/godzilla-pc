#!/usr/bin/env python3
"""The game files this port runs: Godzilla CUSA02046 with the 01.01 update merged in
(scripts/merge_game.py).

Hooks and patches will use the addresses of this one executable. The check compares the loaded
executable image, whose hash is the same whatever tool dumped it (the SELF headers around it
differ). GZ_SKIP_GAME_CHECK=1 skips it.

From bbport (github.com/deadinside28/bloodborne_pc, scripts/game_check.py, GPL-2.0-or-later).
"""

import hashlib
import os
from pathlib import Path

SUPPORTED_TITLE = "CUSA02046"
SUPPORTED_VERSION = "01.01"
SUPPORTED_IMAGE = "aa191628d842649f553ffffcff88bd5c7d977d12a45aed7b2b9743cd2f81f77c"


def image_sha256(game):
    """The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses)."""
    from prepare import parse_self, span  # same directory

    elf, header, ph, _segments, _missing = parse_self((Path(game) / "eboot.bin").read_bytes())
    loads = [p for p in ph if p["type"] in (1, 0x61000010)]
    image = bytearray(max(p["vaddr"] + p["memsz"] for p in loads))
    for p in loads:
        image[p["vaddr"] : p["vaddr"] + p["filesz"]] = span(elf, p["offset"], p["filesz"])
    return hashlib.sha256(image).hexdigest()


def problem(game, image_hash=None):
    """None for the supported game, else (kind, title, version): kind is 'missing_update' (base
    game), 'wrong_eboot' (param.sfo says 01.01, eboot.bin is another version), 'other_title'
    (another edition or region) or 'unreadable'."""
    if os.environ.get("GZ_SKIP_GAME_CHECK") == "1":
        return None
    from prepare import sfo

    try:
        info = sfo((Path(game) / "sce_sys/param.sfo").read_bytes())
    except (OSError, ValueError, IndexError):
        info = {}
    title, version = info.get("TITLE_ID", "?"), info.get("APP_VER", "?")
    try:
        if (image_hash or image_sha256(game)) == SUPPORTED_IMAGE:
            return None
    except (OSError, ValueError, IndexError, StopIteration, KeyError):
        return "unreadable", title, version
    if title != SUPPORTED_TITLE:
        return "other_title", title, version
    if version != SUPPORTED_VERSION:
        return "missing_update", title, version
    return "wrong_eboot", title, version


def explain(kind, title, version):
    """What is wrong and what to do, for the log (English)."""
    found = f"Found {title} version {version} (sce_sys/param.sfo)."
    return {
        "missing_update": f"{found} The 01.01 update must be merged into the game folder: run "
        "scripts/merge_game.py with the base game and CUSA02046-patch folders.",
        "wrong_eboot": f"{found} param.sfo is from 01.01 but eboot.bin is not: merge the game "
        "again with scripts/merge_game.py.",
        "other_title": f"{found} Only Godzilla {SUPPORTED_TITLE} with update 01.01 is supported; "
        "other editions and regions have a different executable.",
        "unreadable": f"{found} eboot.bin could not be read as a decrypted PS4 executable: dump "
        "the game and the 01.01 update again.",
    }[kind]
