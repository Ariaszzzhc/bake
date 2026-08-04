#!/bin/bash
# test_frozen_no_lock.sh — verify --frozen fails when no lockfile exists
set -euo pipefail

BAKE_BIN="$1"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$TMPDIR"

cat > bake.toml <<'EOF'
[package]
name = "test-app"
version = "0.1.0"
type = "executable"

[dependencies]
mylib = { url = "https://github.com/example/mylib", tag = "v1.0" }
EOF

mkdir -p src
echo 'int main() { return 0; }' > src/main.cpp

# No bake.lock exists — --frozen should fail
OUTPUT=$("$BAKE_BIN" build --frozen 2>&1) && {
    echo "FAIL: --frozen succeeded without lockfile"
    exit 1
} || true

echo "$OUTPUT" | grep -q "stale" && {
    echo "PASS: --frozen correctly fails without lockfile"
    exit 0
}

echo "$OUTPUT" | grep -q "offline" && {
    echo "PASS: --frozen correctly fails (offline mode)"
    exit 0
}

echo "FAIL: --frozen failed but with unexpected message"
echo "$OUTPUT"
exit 1
