#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PYTHON="$ROOT/.tools/venv/bin/python"
[[ -x "$PYTHON" ]] || PYTHON=python3
exec "$PYTHON" "$ROOT/scripts/package_macos.py" "$@"
