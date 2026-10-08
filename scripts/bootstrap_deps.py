#!/usr/bin/env python3
"""Build the pinned native dependencies without changing the host installation.

Downloads use fixed upstream tags. Archive SHA-256 values are recorded in
.deps/downloads/SHA256SUMS.json; known hashes in the lock file are enforced.
The first download is authenticated by HTTPS and upstream Qt archive checksums.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import configparser
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "scripts/dependencies.lock.json").read_text())
DEPS = ROOT / ".deps"
PREFIX = DEPS / "install"


def run(args, **kwargs):
    print("+", " ".join(str(x) for x in args), flush=True)
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def source(name):
    spec = LOCK["sources"][name]
    archive = DEPS / "downloads" / f"{name}-{spec['version']}.tar.gz"
    dest = DEPS / "src" / spec["folder"]
    if not archive.exists():
        part = archive.with_suffix(".gz.part")
        run(["curl", "--fail", "--location", "--retry", "2", "--connect-timeout", "20",
             "--max-time", "900", "--silent", "--show-error", spec["url"], "--output", part])
        part.replace(archive)
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    if spec.get("sha256") and checksum != spec["sha256"]:
        raise RuntimeError(f"SHA-256 mismatch: {archive}; remove and download again.")
    if not dest.exists():
        with tarfile.open(archive, "r:gz") as package:
            # Reject archive paths and symlinks escaping the extraction root.
            if sys.version_info >= (3, 12):
                package.extractall(DEPS / "src", filter="data")
            else:
                raise RuntimeError("Python 3.12 or newer is required for safe archive extraction.")
    if not (dest / "CMakeLists.txt").is_file():
        raise RuntimeError(f"Downloaded source tree is incomplete: {dest}")
    return name, str(archive.name), checksum


def configure_build(name, options, jobs, platform):
    src = DEPS / "src" / LOCK["sources"][name]["folder"]
    build = DEPS / "build" / name
    args = ["cmake", "-S", src, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={PREFIX}", f"-DCMAKE_PREFIX_PATH={PREFIX}",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON", "-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON",
            "-DCMAKE_EXPORT_PACKAGE_REGISTRY=OFF"]
    if platform == "macos":
        args += ["-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0"]
    args += options
    run(args)
    run(["cmake", "--build", build, "--parallel", jobs])
    run(["cmake", "--install", build])


def collect_licenses(qt_root):
    out = PREFIX / "share" / "stp-licenses"
    out.mkdir(parents=True, exist_ok=True)
    candidates = {
        "occt": ["LICENSE_LGPL_21.txt", "OCCT_LGPL_EXCEPTION.txt"],
        "libxlsxwriter": ["License.txt"],
        "freetype": ["LICENSE.TXT", "docs/FTL.TXT", "docs/GPLv2.TXT"],
        "zlib": ["LICENSE"],
    }
    for name, files in candidates.items():
        src = DEPS / "src" / LOCK["sources"][name]["folder"]
        for rel in files:
            path = src / rel
            if path.is_file():
                shutil.copy2(path, out / f"{name}-{path.name}")
    for dirname in ("LICENSES", "licenses"):
        if (qt_root / dirname).is_dir():
            shutil.copytree(qt_root / dirname, out / "Qt", dirs_exist_ok=True)
    if (DEPS / "licenses/Qt").is_dir():
        shutil.copytree(DEPS / "licenses/Qt", out / "Qt", dirs_exist_ok=True)
    if (qt_root / "sbom").is_dir():
        shutil.copytree(qt_root / "sbom", out / "Qt-SBOM", dirs_exist_ok=True)
    (out / "DEPENDENCY_VERSIONS.json").write_text(json.dumps(LOCK, indent=2) + "\n")


def configure_private_aqt():
    # aqt 3.3 initializes its default temp directory before processing CLI
    # flags. Configure this private venv copy so it cannot write to ~/Library.
    spec = importlib.util.find_spec("aqt")
    if spec is None or not spec.origin:
        raise RuntimeError("aqtinstall is missing from the project tool environment.")
    settings = Path(spec.origin).parent / "settings.ini"
    if not settings.is_relative_to(ROOT / ".tools"):
        raise RuntimeError("Use the project private Python environment to configure aqt.")
    config = configparser.ConfigParser()
    (DEPS / "downloads/qt").mkdir(parents=True, exist_ok=True)
    config.read(settings)
    config["qtofficial"]["temp_path"] = str(DEPS / "aqt-tmp")
    config["qtofficial"]["cache_path"] = str(DEPS / "aqt-cache")
    config["aqt"]["archive_download_location"] = str(DEPS / "downloads/qt")
    config["aqt"]["always_keep_archives"] = "True"
    config["requests"]["connection_timeout"] = "20"
    config["requests"]["response_timeout"] = "120"
    with settings.open("w") as stream:
        config.write(stream)


def download_qt_licenses():
    """Keep Qt's license texts and third-party copyright inventory in the ZIP."""
    dest = DEPS / "licenses/Qt"
    if (dest / ".complete").is_file():
        return
    import requests
    response = requests.get(
        "https://api.github.com/repos/qt/qtbase/contents/LICENSES?ref=v" + LOCK["qt"]["version"],
        timeout=45)
    response.raise_for_status()
    entries = response.json()
    dest.mkdir(parents=True, exist_ok=True)
    def fetch(entry):
        if entry["type"] != "file":
            return
        result = requests.get(entry["download_url"], timeout=45)
        result.raise_for_status()
        (dest / entry["name"]).write_bytes(result.content)
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as executor:
        list(executor.map(fetch, entries))
    (dest / ".complete").write_text(LOCK["qt"]["version"] + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=["macos", "windows"], required=True)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 4, 8))
    parser.add_argument("--skip-qt", action="store_true")
    parser.add_argument("--download-only", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    for dirname in ("downloads", "src", "build", "install"):
        (DEPS / dirname).mkdir(parents=True, exist_ok=True)
    names = ["freetype", "occt", "libxlsxwriter"]
    if args.platform == "windows":
        names.insert(0, "zlib")
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        result = list(executor.map(source, names))
    (DEPS / "downloads/SHA256SUMS.json").write_text(
        json.dumps({file: sha for _, file, sha in result}, indent=2) + "\n")
    qt = LOCK["qt"]
    host = "mac" if args.platform == "macos" else "windows"
    qt_dir = "macos" if args.platform == "macos" else "msvc2022_64"
    qt_root = DEPS / "qt" / qt["version"] / qt_dir
    if not args.skip_qt and not (qt_root / "lib/cmake/Qt6/Qt6Config.cmake").is_file():
        configure_private_aqt()
        run([sys.executable, "-m", "aqt", "install-qt", host, "desktop", qt["version"],
             qt[args.platform + "_arch"], "--outputdir", DEPS / "qt", "--base", qt["base"],
             "--timeout", "120", "--archives", *qt["archives"]])
    if not args.skip_qt:
        download_qt_licenses()
    if args.download_only:
        return
    if args.platform == "windows":
        configure_build("zlib", ["-DBUILD_SHARED_LIBS=OFF", "-DZLIB_BUILD_EXAMPLES=OFF"], args.jobs, args.platform)
    configure_build("freetype", ["-DBUILD_SHARED_LIBS=OFF", "-DFT_DISABLE_ZLIB=ON",
        "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON", "-DFT_DISABLE_HARFBUZZ=ON",
        "-DFT_DISABLE_BROTLI=ON"], args.jobs, args.platform)
    configure_build("libxlsxwriter", ["-DBUILD_SHARED_LIBS=OFF", "-DUSE_STANDARD_TMPFILE=ON",
        "-DUSE_NO_MD5=ON", "-DBUILD_TESTING=OFF"], args.jobs, args.platform)
    configure_build("occt", [f"-DINSTALL_DIR={PREFIX}", "-DINSTALL_DIR_LAYOUT=Unix",
        "-DINSTALL_DIR_INCLUDE=include/opencascade", "-DINSTALL_DIR_LIB=lib",
        "-DINSTALL_DIR_BIN=bin", "-DINSTALL_DIR_RESOURCE=share/opencascade/resources",
        "-DINSTALL_DIR_CMAKE=lib/cmake/opencascade", "-DBUILD_LIBRARY_TYPE=Shared",
        "-DBUILD_MODULE_Draw=OFF", "-DBUILD_DOC_Overview=OFF", "-DINSTALL_TEST_CASES=OFF",
        "-DUSE_TCL=OFF", "-DUSE_TK=OFF", "-DUSE_TBB=OFF", "-DUSE_FREEIMAGE=OFF",
        "-DUSE_FFMPEG=OFF", "-DUSE_VTK=OFF", "-DUSE_RAPIDJSON=OFF",
        "-DUSE_FREETYPE=ON", f"-D3RDPARTY_FREETYPE_DIR={PREFIX}"], args.jobs, args.platform)
    collect_licenses(qt_root)
    config = DEPS / "paths.cmake"
    config.write_text("# Generated by scripts/bootstrap_deps.py\n"
        f'set(STP_DEPS_PREFIX "{PREFIX.as_posix()}" CACHE PATH "")\n'
        f'set(Qt6_DIR "{(qt_root / "lib/cmake/Qt6").as_posix()}" CACHE PATH "")\n'
        f'set(OpenCASCADE_DIR "{(PREFIX / "lib/cmake/opencascade").as_posix()}" CACHE PATH "")\n'
        f'set(CMAKE_PREFIX_PATH "{PREFIX.as_posix()};{qt_root.as_posix()}" CACHE STRING "Pinned native dependency roots")\n')
    print(f"Dependencies ready. Configure the application with: cmake -C \"{config}\" -S . -B build -G Ninja")


if __name__ == "__main__":
    main()
