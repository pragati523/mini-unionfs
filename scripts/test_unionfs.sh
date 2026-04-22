#!/bin/bash
# test_unionfs.sh – Automated test suite for Mini-UnionFS
# Run from the project root:  bash scripts/test_unionfs.sh

set -e

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "  ${GREEN}PASSED${NC}"; ((PASS++)); }
fail() { echo -e "  ${RED}FAILED${NC} – $1"; ((FAIL++)); }

# ---------- helper: unmount safely ----------
unmount() {
    fusermount3 -u "$MOUNT_DIR" 2>/dev/null  \
    || fusermount -u  "$MOUNT_DIR" 2>/dev/null \
    || umount         "$MOUNT_DIR" 2>/dev/null \
    || true
}

# ---------- sanity check ----------
if [ ! -x "$FUSE_BINARY" ]; then
    echo -e "${RED}ERROR:${NC} $FUSE_BINARY not found. Run 'make' first."
    exit 1
fi

echo "================================================"
echo " Mini-UnionFS Test Suite"
echo "================================================"

# ---------- setup ----------
unmount 2>/dev/null || true
rm -rf "$TEST_DIR"
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

# Seed lower layer
echo "base_only_content"  > "$LOWER_DIR/base.txt"
echo "to_be_deleted"      > "$LOWER_DIR/delete_me.txt"
echo "lower_shared"       > "$LOWER_DIR/shared.txt"
echo "upper_shared"       > "$UPPER_DIR/shared.txt"   # upper override

# Mount (foreground, background the process)
"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" &
FUSE_PID=$!
sleep 1   # let FUSE settle

# ---- Test 1: Layer visibility ----
echo -n "Test 1: Layer visibility (lower file visible in mount)..."
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then
    pass
else
    fail "base.txt not readable or wrong content"
fi

# ---- Test 2: Upper takes precedence ----
echo -n "Test 2: Upper layer overrides lower for shared file..."
content=$(cat "$MOUNT_DIR/shared.txt" 2>/dev/null)
if [ "$content" = "upper_shared" ]; then
    pass
else
    fail "Expected 'upper_shared', got '$content'"
fi

# ---- Test 3: Copy-on-Write ----
echo -n "Test 3: Copy-on-Write (write to lower file)..."
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
sleep 0.2
cow_mount=$(grep -c "modified_content" "$MOUNT_DIR/base.txt" 2>/dev/null || echo 0)
cow_upper=$(grep -c "modified_content" "$UPPER_DIR/base.txt" 2>/dev/null || echo 0)
cow_lower=$(grep -c "modified_content" "$LOWER_DIR/base.txt" 2>/dev/null || echo 0)

if [ "$cow_mount" -ge 1 ] && [ "$cow_upper" -ge 1 ] && [ "$cow_lower" -eq 0 ]; then
    pass
else
    fail "mount=$cow_mount upper=$cow_upper lower=$cow_lower (lower should be 0)"
fi

# ---- Test 4: Lower untouched ----
echo -n "Test 4: Lower layer is read-only (not modified by CoW)..."
if grep -q "base_only_content" "$LOWER_DIR/base.txt" 2>/dev/null; then
    pass
else
    fail "lower/base.txt was modified – CoW broken"
fi

# ---- Test 5: Whiteout on delete ----
echo -n "Test 5: Whiteout created when deleting a lower file..."
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
sleep 0.2
wh_visible=$([ ! -f "$MOUNT_DIR/delete_me.txt" ] && echo 1 || echo 0)
wh_lower=$([ -f "$LOWER_DIR/delete_me.txt" ] && echo 1 || echo 0)
wh_marker=$([ -f "$UPPER_DIR/.wh.delete_me.txt" ] && echo 1 || echo 0)

if [ "$wh_visible" -eq 1 ] && [ "$wh_lower" -eq 1 ] && [ "$wh_marker" -eq 1 ]; then
    pass
else
    fail "visible_gone=$wh_visible lower_intact=$wh_lower whiteout_exists=$wh_marker"
fi

# ---- Test 6: Create new file (goes to upper) ----
echo -n "Test 6: New files created in upper layer..."
echo "brand_new" > "$MOUNT_DIR/newfile.txt" 2>/dev/null
sleep 0.2
if [ -f "$UPPER_DIR/newfile.txt" ] && ! [ -f "$LOWER_DIR/newfile.txt" ]; then
    pass
else
    fail "newfile.txt should be in upper only"
fi

# ---- Test 7: readdir hides whited-out files ----
echo -n "Test 7: readdir hides whited-out files..."
listing=$(ls "$MOUNT_DIR" 2>/dev/null)
if echo "$listing" | grep -q "delete_me"; then
    fail "delete_me.txt should be hidden but appears in listing"
else
    pass
fi

# ---- Test 8: readdir hides .wh.* marker files ----
echo -n "Test 8: readdir hides .wh.* marker files..."
if ls "$MOUNT_DIR" | grep -q "\.wh\."; then
    fail "whiteout markers visible in mount listing"
else
    pass
fi

# ---------- teardown ----------
unmount
wait $FUSE_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

echo ""
echo "================================================"
echo -e " Results: ${GREEN}${PASS} passed${NC}  ${RED}${FAIL} failed${NC}"
echo "================================================"

[ "$FAIL" -eq 0 ]

