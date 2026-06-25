#!/usr/bin/env bash
# =============================================================================
# setup.sh  – Downloads third-party single-header libraries into third_party/
#
# Libraries installed:
#   • cpp-httplib   v0.15.3  → third_party/httplib.h
#   • nlohmann/json v3.11.3  → third_party/nlohmann/json.hpp
#
# Usage (from the project root):
#   bash scripts/setup.sh
# =============================================================================
set -e  # exit immediately on any error

# Resolve project root (the directory that contains this script's parent)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
THIRD_PARTY="$PROJECT_ROOT/third_party"

echo "=== cpp-url-shortener: third-party setup ==="
echo "  Target directory: $THIRD_PARTY"
echo ""

# -------------------------------------------------------
# Ensure target directories exist
# -------------------------------------------------------
mkdir -p "$THIRD_PARTY/nlohmann"

# -------------------------------------------------------
# Choose downloader: prefer curl, fall back to wget
# -------------------------------------------------------
if command -v curl &>/dev/null; then
  DOWNLOAD="curl -fsSL -o"
elif command -v wget &>/dev/null; then
  DOWNLOAD="wget -q -O"
else
  echo "[ERROR] Neither curl nor wget is installed. Please install one and retry."
  exit 1
fi

# -------------------------------------------------------
# cpp-httplib  (single header HTTP server / client)
# -------------------------------------------------------
HTTPLIB_URL="https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h"
HTTPLIB_DST="$THIRD_PARTY/httplib.h"

if [[ -f "$HTTPLIB_DST" ]]; then
  echo "[SKIP] httplib.h already present."
else
  echo "[DOWNLOAD] cpp-httplib v0.15.3 …"
  $DOWNLOAD "$HTTPLIB_DST" "$HTTPLIB_URL"
  echo "[OK]   third_party/httplib.h"
fi

# -------------------------------------------------------
# nlohmann/json  (single header JSON library)
# -------------------------------------------------------
JSON_URL="https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
JSON_DST="$THIRD_PARTY/nlohmann/json.hpp"

if [[ -f "$JSON_DST" ]]; then
  echo "[SKIP] nlohmann/json.hpp already present."
else
  echo "[DOWNLOAD] nlohmann/json v3.11.3 …"
  $DOWNLOAD "$JSON_DST" "$JSON_URL"
  echo "[OK]   third_party/nlohmann/json.hpp"
fi

echo ""
echo "=== Setup complete! You can now build: ==="
echo "   mkdir -p build && cd build"
echo "   cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo"
echo "   cmake --build . --parallel"
echo ""
