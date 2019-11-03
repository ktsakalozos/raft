#include "uv_fs.h"

#include <string.h>

#include "assert.h"
#include "err.h"
#include "heap.h"
#include "uv_error.h"

void UvFsInit(struct UvFs *fs, struct uv_loop_s *loop)
{
    fs->loop = loop;
    fs->errmsg = NULL;
}

void UvFsClose(struct UvFs *fs)
{
    HeapFree(fs->errmsg);
}

const char *UvFsErrMsg(struct UvFs *fs)
{
    return fs->errmsg;
}

void UvFsSetErrMsg(struct UvFs *fs, char *errmsg)
{
    HeapFree(fs->errmsg); /* Delete any previous error. */
    fs->errmsg = errmsg;
}

static int uvFsSyncDirThreadSafe(const char *dir, char **errmsg)
{
    uv_file fd;
    int rv;
    fd = UvOsOpen(dir, UV_FS_O_RDONLY | UV_FS_O_DIRECTORY, 0);
    if (fd < 0) {
        *errmsg = uvSysErrMsg("open directory", fd);
        return UV__ERROR;
    }
    rv = UvOsFsync(fd);
    UvOsClose(fd);
    if (rv != 0) {
        *errmsg = uvSysErrMsg("fsync directory", rv);
        return UV__ERROR;
    }
    return 0;
}

static int uvFsSyncDir(struct UvFs *fs, const char *dir)
{
    int rv;
    rv = uvFsSyncDirThreadSafe(dir, &fs->errmsg);
    if (rv != 0) {
        return rv;
    }
    return 0;
}

int UvFsCreateFile2(struct UvFs *fs,
                    const char *dir,
                    const char *filename,
                    size_t size,
                    uv_file *fd)
{
    char path[UV__PATH_SZ];
    int flags = O_WRONLY | O_CREAT | O_EXCL; /* Common open flags */
    char *errmsg = NULL;
    int rv = 0;

    UvOsJoin(dir, filename, path);

    rv = UvOsOpen(path, flags, S_IRUSR | S_IWUSR);
    if (rv < 0) {
        errmsg = uvSysErrMsg("open", rv);
        rv = UV__ERROR;
        goto err;
    }

    *fd = rv;

    /* Allocate the desired size. */
    rv = UvOsFallocate(*fd, 0, size);
    if (rv != 0) {
        /* From the manual page:
         *
         *   posix_fallocate() returns zero on success, or an error number on
         *   failure.  Note that errno is not set.
         */
        errmsg = uvSysErrMsg("posix_fallocate", rv);
        rv = UV__ERROR;
        goto err_after_open;
    }

    rv = uvFsSyncDirThreadSafe(dir, &errmsg);
    if (rv != 0) {
        goto err_after_open;
    }

    return 0;

err_after_open:
    UvOsClose(*fd);
    UvOsUnlink(path);
err:
    assert(rv != 0);
    UvFsSetErrMsg(fs, errmsg);
    return rv;
}

static void uvFsCreateFileWorkCb(uv_work_t *work)
{
    struct UvFsCreateFile *req = work->data;
    char path[UV__PATH_SZ];
    int flags = O_WRONLY | O_CREAT | O_EXCL; /* Common open flags */
    char *errmsg = NULL;
    uv_file fd;
    int rv = 0;

    UvOsJoin(req->dir, req->filename, path);

    rv = UvOsOpen(path, flags, S_IRUSR | S_IWUSR);
    if (rv < 0) {
        errmsg = uvSysErrMsg("open", rv);
        rv = UV__ERROR;
        goto err;
    }

    fd = rv;

    /* Allocate the desired size.
     *
     * TODO: add a portable version of this OS api. */
    rv = UvOsFallocate(fd, 0, req->size);
    if (rv != 0) {
        /* From the manual page:
         *
         *   posix_fallocate() returns zero on success, or an error number on
         *   failure.  Note that errno is not set.
         */
        errmsg = uvSysErrMsg("posix_fallocate", rv);
        rv = UV__ERROR;
        goto err_after_open;
    }

    rv = uvFsSyncDirThreadSafe(req->dir, &errmsg);
    if (rv != 0) {
        goto err_after_open;
    }

    req->errmsg = NULL;
    req->status = 0;
    req->fd = fd;
    return;

err_after_open:
    UvOsClose(fd);
    UvOsUnlink(path);
err:
    assert(rv != 0);
    req->errmsg = errmsg;
    req->status = rv;
    return;
}

static void uvFsCreateFileAfterWorkCb(uv_work_t *work, int status)
{
    struct UvFsCreateFile *req = work->data;
    assert(status == 0);

    /* If we were canceled, let's mark the request as canceled, regardless of
     * the actual outcome. */
    if (req->canceled) {
        if (req->status == 0) {
            char path[UV__PATH_SZ];
            UvOsJoin(req->dir, req->filename, path);
            UvOsClose(req->fd);
            UvOsUnlink(path);
        } else {
            HeapFree(req->errmsg);
        }
        req->errmsg = errMsgPrintf("canceled");
        req->status = UV__CANCELED;
    }

    /* Transfer possible errors back to the file system object. */
    UvFsSetErrMsg(req->fs, req->errmsg);

    if (req->cb != NULL) {
        req->cb(req, req->status);
    }
}

int UvFsCreateFile(struct UvFs *fs,
                   struct UvFsCreateFile *req,
                   const char *dir,
                   const char *filename,
                   size_t size,
                   UvFsCreateFileCb cb)
{
    int rv;

    req->fs = fs;
    req->work.data = req;
    req->dir = dir;
    req->filename = filename;
    req->size = size;
    req->cb = cb;
    req->canceled = false;
    req->errmsg = NULL;

    rv = uv_queue_work(fs->loop, &req->work, uvFsCreateFileWorkCb,
                       uvFsCreateFileAfterWorkCb);
    if (rv != 0) {
        /* UNTESTED: with the current libuv implementation this can't fail. */
        UvFsSetErrMsg(fs, uvSysErrMsg("uv_queue_work", rv));
        return UV__ERROR;
    }

    return 0;
}

void UvFsCreateFileCancel(struct UvFsCreateFile *req)
{
    req->canceled = true;
}

int UvFsRemoveFile(struct UvFs *fs, const char *dir, const char *filename)
{
    char path[UV__PATH_SZ];
    int rv;
    UvOsJoin(dir, filename, path);
    rv = UvOsUnlink(path);
    if (rv != 0) {
        UvFsSetErrMsg(fs, uvSysErrMsg("unlink", rv));
        return UV__ERROR;
    }
    rv = uvFsSyncDir(fs, dir);
    if (rv != 0) {
        return 0;
    }
    return 0;
}

int UvFsTruncateAndRenameFile(struct UvFs *fs,
                              const char *dir,
                              size_t size,
                              const char *filename1,
                              const char *filename2)
{
    char path1[UV__PATH_SZ];
    char path2[UV__PATH_SZ];
    uv_file fd;
    char *errmsg;
    int rv;

    UvOsJoin(dir, filename1, path1);
    UvOsJoin(dir, filename2, path2);

    /* If the desired size is zero, then let's just remove the original file. */
    if (size == 0) {
        rv = UvOsUnlink(path1);
        if (rv != 0) {
            errmsg = uvSysErrMsg("unlink", rv);
            goto err;
        }
        goto sync;
    }

    /* Truncate and rename. */
    rv = UvOsOpen(path1, UV_FS_O_RDWR, 0);
    if (rv < 0) {
        errmsg = uvSysErrMsg("open", rv);
        goto err;
    }
    fd = rv;
    rv = UvOsTruncate(fd, size);
    if (rv != 0) {
        errmsg = uvSysErrMsg("truncate", rv);
        goto err_after_open;
    }
    rv = UvOsFsync(fd);
    if (rv != 0) {
        errmsg = uvSysErrMsg("fsync", rv);
        goto err_after_open;
    }
    UvOsClose(fd);

    rv = UvOsRename(path1, path2);
    if (rv != 0) {
        errmsg = uvSysErrMsg("rename", rv);
        goto err;
    }

sync:
    rv = uvFsSyncDir(fs, dir);
    if (rv != 0) {
        return rv;
    }
    return 0;

err_after_open:
    UvOsClose(fd);
err:
    UvFsSetErrMsg(fs, errmsg);
    return UV__ERROR;
}
