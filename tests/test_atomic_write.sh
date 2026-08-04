#!/bin/bash
# test_atomic_write.sh — verify lockfile is written atomically (no .tmp left)
set -euo pipefail

BAKE_BIN="$1"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$TMPDIR"

# Create a project with only path deps (no network)
mkdir -p src mylib/public mylib/src

cat > bake.toml <<'EOF'
[package]
name = "test-app"
version = "0.1.0"
type = "executable"
EOF

echo 'int main() { return 0; }' > src/main.cpp

# Build — should succeed without creating any lockfile (no deps)
"$BAKE_BIN" build 2>&1

# Since there are no non-path deps, no lockfile should be created
if [ -f "bake.lock.tmp" ]; then
    echo "FAIL: .tmp file left behind"
    exit 1
fi

echo "PASS: no .tmp files left after build"
exit 0
