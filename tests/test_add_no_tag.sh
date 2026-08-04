#!/bin/bash
# test_add_no_tag.sh — verify bake add errors on --tag with no value
set -euo pipefail

BAKE_BIN="$1"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$TMPDIR"

# Create a minimal project
cat > bake.toml <<'EOF'
[package]
name = "test-app"
version = "0.1.0"
type = "executable"
EOF

mkdir -p src
echo 'int main() { return 0; }' > src/main.cpp

# --tag with no value should error
OUTPUT=$("$BAKE_BIN" add "https://github.com/fmtlib/fmt" --tag 2>&1) && {
    echo "FAIL: add with empty --tag was accepted"
    exit 1
} || true

echo "$OUTPUT" | grep -q "requires --tag" && {
    echo "PASS: empty --tag correctly rejected"
    exit 0
}

echo "FAIL: empty --tag failed but with wrong error"
exit 1
