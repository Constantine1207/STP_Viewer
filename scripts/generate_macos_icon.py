#!/usr/bin/env python3
"""Convert the supplied AEME artwork into the Mac app's multi-resolution icon."""
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    if platform.system() != "Darwin":
        raise SystemExit("Icon regeneration requires macOS sips and iconutil; the ICNS is included in the source package.")
    source = ROOT / "images/stpviewer-aeme.png"
    header = source.read_bytes()[:24]
    if header[:8] != b"\x89PNG\r\n\x1a\n" or len(header) != 24:
        raise SystemExit("The icon source must be a PNG image.")
    width, height = struct.unpack(">II", header[16:24])
    if width != height or width < 1024:
        raise SystemExit("The icon source must be square and at least 1024 pixels.")
    with tempfile.TemporaryDirectory(prefix="stp-icon-") as temporary:
        iconset = Path(temporary) / "AEME.iconset"
        iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                suffix = "@2x" if scale == 2 else ""
                output = iconset / f"icon_{size}x{size}{suffix}.png"
                subprocess.run(["/usr/bin/sips", "-z", str(size * scale), str(size * scale),
                                str(source), "--out", str(output)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["/usr/bin/iconutil", "-c", "icns", "-o",
                        str(ROOT / "images/stpviewer-aeme.icns"), str(iconset)], check=True)
    print(ROOT / "images/stpviewer-aeme.icns")

if __name__ == "__main__":
    main()
