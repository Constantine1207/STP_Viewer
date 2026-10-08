#!/usr/bin/env python3
"""Bundle and verify an arm64 application; no network or system changes."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / "scripts/dependencies.lock.json").read_text())


def command(args):
    subprocess.run([str(x) for x in args], check=True)


def output(args):
    return subprocess.check_output([str(x) for x in args], text=True)


def macho(path):
    return path.is_file() and "Mach-O" in output(["file", "-b", path])


def links(path):
    return [line.strip().split(" (compatibility version", 1)[0]
            for line in output(["otool", "-arch", "arm64", "-L", path]).splitlines()[1:]]


def is_system(path):
    return path.startswith(("/System/Library/", "/usr/lib/"))


def rpaths(path):
    return re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", output(["otool", "-arch", "arm64", "-l", path]))


def minimum_macos(path):
    commands = output(["otool", "-arch", "arm64", "-l", path])
    version = re.search(r"\bminos ([\d.]+)", commands)
    if not version:
        version = re.search(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)", commands)
    if not version:
        raise RuntimeError(f"No macOS minimum version in arm64 binary: {path}")
    return version.group(1)


def contained_file(path, contents):
    return path.is_file() and path.resolve().is_relative_to(contents.resolve())


def bundle_inventory(app):
    entries = {}
    for item in [app, *sorted(app.rglob("*"))]:
        rel = "." if item == app else str(item.relative_to(app))
        mode = stat.S_IMODE(item.lstat().st_mode)
        if item.is_symlink():
            entries[rel] = {"type": "link", "target": os.readlink(item), "mode": mode}
        elif item.is_file():
            entries[rel] = {"type": "file", "sha256": hashlib.sha256(item.read_bytes()).hexdigest(), "mode": mode}
        elif item.is_dir():
            entries[rel] = {"type": "directory", "mode": mode}
    return entries


def copy_runtime_bundle(source, destination):
    # Copy content, permissions and relative framework links. File Provider and
    # Finder metadata belong to the host, not to the runtime bundle. Preserve
    # quarantine when present rather than removing it to make launch easier.
    for item in [source, *sorted(source.rglob("*"))]:
        target = destination if item == source else destination / item.relative_to(source)
        if item.is_symlink():
            target.symlink_to(os.readlink(item))
        elif item.is_dir():
            target.mkdir(exist_ok=True)
            target.chmod(stat.S_IMODE(item.stat().st_mode))
        else:
            shutil.copyfile(item, target)
            target.chmod(stat.S_IMODE(item.stat().st_mode))
        if not item.is_symlink():
            quarantine = subprocess.run(["xattr", "-px", "com.apple.quarantine", str(item)],
                                        text=True, capture_output=True)
            if quarantine.returncode == 0:
                command(["xattr", "-wx", "com.apple.quarantine", quarantine.stdout.strip(), target])


def verify_published_signature(app):
    attempts = []
    for attempt in range(3):
        for attribute in ("com.apple.FinderInfo", "com.apple.ResourceFork"):
            command(["xattr", "-dr", attribute, app])
        result = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)],
                                text=True, capture_output=True)
        attempts.append({"attempt": attempt + 1, "passed": result.returncode == 0,
                         "diagnostic": (result.stdout + result.stderr).strip()})
        if result.returncode == 0:
            return {"passed": True, "attempts": attempts}
        if attempt < 2:
            time.sleep(.5)
    return {"passed": False, "attempts": attempts}


def published_runtime_check(app, report_dir):
    report_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="stp-published-runtime-") as temp:
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("DYLD_", "QT_", "QML_", "CSF_")) and key != "CASROOT"}
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        executable = app / "Contents/MacOS/STPViewer"
        sample = app / "Contents/Resources/samples/measurement-reference.step"
        smoke = Path(temp) / "smoke"
        result = subprocess.run([str(executable), "--smoke-test", str(smoke), str(sample)],
                                cwd=temp, env=env, text=True, capture_output=True, timeout=180)
        (report_dir / "published-runtime.log").write_text(result.stdout + result.stderr)
        if smoke.is_dir():
            shutil.copytree(smoke, report_dir / "published-smoke", dirs_exist_ok=True)
        result.check_returncode()
        report = json.loads((smoke / "smoke-report.json").read_text())
        if not report.get("measurementReferenceSmokePassed") or not report.get("measurementCentersSmokePassed") or not report.get("measurementExtensionSmokePassed") or report.get("measurementRecordCount") != 10:
            raise RuntimeError("Published real application failed reference/center measurement smoke test")
        return {"passed": True, "real_directory": not app.is_symlink(),
                "report": "validation/published-smoke/smoke-report.json"}


def runtime_check(archive, report_dir, expected_inventory):
    """Exercise the ZIP after moving it away from build/dependency directories."""
    relocated = Path(tempfile.mkdtemp(prefix="stp-portable-"))
    report_dir.mkdir(parents=True, exist_ok=True)
    try:
        temp = str(relocated)
        command(["ditto", "-x", "-k", archive, relocated])
        app = next(relocated.glob("*.app"))
        command(["codesign", "--verify", "--deep", "--strict", app])
        if bundle_inventory(app) != expected_inventory:
            raise RuntimeError("Extracted ZIP content, permissions or internal links differ from published application")
        executable = app / "Contents/MacOS/STPViewer"
        samples = app / "Contents/Resources/samples"
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("DYLD_", "QT_", "QML_", "CSF_")) and key != "CASROOT"}
        env["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
        version = subprocess.run([str(executable), "--version"], cwd=temp, env=env,
                                 text=True, capture_output=True, timeout=45)
        log = report_dir / "relocated-runtime.log"
        log.write_text(version.stdout + version.stderr)
        version.check_returncode()
        smoke_reports = []
        for sample_name in ("cube.step", "colored-assembly.step", "pmi-basic.step",
                            "nist_ctc_01_asme1_ap242-e1.stp", "measurement-reference.step", "measurement-extension.step"):
            sample = samples / sample_name
            if not sample.is_file():
                if sample_name == "cube.step":
                    raise RuntimeError(f"Required bundled sample is missing: {sample}")
                continue
            smoke = relocated / ("smoke-" + sample.stem)
            result = subprocess.run([str(executable), "--smoke-test", str(smoke), str(sample)],
                                    cwd=temp, env=env, text=True, capture_output=True, timeout=180)
            with log.open("a") as stream:
                stream.write(f"\nSample: {sample_name}\n" + result.stdout + result.stderr)
            destination = "relocated-smoke" if sample_name == "cube.step" else "relocated-smoke-" + sample.stem
            if smoke.is_dir():
                shutil.copytree(smoke, report_dir / destination, dirs_exist_ok=True)
            result.check_returncode()
            report = json.loads((smoke / "smoke-report.json").read_text())
            if not report.get("qtChineseTranslationLoaded") or report.get("qtCancelText") != "取消":
                raise RuntimeError(f"Bundled Qt Chinese translation did not load: {sample_name}")
            if not report.get("measurementSmokePassed") or report.get("measurementRecordCount", 0) < 2:
                raise RuntimeError(f"Bundled measurement history did not pass: {sample_name}")
            if sample_name == "measurement-reference.step" and (not report.get("measurementReferenceSmokePassed") or not report.get("measurementCentersSmokePassed") or not report.get("measurementExtensionSmokePassed") or report.get("measurementRecordCount") != 10):
                raise RuntimeError("Extracted ZIP reference/center measurement smoke test failed")
            if sample_name == "measurement-extension.step" and (not report.get("measurementExtensionSmokePassed") or report.get("measurementRecordCount") != 7):
                raise RuntimeError("Extracted ZIP infinite line/plane smoke test failed")
            if report.get("error") or not report.get("screenshot") or not report.get("sectionScreenshot"):
                raise RuntimeError(f"Relocated GUI smoke test failed: {sample_name}: {report}")
            if report.get("parts", 0) < 1 or not report.get("exportedFiles") or not (smoke / "BOM.xlsx").is_file():
                raise RuntimeError(f"Relocated STEP/Excel export test failed: {sample_name}: {report}")
            smoke_reports.append({"sample": sample_name, "report": "validation/" + destination + "/smoke-report.json"})
        verified = {"version": version.stdout.strip(), "relocated": True, "strict_signature_verified": True,
                "content_permissions_links_match": True,
                "environment": "No developer dependency paths or Qt/OCCT overrides",
                "smoke_report": "validation/relocated-smoke/smoke-report.json",
                "smoke_reports": smoke_reports}
    except Exception:
        print(f"Relocated application preserved for diagnosis: {relocated}", file=sys.stderr)
        raise
    shutil.rmtree(relocated)
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path, help="Built .app directory")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--identity", default="-", help="Default: local ad-hoc signing for internal distribution")
    args = parser.parse_args()
    source = args.app.resolve()
    if not source.is_dir() or source.suffix != ".app":
        parser.error(f"Application bundle not found: {source}")
    qt = ROOT / ".deps/qt" / LOCK["qt"]["version"] / "macos"
    deploy = qt / "bin/macdeployqt"
    if not deploy.exists():
        parser.error("Run scripts/bootstrap-macos.sh first (macdeployqt is missing).")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    # Prepare and sign outside sync-managed folders, then publish both the real
    # application and its ZIP from this same immutable snapshot.
    working = tempfile.TemporaryDirectory(prefix="stp-bundle-")
    bundle = Path(working.name) / source.name
    shutil.copytree(source, bundle, symlinks=True)
    contents = bundle / "Contents"
    frameworks = contents / "Frameworks"
    frameworks.mkdir(exist_ok=True)
    resources = contents / "Resources"
    resources.mkdir(exist_ok=True)
    # Do not depend on macdeployqt discovering plugins through the build host's
    # QLibraryInfo paths. Some relocatable Qt distributions deploy frameworks
    # successfully but omit the platform plugin. Stage the pinned plugins first,
    # so macdeployqt and the dependency-graph pass inspect and rewrite them too.
    plugins = contents / "PlugIns"
    required_plugins = {
        "platforms": ("libqcocoa.dylib",),
        "styles": ("libqmacstyle.dylib",),
        "imageformats": ("libqgif.dylib", "libqico.dylib", "libqjpeg.dylib", "libqsvg.dylib"),
        "iconengines": ("libqsvgicon.dylib",),
    }
    for category, names in required_plugins.items():
        destination = plugins / category
        destination.mkdir(parents=True, exist_ok=True)
        for name in names:
            original = qt / "plugins" / category / name
            if not original.is_file():
                raise RuntimeError(f"Missing required pinned Qt plugin: {original}")
            shutil.copy2(original, destination / name)
            # The development dependency tree can be hidden in Finder. Qt's
            # plugin loader skips files with UF_HIDDEN even at an explicit path.
            copied = destination / name
            os.chflags(copied, copied.stat().st_flags & ~stat.UF_HIDDEN)
    # Qt deployment also fixes its own framework install names and platform plugin.
    # Qt 6.8 does not accept the later -no-codesign option. We sign explicitly
    # after all third-party libraries and install names have been fixed.
    command([deploy, bundle, "-always-overwrite", "-verbose=1"])
    # OCCT resources include GLSL shaders needed on an otherwise clean Mac.
    occ_resources = ROOT / ".deps/install/share/opencascade/resources"
    if not occ_resources.is_dir():
        raise RuntimeError(f"Missing installed OCCT resources: {occ_resources}")
    shutil.copytree(occ_resources, resources / "opencascade", dirs_exist_ok=True)
    licenses = ROOT / ".deps/install/share/stp-licenses"
    if licenses.is_dir():
        shutil.copytree(licenses, resources / "licenses", dirs_exist_ok=True)
    for rel in ("LICENSE.txt", "THIRD_PARTY_NOTICES.md", "STP_README.md", "VALIDATION.md",
                "doc/stp_mac_part_grab.md", "doc/stp_mac_measurement.md", "doc/stp_mac_measurement_validation.md"):
        if not (ROOT / rel).is_file():
            raise RuntimeError(f"Required distribution document is missing: {rel}")
        destination = resources / rel
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / rel, destination)
    samples = resources / "samples"
    samples.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "tests/inputs/cube.step", samples / "cube.step")
    if (ROOT / "samples").is_dir():
        shutil.copytree(ROOT / "samples", samples, dirs_exist_ok=True)
    if (ROOT / "resources/fonts").is_dir():
        shutil.copytree(ROOT / "resources/fonts", resources / "fonts", dirs_exist_ok=True)
    # QLibraryInfo paths must resolve inside the moved bundle.
    (resources / "qt.conf").write_text("[Paths]\nPrefix = .\nPlugins = PlugIns\nTranslations = Resources/translations\n")
    translations = qt / "translations"
    if translations.is_dir():
        for translation in translations.glob("*_zh_CN.qm"):
            (resources / "translations").mkdir(exist_ok=True)
            shutil.copy2(translation, resources / "translations" / translation.name)
    shutil.copytree(ROOT / "resources/translations", resources / "translations", dirs_exist_ok=True)
    if not (resources / "translations/qtbase_zh_CN.qm").is_file():
        raise RuntimeError("Required bundled Qt Chinese translation is missing")
    # Some macdeployqt versions do not follow third-party libraries. Close the
    # dependency graph and replace all build-machine absolute dylib references.
    inspected = set()
    while True:
        pending = [p for p in contents.rglob("*") if p.is_file() and not p.is_symlink()
                   and p not in inspected and macho(p)]
        if not pending:
            break
        for binary in pending:
            inspected.add(binary)
            for dep in links(binary):
                if dep.startswith("@rpath/") and not (frameworks / dep.removeprefix("@rpath/")).exists():
                    framework_match = re.match(r"@rpath/([^/]+\.framework)/", dep)
                    if framework_match:
                        framework_name = framework_match.group(1)
                        original_framework = qt / "lib" / framework_name
                        if not original_framework.is_dir():
                            raise RuntimeError(f"Pinned Qt framework not found: {dep}")
                        shutil.copytree(original_framework, frameworks / framework_name,
                                        symlinks=True, dirs_exist_ok=True,
                                        ignore=shutil.ignore_patterns("Headers", "*.prl"))
                    # OCCT installed with relocatable IDs has no absolute path
                    # for macdeployqt to follow. Resolve it in our pinned prefix.
                    installed = ROOT / ".deps/install/lib" / Path(dep).name
                    if installed.is_file() and installed.suffix == ".dylib":
                        copied = frameworks / installed.name
                        shutil.copy2(installed.resolve(), copied)
                        command(["install_name_tool", "-id", "@rpath/" + copied.name, copied])
                if not dep.startswith("/") or is_system(dep):
                    continue
                original = Path(dep)
                if str(original).startswith(str(bundle.resolve())):
                    continue
                if original.suffix != ".dylib":
                    raise RuntimeError(f"Unbundled external framework dependency: {binary}: {dep}")
                if not original.is_file():
                    raise RuntimeError(f"Dependency not found: {dep}")
                copied = frameworks / original.name
                if not copied.exists():
                    shutil.copy2(original.resolve(), copied)
                    command(["install_name_tool", "-id", "@rpath/" + copied.name, copied])
                command(["install_name_tool", "-change", dep, "@rpath/" + copied.name, binary])
    info = plistlib.loads((contents / "Info.plist").read_bytes())
    info["LSMinimumSystemVersion"] = "12.0"
    (contents / "Info.plist").write_bytes(plistlib.dumps(info))
    executable = contents / "MacOS" / info["CFBundleExecutable"]
    load_commands = output(["otool", "-l", executable])
    if "@executable_path/../Frameworks" not in load_commands:
        command(["install_name_tool", "-add_rpath", "@executable_path/../Frameworks", executable])
    # Drop build-machine search paths after the whole library graph is copied.
    for binary in sorted(inspected):
        for search_path in rpaths(binary):
            if search_path.startswith("/") and not is_system(search_path):
                command(["install_name_tool", "-delete_rpath", search_path, binary])
    # Validate every binary and its arm64 deployment target.
    binary_inventory = []
    for binary in sorted(inspected):
        if "arm64" not in output(["lipo", "-archs", binary]):
            raise RuntimeError(f"Library lacks the required arm64 architecture: {binary}")
        minimum = minimum_macos(binary)
        if (tuple(int(part) for part in minimum.split(".")) + (0, 0))[:3] > (12, 0, 0):
            raise RuntimeError(f"Dependency requires macOS {minimum}, above 12.0: {binary}")
        binary_inventory.append({"path": str(binary.relative_to(bundle)), "minimum_macos": minimum,
                                 "dependencies": links(binary), "rpaths": rpaths(binary)})
        for dep in links(binary):
            if dep.startswith("/") and not is_system(dep):
                raise RuntimeError(f"Absolute runtime dependency remains: {binary}: {dep}")
            if dep.startswith("@rpath/") and not contained_file(frameworks / dep.removeprefix("@rpath/"), contents):
                raise RuntimeError(f"Unresolved bundled dependency: {binary}: {dep}")
            if dep.startswith("@loader_path/") and not contained_file(binary.parent / dep.removeprefix("@loader_path/"), contents):
                raise RuntimeError(f"Unresolved loader dependency: {binary}: {dep}")
            if dep.startswith("@executable_path/") and not contained_file(executable.parent / dep.removeprefix("@executable_path/"), contents):
                raise RuntimeError(f"Unresolved executable dependency: {binary}: {dep}")
            if not dep.startswith(("/", "@rpath/", "@loader_path/", "@executable_path/")):
                raise RuntimeError(f"Unexpected relative dependency: {binary}: {dep}")
    # Finder metadata may be copied by Python's copystat on macOS, and codesign
    # rejects it on executables. Remove only these two non-runtime attributes in
    # the generated copy; preserve quarantine and never alter the build input.
    for attribute in ("com.apple.FinderInfo", "com.apple.ResourceFork"):
        command(["xattr", "-dr", attribute, bundle])
    for path in [bundle, *contents.rglob("*")]:
        if not path.is_symlink():
            os.chflags(path, path.stat().st_flags & ~stat.UF_HIDDEN)
    # Sign inside-out, including Qt framework bundles, and finally the application.
    for binary in sorted(inspected, key=lambda p: len(p.parts), reverse=True):
        command(["codesign", "--force", "--sign", args.identity, binary])
    for framework in frameworks.glob("*.framework"):
        command(["codesign", "--force", "--sign", args.identity, framework])
    command(["codesign", "--force", "--sign", args.identity, bundle])
    command(["codesign", "--verify", "--deep", "--strict", bundle])
    name = "STPViewer-1.0.0-macos-arm64.zip"
    archive = args.output / name
    if archive.exists():
        archive.unlink()
    command(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", bundle, archive])
    inventory = bundle_inventory(bundle)
    published = args.output / bundle.name
    if published.is_symlink():
        published.unlink()  # Never follow an old link to an Applications folder.
    elif published.exists():
        backup = args.output / "旧版本备份"
        backup.mkdir(exist_ok=True)
        published.rename(backup / (published.stem + "-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".app"))
    copy_runtime_bundle(bundle, published)
    if bundle_inventory(published) != inventory:
        raise RuntimeError("Published application content, permissions or links differ from signed snapshot")
    published_signature = verify_published_signature(published)
    runtime = runtime_check(archive, args.output / "validation", inventory)
    published_runtime = published_runtime_check(published, args.output / "validation")
    # Recheck after launch: syncing services can add metadata asynchronously.
    final_signature = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(published)], text=True, capture_output=True)
    published_signature["after_runtime_passed"] = final_signature.returncode == 0
    published_signature["after_runtime_diagnostic"] = (final_signature.stdout + final_signature.stderr).strip()
    (args.output / "application-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{sha}  {name}\n")
    (args.output / "macos-bundle-validation.json").write_text(json.dumps({
        "archive": name, "application": published.name, "application_is_real_directory": True,
        "published_signature": published_signature, "published_runtime": published_runtime,
        "archive_application_content_match": True,
        "sha256": sha, "architecture": "arm64", "signed": args.identity,
        "macho_files_verified": len(inspected), "notarized": False,
        "minimum_macos": "12.0", "binaries": binary_inventory, "runtime": runtime,
        "note": "Relocation was tested on the build host; macOS 12 itself and a separate clean Mac remain untested."
    }, indent=2) + "\n")
    working.cleanup()
    print(archive)


if __name__ == "__main__":
    main()
