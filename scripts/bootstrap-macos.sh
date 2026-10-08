#!/usr/bin/env bash
# All installed tools and dependencies stay inside this checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if [[ "$(uname -m)" != arm64 ]]; then
  echo "This package targets Apple Silicon (arm64)." >&2
  exit 1
fi
xcrun --find clang >/dev/null
PYTHON="${STP_PYTHON:-python3}"
if [[ ! -x .tools/venv/bin/python ]]; then "$PYTHON" -m venv .tools/venv; fi
PYTHON="$ROOT/.tools/venv/bin/python"
"$PYTHON" -m pip install --disable-pip-version-check \
  'cmake==3.31.10' 'ninja==1.13.0' 'aqtinstall==3.3.0'
export PATH="$ROOT/.tools/venv/bin:$PATH"
exec "$PYTHON" "$ROOT/scripts/bootstrap_deps.py" --platform macos "$@"
