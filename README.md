# Mini-UnionFS

A simplified Union Filesystem (like Docker's overlay) built in userspace with FUSE.

## Quick Start

### 1. Install dependencies (Ubuntu 22.04 / 24.04)

```bash
sudo apt update
sudo apt install -y libfuse3-dev fuse3 build-essential pkg-config
```

### 2. Build

```bash
make
```

### 3. Manual test

```bash
mkdir -p lower upper mnt
echo "base content" > lower/base.txt
echo "delete me"    > lower/delete.txt

./mini_unionfs lower upper mnt
# (runs in foreground – open a second terminal for the steps below)
```

In a **second terminal**:
```bash
# See the merged view
ls mnt                         # shows base.txt  delete.txt

# Read a lower-layer file
cat mnt/base.txt               # "base content"

# Modify it (triggers Copy-on-Write)
echo "extra line" >> mnt/base.txt
cat mnt/base.txt               # both lines
cat upper/base.txt             # copy is in upper
cat lower/base.txt             # lower is UNCHANGED

# Delete a lower-layer file (creates whiteout)
rm mnt/delete.txt
ls mnt                         # delete.txt is gone
ls upper                       # .wh.delete.txt is there
ls lower                       # delete.txt still intact in lower

# Create a brand-new file
echo "new" > mnt/new.txt
ls upper                       # new.txt appears here

# Unmount
fusermount3 -u mnt
```

### 4. Run automated tests

```bash
make test
```

Expected output:
```
================================================
 Mini-UnionFS Test Suite
================================================
Test 1: Layer visibility (lower file visible in mount)...  PASSED
Test 2: Upper layer overrides lower for shared file...     PASSED
Test 3: Copy-on-Write (write to lower file)...             PASSED
Test 4: Lower layer is read-only (not modified by CoW)...  PASSED
Test 5: Whiteout created when deleting a lower file...     PASSED
Test 6: New files created in upper layer...                PASSED
Test 7: readdir hides whited-out files...                  PASSED
Test 8: readdir hides .wh.* marker files...                PASSED

================================================
 Results: 8 passed  0 failed
================================================
```

## Project Structure

```
mini_unionfs/
├── src/
│   └── mini_unionfs.c      ← full FUSE implementation
├── scripts/
│   └── test_unionfs.sh     ← automated test suite
├── docs/
│   └── design_document.md  ← 2-3 page design document
└── Makefile
```

## Debugging

Run with the `-d` flag for verbose FUSE debug output:

```bash
./mini_unionfs lower upper mnt -d
```

## Unmounting if the process crashes

```bash
fusermount3 -u mnt
# or
sudo umount mnt
```

