#!/usr/bin/env bash
# fetch-darwin-headers.sh — Extract macOS C library headers + libSystem.tbd
# from the locally installed SDK into lib/.
#
# Vendor a curated set of Darwin headers so bake can compile C/C++ without
# requiring the system SDK at compile time.
#
# Licensing: everything vendored here is open source — Darwin (APSL-2.0)
# headers plus third-party OSS (Apache, BSD, MIT, ICU, ...). Interface
# headers of Apple's closed frameworks (AppleArchive, EndpointSecurity,
# Spatial, Hypervisor, ...) have no open-source counterpart and must stay
# excluded.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/lib/libc/darwin"

# Locate the SDK
SDK="${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null || true)}"
if [[ -z "$SDK" || ! -d "$SDK/usr/include" ]]; then
  echo "error: macOS SDK not found. Install Command Line Tools: xcode-select --install" >&2
  exit 1
fi

command -v rsync >/dev/null 2>&1 || { echo "error: rsync is required" >&2; exit 1; }

echo "==> SDK: $SDK"
echo "==> Destination: $DEST"

# Clean previous extraction
rm -rf "$DEST"
mkdir -p "$DEST/include" "$DEST"

# 1. Copy /usr/include/* excluding:
#    - c++/ (bake builds its own libc++ headers)
#    - closed Apple framework interfaces with no open-source counterpart
CLOSED_FW=(AppleArchive AppleEXR.h AppleTextureEncoder.h AppleTextureEncoder.modulemap \
           EndpointSecurity Spatial SystemHealthClient.h SystemHealthManager.h \
           networkext odmodule hvf libmanagedconfigurationfiles.h xcselect.h \
           xcselect.modulemap arm64/hv)
RSYNC_ARGS=(-a --exclude='c++/')
for p in "${CLOSED_FW[@]}"; do RSYNC_ARGS+=(--exclude="$p"); done
rsync "${RSYNC_ARGS[@]}" "$SDK/usr/include/" "$DEST/include/"

HEADER_COUNT=$(find "$DEST/include" -name '*.h' | wc -l | tr -d ' ')
echo "    Copied $HEADER_COUNT headers"

# 2. Copy libSystem.tbd (only — libc++/libc++abi are compiled from source)
echo "==> Copying libSystem.tbd..."
cp "$SDK/usr/lib/libSystem.tbd" "$DEST/libSystem.tbd"

# 3. Create SDKSettings.json (MinimalDisplayName only)
SDK_VERSION=$(plutil -extract DefaultProperties.MACOSX_DEPLOYMENT_TARGET raw \
  "$SDK/SDKSettings.json" 2>/dev/null || echo "13.0")
SDK_DISPLAY=$(echo "$SDK_VERSION" | cut -d. -f1-2)
cat > "$DEST/SDKSettings.json" << EOF
{"MinimalDisplayName":"$SDK_DISPLAY"}
EOF
echo "    SDK version: $SDK_DISPLAY"

# 4. Summary
echo ""
echo "==> Done. Darwin headers + libSystem.tbd at: $DEST"
echo "    Size: $(du -sh "$DEST" | cut -f1)"
echo "    Headers: $HEADER_COUNT .h files"
echo "    libSystem.tbd: $(wc -l < "$DEST/libSystem.tbd") lines"
