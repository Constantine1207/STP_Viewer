#!/usr/bin/env python3
"""Create a clean, deterministic source ZIP from explicitly allowed project paths."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DIRECTORIES = (".github", "cmake", "doc", "i18n", "images", "resources", "samples", "scripts", "src", "tests")
FILES = (".gitignore", "CMakeLists.txt", "LICENSE.txt", "README.md", "STP_README.md",
         "README-Mayo.md", "CONTRIBUTING.md", "THIRD_PARTY_NOTICES.md", "VALIDATION.md", "WINDOWS_BUILD.md", "build-windows.cmd")
EXCLUDE = {".DS_Store", "__pycache__", ".git", ".deps", ".tools", ".agents", ".codex"}


def source_files():
    for name in FILES:
        path = ROOT / name
        if not path.is_file():
            raise RuntimeError(f"Required source file is missing: {name}")
        yield path
    for name in DIRECTORIES:
        directory = ROOT / name
        if not directory.exists():
            continue
        for path in sorted(directory.rglob("*")):
            relative = path.relative_to(ROOT)
            if any(part in EXCLUDE or part.startswith("._") for part in relative.parts):
                continue
            if path.is_symlink():
                raise RuntimeError(f"Refusing to archive a symbolic link: {relative}")
            if path.is_file() and path.suffix not in {".pyc", ".log", ".tmp", ".bak"}:
                yield path


def add(archive, name, data, executable=False):
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
    archive.writestr(info, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    folder = "STPViewer-1.0.0-source"
    destination = args.output / (folder + ".zip")
    manifest = []
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in sorted(source_files()):
            relative = path.relative_to(ROOT).as_posix()
            data = path.read_bytes()
            add(archive, folder + "/" + relative, data, bool(path.stat().st_mode & stat.S_IXUSR))
            manifest.append({"path": relative, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        add(archive, folder + "/SOURCE_MANIFEST.json",
            (json.dumps({"files": manifest}, ensure_ascii=False, indent=2) + "\n").encode())
    with zipfile.ZipFile(destination) as archive:
        if archive.testzip() is not None:
            raise RuntimeError("Source ZIP CRC verification failed")
    sha256 = hashlib.sha256(destination.read_bytes()).hexdigest()
    destination.with_suffix(".zip.sha256").write_text(f"{sha256}  {destination.name}\n")
    print(json.dumps({"archive": str(destination), "files": len(manifest), "sha256": sha256}, indent=2))


if __name__ == "__main__":
    main()
