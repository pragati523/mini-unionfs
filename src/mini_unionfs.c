/*
 * Mini-UnionFS: A simplified Union Filesystem using FUSE
 *
 * Implements:
 *   - Layer stacking  (lower read-only, upper read-write)
 *   - Copy-on-Write   (CoW) for lower-layer files
 *   - Whiteout files  (.wh.<name>) for deletions
 *   - POSIX ops: getattr, readdir, read, write, create,
 *                unlink, mkdir, rmdir, truncate, utimens
 *
 * Build:  gcc src/mini_unionfs.c -o mini_unionfs \
 *              $(pkg-config fuse3 --cflags --libs)
 * Usage:  ./mini_unionfs <lower_dir> <upper_dir> <mountpoint>
 */

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <libgen.h>   /* dirname / basename */
#include <utime.h>

/* ------------------------------------------------------------------ */
/*  Global state                                                        */
/* ------------------------------------------------------------------ */

#define MAX_PATH 4096

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define STATE \
    ((struct mini_unionfs_state *) fuse_get_context()->private_data)

/* ------------------------------------------------------------------ */
/*  Path helpers                                                        */
/* ------------------------------------------------------------------ */

/*
 * Build absolute path inside the upper layer.
 * dst must be MAX_PATH bytes.
 */
static void upper_path(char *dst, const char *relpath)
{
    snprintf(dst, MAX_PATH, "%s%s", STATE->upper_dir, relpath);
}

/*
 * Build absolute path inside the lower layer.
 */
static void lower_path(char *dst, const char *relpath)
{
    snprintf(dst, MAX_PATH, "%s%s", STATE->lower_dir, relpath);
}

/*
 * Build the whiteout path for a given file path.
 *
 * e.g. /foo/bar.txt  ->  <upper>/.wh.bar.txt  (whiteout is in
 *                         the parent directory)
 *
 * BUG FIX (original code): original code always put whiteout in
 * upper_dir root regardless of subdirectory depth.  This version
 * correctly places it in the same directory as the file.
 */
static void whiteout_path(char *dst, const char *relpath)
{
    /* Work on a copy because dirname() may modify its argument */
    char tmp[MAX_PATH];
    snprintf(tmp, MAX_PATH, "%s", relpath);

    char *dir  = dirname(tmp);     /* e.g. "/foo"    */
    char base[MAX_PATH];
    snprintf(base, MAX_PATH, "%s", relpath);
    char *name = basename(base);   /* e.g. "bar.txt" */

    if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0)
        snprintf(dst, MAX_PATH, "%s/.wh.%s", STATE->upper_dir, name);
    else
        snprintf(dst, MAX_PATH, "%s%s/.wh.%s",
                 STATE->upper_dir, dir, name);
}

/*
 * resolve_path – find where a file lives.
 *
 * Priority:
 *   1. Whiteout exists in upper  -> return -ENOENT
 *   2. File exists in upper      -> resolved = upper path
 *   3. File exists in lower      -> resolved = lower path
 *   4. Nowhere                   -> return -ENOENT
 *
 * Returns 0 on success, -ENOENT otherwise.
 */
static int resolve_path(const char *path, char *resolved)
{
    char wh[MAX_PATH], up[MAX_PATH], lo[MAX_PATH];

    whiteout_path(wh, path);
    upper_path(up, path);
    lower_path(lo, path);

    if (access(wh, F_OK) == 0)
        return -ENOENT;

    if (access(up, F_OK) == 0) {
        strncpy(resolved, up, MAX_PATH - 1);
        return 0;
    }

    if (access(lo, F_OK) == 0) {
        strncpy(resolved, lo, MAX_PATH - 1);
        return 0;
    }

    return -ENOENT;
}

/*
 * file_in_lower – true if the file exists only in lower (or both).
 * Used by CoW logic.
 */
static int file_in_lower(const char *path)
{
    char lo[MAX_PATH];
    lower_path(lo, path);
    return access(lo, F_OK) == 0;
}

/*
 * file_in_upper – true if the file already exists in upper.
 */
static int file_in_upper(const char *path)
{
    char up[MAX_PATH];
    upper_path(up, path);
    return access(up, F_OK) == 0;
}

/* ------------------------------------------------------------------ */
/*  Copy-on-Write                                                       */
/* ------------------------------------------------------------------ */

/*
 * copy_to_upper – copy a file from lower to upper preserving
 *                 permissions and content.
 *
 * BUG FIX: original did not preserve the file mode and did not
 * ensure the parent directory in upper exists.
 */
static int copy_to_upper(const char *path)
{
    char lo[MAX_PATH], up[MAX_PATH];
    lower_path(lo, path);
    upper_path(up, path);

    /* Ensure parent directory exists in upper */
    char up_dir[MAX_PATH];
    snprintf(up_dir, MAX_PATH, "%s", up);
    char *dir = dirname(up_dir);
    /* Recursively create parent – simple approach with system() */
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dir);
    system(cmd);  /* acceptable for userspace helper */

    struct stat st;
    if (lstat(lo, &st) == -1)
        return -errno;

    int src = open(lo, O_RDONLY);
    if (src < 0)
        return -errno;

    /* O_TRUNC so we start fresh if called after a partial write */
    int dst = open(up, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 07777);
    if (dst < 0) {
        int saved = errno;
        close(src);
        return -saved;
    }

    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            close(src);
            close(dst);
            return -EIO;
        }
    }

    close(src);
    close(dst);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  FUSE callbacks                                                      */
/* ------------------------------------------------------------------ */

/* getattr ----------------------------------------------------------- */

static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[MAX_PATH];

    memset(stbuf, 0, sizeof(*stbuf));

    /* Special-case the root */
    if (strcmp(path, "/") == 0) {
        char up[MAX_PATH];
        upper_path(up, path);
        if (lstat(up, stbuf) == -1)
            return -errno;
        return 0;
    }

    int ret = resolve_path(path, resolved);
    if (ret < 0)
        return ret;

    if (lstat(resolved, stbuf) == -1)
        return -errno;

    return 0;
}

/* readdir ----------------------------------------------------------- */

/*
 * BUG FIX: original readdir showed whiteout files AND showed
 * lower-layer files that are whited-out.  This version:
 *   1. Skips .wh.* entries from upper.
 *   2. Tracks all names already emitted.
 *   3. Skips lower-layer entries that are whited-out or already shown.
 */
static int unionfs_readdir(const char *path, void *buf,
                           fuse_fill_dir_t filler, off_t offset,
                           struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags)
{
    (void) offset; (void) fi; (void) flags;

    char up[MAX_PATH], lo[MAX_PATH];
    upper_path(up, path);
    lower_path(lo, path);

    /* Collect emitted names to avoid duplicates */
    char emitted[1024][256];
    int  emit_count = 0;

    auto int already_emitted(const char *name);
    int already_emitted(const char *name) {
        for (int i = 0; i < emit_count; i++)
            if (strcmp(emitted[i], name) == 0)
                return 1;
        return 0;
    }

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    /* --- Upper layer --- */
    DIR *dp = opendir(up);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;

            /* Skip whiteout marker files themselves */
            if (strncmp(de->d_name, ".wh.", 4) == 0)
                continue;

            filler(buf, de->d_name, NULL, 0, 0);
            if (emit_count < 1024)
                strncpy(emitted[emit_count++], de->d_name, 255);
        }
        closedir(dp);
    }

    /* --- Lower layer --- */
    dp = opendir(lo);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0)
                continue;

            /* Hidden by whiteout? */
            char wh[MAX_PATH];
            snprintf(wh, MAX_PATH, "%s/.wh.%s", up, de->d_name);
            if (access(wh, F_OK) == 0)
                continue;

            /* Already shown from upper? */
            if (already_emitted(de->d_name))
                continue;

            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

    return 0;
}

/* open -------------------------------------------------------------- */

/*
 * BUG FIX: CoW should be triggered at open time for write access,
 * not (only) at write time.  This ensures file descriptors opened
 * for writing actually operate on the upper layer copy.
 */
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char resolved[MAX_PATH];

    int ret = resolve_path(path, resolved);
    if (ret < 0)
        return ret;

    /* Trigger CoW if opening for write and file is only in lower */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (!file_in_upper(path) && file_in_lower(path)) {
            ret = copy_to_upper(path);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

/* read -------------------------------------------------------------- */

static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[MAX_PATH];

    int ret = resolve_path(path, resolved);
    if (ret < 0)
        return ret;

    int fd = open(resolved, O_RDONLY);
    if (fd == -1)
        return -errno;

    ssize_t res = pread(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return (int) res;
}

/* write ------------------------------------------------------------- */

static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    char up[MAX_PATH];
    upper_path(up, path);

    /* CoW: copy from lower if not yet in upper */
    if (access(up, F_OK) != 0) {
        if (file_in_lower(path)) {
            int ret = copy_to_upper(path);
            if (ret < 0)
                return ret;
        }
    }

    int fd = open(up, O_WRONLY);
    if (fd < 0)
        return -errno;

    ssize_t res = pwrite(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return (int) res;
}

/* create ------------------------------------------------------------ */

static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    (void) fi;
    char up[MAX_PATH];
    upper_path(up, path);

    /* Ensure parent dir exists in upper */
    char up_dir[MAX_PATH];
    snprintf(up_dir, sizeof(up_dir), "%s", up);
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", dirname(up_dir));
    system(cmd);

    /* Remove any stale whiteout for this name */
    char wh[MAX_PATH];
    whiteout_path(wh, path);
    if (access(wh, F_OK) == 0)
        unlink(wh);

    int fd = open(up, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0)
        return -errno;

    close(fd);
    return 0;
}

/* unlink / whiteout ------------------------------------------------- */

/*
 * BUG FIX: original code called strrchr and then always incremented
 * the pointer without checking for NULL (crash if path == "/file").
 * Also, it deleted from upper but forgot to check for an existing
 * whiteout so didn't clean up.
 */
static int unionfs_unlink(const char *path)
{
    char up[MAX_PATH], wh[MAX_PATH];
    upper_path(up, path);
    whiteout_path(wh, path);

    /* If it exists in upper, physically remove it */
    if (access(up, F_OK) == 0)
        unlink(up);   /* continue regardless to check lower */

    /* If it also exists in lower, leave a whiteout */
    if (file_in_lower(path)) {
        int fd = open(wh, O_CREAT | O_WRONLY, 0644);
        if (fd < 0)
            return -errno;
        close(fd);
        return 0;
    }

    /* Only existed in upper (already deleted above) */
    return 0;
}

/* truncate ---------------------------------------------------------- */

static int unionfs_truncate(const char *path, off_t size,
                            struct fuse_file_info *fi)
{
    (void) fi;
    char up[MAX_PATH];
    upper_path(up, path);

    if (access(up, F_OK) != 0 && file_in_lower(path)) {
        int ret = copy_to_upper(path);
        if (ret < 0)
            return ret;
    }

    if (truncate(up, size) == -1)
        return -errno;

    return 0;
}

/* mkdir ------------------------------------------------------------- */

static int unionfs_mkdir(const char *path, mode_t mode)
{
    char up[MAX_PATH];
    upper_path(up, path);

    if (mkdir(up, mode) == -1)
        return -errno;

    return 0;
}

/* rmdir ------------------------------------------------------------- */

static int unionfs_rmdir(const char *path)
{
    char up[MAX_PATH];
    upper_path(up, path);

    if (access(up, F_OK) == 0) {
        if (rmdir(up) == -1)
            return -errno;
    }

    return 0;
}

/* utimens ----------------------------------------------------------- */

static int unionfs_utimens(const char *path,
                           const struct timespec tv[2],
                           struct fuse_file_info *fi)
{
    (void) fi;
    char up[MAX_PATH];
    upper_path(up, path);

    if (access(up, F_OK) != 0 && file_in_lower(path)) {
        int ret = copy_to_upper(path);
        if (ret < 0)
            return ret;
    }

    if (utimensat(0, up, tv, 0) == -1)
        return -errno;

    return 0;
}

/* chmod ------------------------------------------------------------- */

static int unionfs_chmod(const char *path, mode_t mode,
                         struct fuse_file_info *fi)
{
    (void) fi;
    char up[MAX_PATH];
    upper_path(up, path);

    if (access(up, F_OK) != 0 && file_in_lower(path)) {
        int ret = copy_to_upper(path);
        if (ret < 0)
            return ret;
    }

    if (chmod(up, mode) == -1)
        return -errno;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  FUSE operations table                                              */
/* ------------------------------------------------------------------ */

static struct fuse_operations unionfs_oper = {
    .getattr  = unionfs_getattr,
    .readdir  = unionfs_readdir,
    .open     = unionfs_open,
    .read     = unionfs_read,
    .write    = unionfs_write,
    .create   = unionfs_create,
    .unlink   = unionfs_unlink,
    .truncate = unionfs_truncate,
    .mkdir    = unionfs_mkdir,
    .rmdir    = unionfs_rmdir,
    .utimens  = unionfs_utimens,
    .chmod    = unionfs_chmod,
};

/* ------------------------------------------------------------------ */
/*  main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Mini-UnionFS\n"
            "Usage: %s <lower_dir> <upper_dir> <mountpoint> [fuse_opts]\n"
            "\n"
            "  lower_dir  : read-only base layer\n"
            "  upper_dir  : read-write container layer\n"
            "  mountpoint : where the merged view appears\n",
            argv[0]);
        return 1;
    }

    struct mini_unionfs_state *state =
        calloc(1, sizeof(struct mini_unionfs_state));
    if (!state) {
        perror("calloc");
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (!state->lower_dir || !state->upper_dir) {
        fprintf(stderr, "Error: could not resolve lower/upper paths\n");
        return 1;
    }

    /*
     * Build FUSE argv:
     *   fuse_argv[0] = program name
     *   fuse_argv[1] = mountpoint
     *   fuse_argv[2] = "-f"   (foreground, helps with debugging)
     *   extra opts from argv[4..] are appended if supplied
     */
    int fuse_argc = 0;
    char **fuse_argv = malloc((argc + 2) * sizeof(char *));

    fuse_argv[fuse_argc++] = argv[0];
    fuse_argv[fuse_argc++] = argv[3];          /* mountpoint */
    fuse_argv[fuse_argc++] = "-f";             /* stay in foreground */

    /* Pass any extra FUSE options (e.g. -d for debug) */
    for (int i = 4; i < argc; i++)
        fuse_argv[fuse_argc++] = argv[i];

    fprintf(stderr,
        "[mini_unionfs] lower=%s  upper=%s  mount=%s\n",
        state->lower_dir, state->upper_dir, argv[3]);

    return fuse_main(fuse_argc, fuse_argv, &unionfs_oper, state);
}

