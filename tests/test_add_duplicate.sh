#!/bin/bash
# test_add_duplicate.sh — verify bake add rejects duplicate dependency names
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

# First add should succeed
"$BAKE_BIN" add "https://github.com/fmtlib/fmt" --tag 10.2.1 fmt 2>&1
echo "First add succeeded (expected)"

# Second add should fail
OUTPUT=$("$BAKE_BIN" add "https://github.com/fmtlib/fmt" --tag 10.2.1 fmt 2>&1) && {
    echo "FAIL: duplicate add was accepted"
    exit 1
} || true

echo "$OUTPUT" | grep -q "already exists" && {
    echo "PASS: duplicate add rejected"
    exit 0
}

echo "FAIL: duplicate add failed but with wrong error message"
exit 1
