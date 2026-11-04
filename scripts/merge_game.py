#!/usr/bin/env python3
"""Build the working game folder: the base game with the whole 01.01 update copied over it.

The update is more than eboot.bin and sce_sys: it also replaces ~60 data files (.pac) and
sce_module, as shadPS4 does when it mounts the patch over app0. The source folders are only
read. Files are real copies (not links), so nothing the port does can touch the originals.
Re-running copies only files whose size or modification time changed.
Output: <dest>/ (the merged game) and <dest>/../merge.json (which file came from where).
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil


def files(root):
    return {p.relative_to(root): p for p in root.rglob("*") if p.is_file()}


def same(src, dst):
    if not dst.is_file():
        return False
    a, b = src.stat(), dst.stat()
    return a.st_size == b.st_size and int(a.st_mtime) == int(b.st_mtime)


def merge(base, patch, dest):
    base, patch, dest = base.resolve(), patch.resolve(), dest.resolve()
    for source in (base, patch):
        if dest == source or source in dest.parents or dest in source.parents:
            raise ValueError(f"destination must be outside the game folders: {dest}")
        if not (source / "eboot.bin").is_file():
            raise ValueError(f"no eboot.bin in {source}")
    plan = files(base)
    origin = {rel: "base" for rel in plan}
    for rel, path in files(patch).items():
        origin[rel] = "patch-new" if rel not in plan else "patch"
        plan[rel] = path
    copied = skipped = 0
    for rel, src in sorted(plan.items()):
        dst = dest / rel
        if same(src, dst):
            skipped += 1
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, 0o644)  # never inherit a read-only flag that would block re-runs
        copied += 1
    stale = sorted(str(rel) for rel in files(dest) if rel not in plan)
    report = dict(
        base=str(base),
        patch=str(patch),
        dest=str(dest),
        eboot_sha256=hashlib.sha256((dest / "eboot.bin").read_bytes()).hexdigest(),
        total_files=len(plan),
        total_bytes=sum(p.stat().st_size for p in plan.values()),
        from_patch=sorted(str(r) for r, o in origin.items() if o == "patch"),
        new_in_patch=sorted(str(r) for r, o in origin.items() if o == "patch-new"),
        unexpected_files_in_dest=stale,
    )
    (dest.parent / "merge.json").write_text(json.dumps(report, indent=2) + "\n")
    print(
        f"Merged {len(plan)} files ({report['total_bytes'] / 2**30:.1f} GiB): "
        f"{len(report['from_patch'])} replaced and {len(report['new_in_patch'])} added by the update; "
        f"{copied} copied, {skipped} already up to date"
    )
    if stale:
        print(f"Warning: {len(stale)} files in {dest} are in neither source (see merge.json)")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("base", type=Path)
    p.add_argument("patch", type=Path)
    p.add_argument("dest", type=Path)
    a = p.parse_args()
    try:
        merge(a.base, a.patch, a.dest)
    except (ValueError, OSError) as error:
        p.exit(1, f"merge failed: {error}\n")
