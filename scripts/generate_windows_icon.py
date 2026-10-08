#!/usr/bin/env python3
"""Generate the supplied Windows ICO on macOS; builds use the checked-in ICO."""
from pathlib import Path
import platform
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 24, 32, 48, 64, 128, 256)

def main():
    if platform.system() != 'Darwin':
        raise SystemExit('Regeneration uses macOS sips; the finished ICO is included for Windows builds.')
    images = []
    with tempfile.TemporaryDirectory(prefix='stp-win-icon-') as temporary:
        for size in SIZES:
            output = Path(temporary) / f'{size}.png'
            subprocess.run(['/usr/bin/sips', '-z', str(size), str(size),
                            str(ROOT / 'images/stpviewer-aeme.png'), '--out', str(output)],
                           check=True, stdout=subprocess.DEVNULL)
            images.append(output.read_bytes())
    offset = 6 + 16 * len(images)
    entries = []
    for size, data in zip(SIZES, images):
        entries.append(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset))
        offset += len(data)
    destination = ROOT / 'images/stpviewer-aeme.ico'
    destination.write_bytes(struct.pack('<HHH', 0, 1, len(images)) + b''.join(entries) + b''.join(images))
    print(destination)

if __name__ == '__main__':
    main()
