#!/usr/bin/env python3
"""Create a portable ZIP with standard '/' paths and a single root folder."""
import argparse
import json
import os
from pathlib import Path
import tempfile
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    root, archive = args.directory.resolve(), args.archive.resolve()
    if not root.is_dir():
        parser.error("directory must be an existing folder")
    if archive.is_relative_to(root):
        parser.error("archive must be outside the source directory")
    archive.parent.mkdir(parents=True, exist_ok=True)
    entries = [root, *sorted(root.rglob("*"), key=lambda p: p.as_posix())]
    if any(path.is_symlink() for path in entries):
        parser.error("source tree must not contain symbolic links")
    descriptor, temporary = tempfile.mkstemp(prefix=".stp-package-", suffix=".zip", dir=archive.parent)
    os.close(descriptor)
    temporary = Path(temporary)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in entries:
                name = (Path(root.name) / path.relative_to(root)).as_posix()
                bundle.write(path, name)
        with zipfile.ZipFile(temporary) as bundle:
            bad_entry = bundle.testzip()
            if bad_entry is not None:
                raise RuntimeError(f"ZIP CRC verification failed: {bad_entry}")
            if any("\\" in name for name in bundle.namelist()):
                raise RuntimeError("ZIP contains a nonstandard path separator")
        os.replace(temporary, archive)
        print(json.dumps({"archive": str(archive), "entries": len(entries), "crc": "passed"}))
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
