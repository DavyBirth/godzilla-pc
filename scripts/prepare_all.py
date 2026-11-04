#!/usr/bin/env python3
"""Prepare external Godzilla files on Linux or Windows; does not compile a runtime.
SPDX-License-Identifier: GPL-2.0-or-later
"""

import argparse
from pathlib import Path
import subprocess
import sys

from game_check import problem, explain
from link_modules import FIRMWARE_MODULES


def overlap(first, second):
    return first == second or first in second.parents or second in first.parents


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("base", "patch", "firmware", "work"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate inputs and show steps without writing files",
    )
    args = parser.parse_args()
    sources = [args.base.resolve(), args.patch.resolve(), args.firmware.resolve()]
    work = args.work.resolve()
    repo = Path(__file__).resolve().parent.parent
    if work == repo or repo in work.parents:
        parser.error("work folder must be outside the repository")
    for source in sources:
        if not source.is_dir():
            parser.error(f"source folder not found: {source}")
        if overlap(source, work):
            parser.error(f"work must not overlap a source folder: {source}")
    for source in sources[:2]:
        for name in ("eboot.bin", "sce_sys/param.sfo"):
            if not (source / name).is_file():
                parser.error(f"required file missing: {source / name}")
    for name in FIRMWARE_MODULES:
        if not (sources[2] / name).is_file():
            parser.error(f"required module missing: {sources[2] / name}")
    unsupported = problem(sources[1])
    if unsupported:
        parser.error(explain(*unsupported))
    scripts = Path(__file__).resolve().parent
    game, out = work / "CUSA02046", work / "out"
    steps = (
        ("merge_game.py", sources[0], sources[1], game),
        ("prepare.py", game, "--out", out),
        ("link_modules.py", game, "--out", out, "--firmware", sources[2]),
        ("content_profile.py", game, "--out", out),
    )
    for script, *arguments in steps:
        command = [sys.executable, str(scripts / script), *map(str, arguments)]
        print(f"{script}: {arguments}", flush=True)
        if not args.dry_run:
            subprocess.run(command, check=True)
    print(
        "Inputs validated; no files written."
        if args.dry_run
        else f'Prepared image: {out / "boot-linked.bin"}'
    )
    print("This prepares game data only. A Linux runtime is not yet implemented or validated.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Preparation failed: {error}")
