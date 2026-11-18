#ifndef GP32EMU_HOST_FILE_H
#define GP32EMU_HOST_FILE_H

#include <stdio.h>

/* Frontends pass UTF-8 paths even when the Windows process uses another
 * ANSI code page. Keep the encoding local to the filesystem boundary. */
#ifdef _WIN32
FILE *gp32_open_read(const char *path);
int gp32_remove_file(const char *path);
#else
#define gp32_open_read(path) fopen((path), "rb")
#define gp32_remove_file remove
#endif

#endif
