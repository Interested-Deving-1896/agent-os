#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentos-patch-std-regression.XXXXXX")"

cleanup() {
    rm -rf "$TMP_DIR"
}

trap cleanup EXIT

SOURCE_ROOT="$TMP_DIR/rust"
PATCHES_DIR="$TMP_DIR/patches"
mkdir -p "$SOURCE_ROOT/library/std/src" "$PATCHES_DIR"

cat > "$SOURCE_ROOT/library/std/src/example.rs" <<'EOF'
before
EOF

cat > "$PATCHES_DIR/0001-valid.patch" <<'EOF'
--- a/library/std/src/example.rs
+++ b/library/std/src/example.rs
@@ -1 +1 @@
-before
+after
EOF

PATCH_STD_SOURCE_ROOT="$SOURCE_ROOT" \
PATCH_STD_PATCHES_DIR="$PATCHES_DIR" \
    "$SCRIPT_DIR/patch-std.sh" --check > "$TMP_DIR/valid.log"

# Model an installed source tree that already has the valid patch. The malformed
# modification below targets this existing file, which the former fallback
# incorrectly accepted solely because the `+++ b/...` path existed.
patch --batch -p1 -d "$SOURCE_ROOT" < "$PATCHES_DIR/0001-valid.patch" > /dev/null

cat > "$PATCHES_DIR/0002-malformed.patch" <<'EOF'
--- a/library/std/src/example.rs
+++ b/library/std/src/example.rs
@@ -10 +10 @@
-context-that-does-not-exist
+must-not-pass
EOF

if PATCH_STD_SOURCE_ROOT="$SOURCE_ROOT" \
    PATCH_STD_PATCHES_DIR="$PATCHES_DIR" \
    "$SCRIPT_DIR/patch-std.sh" --check > "$TMP_DIR/invalid.log" 2>&1; then
    echo "ERROR: malformed modification patch passed validation" >&2
    exit 1
fi

grep -q "0002-malformed.patch" "$TMP_DIR/invalid.log"
echo "patch-std sequential validation regression: ok"
