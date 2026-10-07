/* mkstemp needs a feature macro and every libc header below must see it. */
#if !defined(_WIN32) && !defined(GP32EMU_WASM) && !defined(_XOPEN_SOURCE) && !defined(_POSIX_C_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "save_atomic.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#elif !defined(GP32EMU_WASM)
#include <unistd.h>
#endif

static void save_atomic_reset(save_atomic_t *st) {
    st->file = NULL;
    st->tmp_path[0] = '\0';
    st->active = 0;
    st->direct = 0;
}

#if defined(_WIN32)
/* Exclusively create "<path>.tmp.<serial>" next to path. The token only needs
 * to make collisions unlikely; _O_EXCL is what makes creation exclusive. */
static FILE *save_atomic_open_win32(char *out, size_t out_size, const char *path, char *err, size_t err_len) {
    static volatile LONG serial;
    for (int attempt = 0; attempt < 64; ++attempt) {
        unsigned long token = (unsigned long)GetTickCount()
                            ^ ((unsigned long)GetCurrentProcessId() << 9)
                            ^ ((unsigned long)InterlockedIncrement(&serial) * 2654435761UL);
        int n = snprintf(out, out_size, "%s.tmp.%08lx", path, token);
        if (n < 0 || (size_t)n >= out_size) {
            if (err && err_len) snprintf(err, err_len, "save path too long: %s", path);
            return NULL;
        }
        int fd = _open(out, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd >= 0) {
            FILE *f = _fdopen(fd, "wb");
            if (!f) {
                int e = errno;
                _close(fd);
                _unlink(out);
                if (err && err_len) snprintf(err, err_len, "open %s: %s", out, strerror(e));
                return NULL;
            }
            return f;
        }
        if (errno != EEXIST) {
            if (err && err_len) snprintf(err, err_len, "create %s: %s", out, strerror(errno));
            return NULL;
        }
    }
    if (err && err_len) snprintf(err, err_len, "create %s: too many name collisions", path);
    return NULL;
}
#endif

int save_atomic_begin(save_atomic_t *st, const char *path, char *err, size_t err_len) {
    if (!st) return 0;
    save_atomic_reset(st);
    if (!path || !path[0]) {
        if (err && err_len) snprintf(err, err_len, "save path is empty");
        return 0;
    }

#if defined(GP32EMU_WASM)
    /* wasm/libc is a freestanding in-memory VFS: fopen("wb") buffers a fresh
     * entry that replaces the path only at fclose, rename() is a no-op stub,
     * and there is no mkstemp/open/fdopen. The destination itself is therefore
     * the only usable stage; a failed write is reported to the caller, and the
     * close below is not an atomic browser-side save. */
    st->file = fopen(path, "wb");
    if (!st->file) {
        if (err && err_len) snprintf(err, err_len, "open %s: %s", path, strerror(errno));
        save_atomic_reset(st);
        return 0;
    }
    st->direct = 1;
    st->active = 1;
    return 1;
#else
    int n = snprintf(st->tmp_path, sizeof(st->tmp_path), "%s.tmp.XXXXXX", path);
    if (n < 0 || (size_t)n >= sizeof(st->tmp_path)) {
        if (err && err_len) snprintf(err, err_len, "save path too long: %s", path);
        save_atomic_reset(st);
        return 0;
    }

#if defined(_WIN32)
    st->file = save_atomic_open_win32(st->tmp_path, sizeof(st->tmp_path), path, err, err_len);
#else
    {
        int fd = mkstemp(st->tmp_path);
        if (fd < 0) {
            if (err && err_len) snprintf(err, err_len, "create %s: %s", st->tmp_path, strerror(errno));
            save_atomic_reset(st);
            return 0;
        }
        st->file = fdopen(fd, "wb");
        if (!st->file) {
            int e = errno;
            close(fd);
            remove(st->tmp_path);
            if (err && err_len) snprintf(err, err_len, "open %s: %s", st->tmp_path, strerror(e));
            save_atomic_reset(st);
            return 0;
        }
    }
#endif

    if (!st->file) {
        save_atomic_reset(st);
        return 0;
    }
    st->active = 1;
    return 1;
#endif
}

int save_atomic_commit(save_atomic_t *st, const char *path, char *err, size_t err_len) {
    if (!st || !st->active || !st->file || !path) {
        if (err && err_len) snprintf(err, err_len, "no staged save file for %s", path ? path : "");
        return 0;
    }

#if defined(GP32EMU_WASM)
    /* Closing the VFS stream publishes it; there is nothing else to do. */
    {
        int ok = (fclose(st->file) == 0);
        if (!ok && err && err_len) snprintf(err, err_len, "close %s: %s", path, strerror(errno));
        save_atomic_reset(st);
        return ok;
    }
#else
    char tmp[SAVE_ATOMIC_MAX_PATH];
    memcpy(tmp, st->tmp_path, sizeof(tmp));

    if (fclose(st->file) != 0) {
        int e = errno;
        st->file = NULL;
        remove(tmp);
        save_atomic_reset(st);
        if (err && err_len) snprintf(err, err_len, "close %s: %s", path, strerror(e));
        return 0;
    }
    st->file = NULL;

#if defined(_WIN32)
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        unsigned long code = (unsigned long)GetLastError();
        remove(tmp);
        save_atomic_reset(st);
        if (err && err_len) snprintf(err, err_len, "replace %s failed (error %lu)", path, code);
        return 0;
    }
#else
    if (rename(tmp, path) != 0) {
        int e = errno;
        remove(tmp);
        save_atomic_reset(st);
        if (err && err_len) snprintf(err, err_len, "replace %s: %s", path, strerror(e));
        return 0;
    }
#endif

    save_atomic_reset(st);
    return 1;
#endif
}

void save_atomic_abort(save_atomic_t *st) {
    if (!st) return;
    if (st->file) fclose(st->file);
    /* The WASM VFS closes onto the destination path, so it must not be removed
     * here; hosted builds only ever delete the stage this helper created. */
    if (!st->direct && st->active && st->tmp_path[0]) remove(st->tmp_path);
    save_atomic_reset(st);
}
