#ifndef GP32EMU_SAVE_ATOMIC_H
#define GP32EMU_SAVE_ATOMIC_H

#include <stddef.h>
#include <stdio.h>

/* Replace-on-commit staging for save files.
 *
 * A failed write, close, or replacement must leave the previous file exactly
 * as it was, so callers write the new image to an exclusively created sibling
 * file ("<path>.tmp.XXXXXX") and publish it with one rename. The stage name is
 * unique per attempt and exclusive, so neither another instance's stage nor an
 * unrelated "<path>.tmp" can be truncated or deleted. The freestanding WASM
 * VFS keeps its existing direct in-memory save path, without these guarantees. */
#define SAVE_ATOMIC_MAX_PATH 4096

typedef struct save_atomic {
    FILE *file;                          /* staged stream, NULL when idle */
    char tmp_path[SAVE_ATOMIC_MAX_PATH]; /* file created by save_atomic_begin */
    int active;
    int direct;                          /* stage is the destination (WASM VFS) */
} save_atomic_t;

/* Open a unique sibling stage for path. Returns 1 on success; on failure st is
 * idle and err (when err_len is nonzero) names the reason. */
int save_atomic_begin(save_atomic_t *st, const char *path, char *err, size_t err_len);

/* Close the stage and replace path with it. Returns 1 on success. On failure
 * path keeps its previous contents and the stage is deleted. st ends idle. */
int save_atomic_commit(save_atomic_t *st, const char *path, char *err, size_t err_len);
/* Flush staged data to the host device before publishing a persistent save. */
int save_atomic_sync(save_atomic_t *st, char *err, size_t err_len);

/* Close and delete only the stage created by save_atomic_begin, never path.
 * Safe on an idle state and after a failed commit. */
void save_atomic_abort(save_atomic_t *st);

#endif /* GP32EMU_SAVE_ATOMIC_H */
