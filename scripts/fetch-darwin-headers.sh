#!/usr/bin/env bash
# fetch-darwin-headers.sh — Extract macOS C library headers + libSystem.tbd
# from the locally installed SDK into lib/bake/.
#
# This mirrors Zig's approach: vendor a curated set of Darwin headers so
# bake can compile C/C++ without requiring the system SDK at compile time.
#
# Headers are Apple's, redistributed unmodified under APSL-2.0 / OS Reference License.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/lib/bake/libc/darwin"

# Locate the SDK
SDK="${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null || true)}"
if [[ -z "$SDK" || ! -d "$SDK/usr/include" ]]; then
  echo "error: macOS SDK not found. Install Command Line Tools: xcode-select --install" >&2
  exit 1
fi

echo "==> SDK: $SDK"
echo "==> Destination: $DEST"

# Clean previous extraction
rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST"

# 1. Copy /usr/include/* excluding the c++ directory (we build our own libc++ headers).
echo "==> Copying C library headers (excluding c++/)..."
# Use rsync to preserve the directory tree; exclude c++/ since we vendor our own.
if command -v rsync &>/dev/null; then
  rsync -a --exclude='c++/' "$SDK/usr/include/" "$DEST/include/"
else
  # Fallback: cp without c++/
  (cd "$SDK/usr/include" && find . -not -path './c++/*' -name '*.h' -o -name '*.modulemap' | \
    while read -r f; do
      mkdir -p "$DEST/include/$(dirname "$f")"
      cp "$SDK/usr/include/$f" "$DEST/include/$f"
    done)
fi

HEADER_COUNT=$(find "$DEST/include" -name '*.h' | wc -l | tr -d ' ')
echo "    Copied $HEADER_COUNT headers"

# 2. Copy libSystem.tbd (only — libc++/libc++abi are compiled from source)
echo "==> Copying libSystem.tbd..."
cp "$SDK/usr/lib/libSystem.tbd" "$DEST/libSystem.tbd"

# 3. Create SDKSettings.json with the SDK version
SDK_VERSION=$(plutil -extract DefaultProperties.MACOSX_DEPLOYMENT_TARGET raw \
  "$SDK/SDKSettings.json" 2>/dev/null || echo "13.0")
# Extract just the major.minor for MinimalDisplayName
SDK_DISPLAY=$(echo "$SDK_VERSION" | cut -d. -f1-2)
cat > "$DEST/SDKSettings.json" << EOF
{"MinimalDisplayName":"$SDK_DISPLAY","Version":"$SDK_VERSION"}
EOF
echo "    SDK version: $SDK_DISPLAY"

# 4. Summary
echo ""
echo "==> Done. Darwin headers + libSystem.tbd at: $DEST"
echo "    Size: $(du -sh "$DEST" | cut -f1)"
echo "    Headers: $HEADER_COUNT .h files"
echo "    libSystem.tbd: $(wc -l < "$DEST/libSystem.tbd") lines"
