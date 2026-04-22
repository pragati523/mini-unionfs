# Mini-UnionFS Design Document

## 1. Overview

Mini-UnionFS is a simplified Union Filesystem implemented in userspace
using FUSE (Filesystem in Userspace).  It merges two real directories
into one virtual mountpoint:

| Layer     | Directory    | Access      | Role                        |
|-----------|--------------|-------------|------------------------------|
| Lower     | `lower_dir`  | Read-only   | Base image (e.g. Docker base OS) |
| Upper     | `upper_dir`  | Read-write  | Container modifications      |
| Mount     | `mountpoint` | Read-write  | Unified merged view          |

The design mirrors how production overlay filesystems (OverlayFS,
AUFS) power Docker containers, but is implemented entirely in
userspace for clarity and portability.

---

## 2. Architecture

```
User Process
    │  read("base.txt"), write("base.txt"), rm("delete_me.txt")
    ▼
Linux VFS
    │  POSIX syscall (open, read, write, unlink, …)
    ▼
FUSE Kernel Module  (/dev/fuse)
    │  routes request to userspace daemon
    ▼
mini_unionfs  (our daemon)
    │
    ├─ resolve_path()      – find canonical location of a path
    ├─ copy_to_upper()     – Copy-on-Write engine
    ├─ whiteout_path()     – deletion marker helpers
    │
    ├── upper_dir/         – checked first; writable
    └── lower_dir/         – fallback; never written
```

---

## 3. Data Structures

### 3.1 `mini_unionfs_state`

```c
struct mini_unionfs_state {
    char *lower_dir;   /* absolute path of the lower (read-only) layer */
    char *upper_dir;   /* absolute path of the upper (read-write) layer */
};
```

This struct is allocated once in `main()` and passed to
`fuse_main()` as `private_data`.  Every FUSE callback retrieves it
via the `STATE` macro:

```c
#define STATE \
    ((struct mini_unionfs_state *) fuse_get_context()->private_data)
```

### 3.2 `fuse_operations` (vtable)

FUSE dispatches kernel requests to function pointers registered in
`struct fuse_operations unionfs_oper`.  We populate:

| Field       | Our handler           | Purpose                        |
|-------------|----------------------|--------------------------------|
| `getattr`   | `unionfs_getattr`    | `stat()` equivalent            |
| `readdir`   | `unionfs_readdir`    | Directory listing              |
| `open`      | `unionfs_open`       | Open / CoW trigger             |
| `read`      | `unionfs_read`       | Read bytes                     |
| `write`     | `unionfs_write`      | Write bytes (CoW if needed)    |
| `create`    | `unionfs_create`     | Create new file in upper       |
| `unlink`    | `unionfs_unlink`     | Delete / whiteout              |
| `truncate`  | `unionfs_truncate`   | Resize file (CoW if needed)    |
| `mkdir`     | `unionfs_mkdir`      | Create directory in upper      |
| `rmdir`     | `unionfs_rmdir`      | Remove directory from upper    |
| `utimens`   | `unionfs_utimens`    | Update timestamps (CoW)        |
| `chmod`     | `unionfs_chmod`      | Change permissions (CoW)       |

---

## 4. Core Algorithms

### 4.1 Path Resolution (`resolve_path`)

Every FUSE callback begins by resolving the virtual path to a real path:

```
1. Compute whiteout marker path: upper_dir/<dir>/.wh.<filename>
2. If whiteout exists  → return -ENOENT  (file was deleted)
3. If upper_dir/path exists  → return upper path
4. If lower_dir/path exists  → return lower path
5. Otherwise → return -ENOENT
```

### 4.2 Copy-on-Write (`copy_to_upper`)

Triggered whenever a write, truncate, chmod, or utimens is requested
for a file that currently only exists in the lower layer:

```
1. stat() the lower file to read its mode bits
2. Create parent directories in upper (mkdir -p)
3. open(lower, O_RDONLY)
4. open(upper, O_CREAT|O_WRONLY|O_TRUNC, lower_mode)
5. Read lower → write upper in 64 KiB chunks
6. close both fds
```

After CoW, the lower file is **never touched**; all subsequent
reads and writes go to the upper copy.

### 4.3 Whiteout (`unionfs_unlink`)

When a user deletes a file that originates in the lower layer:

```
1. If file exists in upper  → unlink it (removes the CoW copy)
2. If file exists in lower  → create upper_dir/<dir>/.wh.<filename>
   (a zero-byte marker file)
3. resolve_path() checks for this marker before returning any path,
   so the file appears absent to all subsequent lookups
```

### 4.4 Directory Listing (`unionfs_readdir`)

```
Emit "." and ".."

For every entry in upper_dir:
    skip .wh.* markers        (don't expose internals)
    emit name, record in seen-set

For every entry in lower_dir:
    skip if upper_dir/.wh.<name> exists   (whited-out)
    skip if name already in seen-set      (upper overrides)
    emit name
```

---

## 5. Edge Cases Handled

| Scenario | Handling |
|----------|----------|
| Delete then re-create a lower file | `create` removes the whiteout marker before creating the new upper file |
| Write to a lower file that has a CoW copy in upper | `write` detects upper copy already exists; skips redundant copy |
| Nested directories (e.g. `/a/b/c.txt`) | `whiteout_path` uses `dirname`/`basename` to place markers in correct subdirectory; `copy_to_upper` uses `mkdir -p` on the parent |
| `root (/)` getattr | Special-cased to `lstat` the upper root directly; avoids `resolve_path` recursion |
| Duplicate entries in `readdir` | In-function `seen` array prevents a name present in both layers from appearing twice |
| Whiteout markers visible in listing | `readdir` skips all `.wh.*` entries from upper before emitting them |
| File permissions after CoW | `copy_to_upper` preserves `st.st_mode` from the source file |

---

## 6. Building and Running

### Prerequisites (Ubuntu 22.04 / 24.04)

```bash
sudo apt install libfuse3-dev fuse3 build-essential pkg-config
```

### Build

```bash
make
```

### Run

```bash
mkdir -p lower upper mnt
echo "hello" > lower/base.txt
./mini_unionfs lower upper mnt
# In another terminal:
ls mnt
cat mnt/base.txt
echo "world" >> mnt/base.txt   # triggers CoW
cat upper/base.txt             # has both lines
cat lower/base.txt             # still has only "hello"
# Unmount:
fusermount3 -u mnt
```

### Run test suite

```bash
make test
```

---

## 7. Limitations

- No support for hard links or symbolic links in the lower layer.
- `readdir` seen-set is capped at 1024 entries per directory.
- No NFS-safe file handles; `open` does not store an fd in `fi->fh`.
- Concurrent write access to the same file is not serialised.

