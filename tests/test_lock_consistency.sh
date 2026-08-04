#!/bin/bash
# test_lock_consistency.sh — verify is_consistent() checks commit + hashes
set -euo pipefail

BAKE_BIN="$1"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

cd "$TMPDIR"

# Create a project with a fake remote dep
cat > bake.toml <<'EOF'
[package]
name = "test-app"
version = "0.1.0"
type = "executable"

[dependencies]
mylib = { url = "https://github.com/example/mylib", tag = "v1.0" }
EOF

mkdir -p src
cat > src/main.cpp <<'EOF'
int main() { return 0; }
EOF

# Create a lockfile that matches manifest but has EMPTY commit + hashes
# is_consistent() should reject this
cat > bake.lock <<'EOF'
# AUTO-GENERATED. Do not edit.

[root_deps]
mylib = "mylib-v1.0"

[nodes."mylib-v1.0"]
url              = "https://github.com/example/mylib"
tag              = "v1.0"
commit           = ""
transport_sha256 = ""
tree_sha256      = ""
native           = false
dependencies     = []
EOF

# With --locked, this should FAIL because commit/hashes are empty
OUTPUT=$("$BAKE_BIN" build --locked 2>&1) || true
echo "$OUTPUT" | grep -q "stale" && {
    echo "PASS: lock with empty commit rejected by --locked"
    exit 0
}

echo "FAIL: lock with empty commit was accepted"
exit 1
