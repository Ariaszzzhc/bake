#!/bin/bash
# test_path_dep_locked.sh — verify --locked works with path-only dependencies
set -euo pipefail

BAKE_BIN="$1"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$TMPDIR"

# Create a workspace-style project with path deps only
mkdir -p app/src mylib/public mylib/src

cat > bake.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"
type = "executable"

[dependencies]
mylib = { path = "mylib" }
EOF

cat > mylib/bake.toml <<'EOF'
[package]
name = "mylib"
version = "0.1.0"
type = "static-lib"
EOF

echo 'int add(int a, int b);' > mylib/public/mylib.hpp
echo 'int add(int a, int b) { return a + b; }' > mylib/src/mylib.cpp

cat > app/src/main.cpp <<'EOF'
#include <mylib.hpp>
int main() { return add(1, 2) - 3; }
EOF

# Wait, this is a single-package project with a path dep.
# With only path deps, enforce_lock should be a no-op.
# --locked should NOT report stale.
OUTPUT=$("$BAKE_BIN" build --locked 2>&1)
EXIT_CODE=$?

if [ $EXIT_CODE -eq 0 ]; then
    echo "PASS: --locked works with path-only deps"
    exit 0
fi

echo "$OUTPUT"
echo "FAIL: --locked failed for path-dep-only project (exit $EXIT_CODE)"
exit 1
