#include "input_script.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    gp32_input_script_t *script = NULL;
    char error[256];
    if (!gp32_input_script_load(argv[1], &script, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    /* This replay formerly selected RIGHT on Windows but RIGHT+A on Linux:
     * the two frame-zero SET events compared equal in an unstable qsort. */
    const struct { uint64_t frame; uint32_t buttons; } expected[] = {
        {0, GP32_BUTTON_RIGHT | GP32_BUTTON_A}, {15, GP32_BUTTON_RIGHT},
        {30, GP32_BUTTON_RIGHT | GP32_BUTTON_A}, {180, GP32_BUTTON_A}, {195, 0},
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        uint32_t actual = gp32_input_script_frame(script, expected[i].frame);
        if (actual != expected[i].buttons) {
            fprintf(stderr, "frame %llu: buttons %x, expected %x\n",
                    (unsigned long long)expected[i].frame, actual, expected[i].buttons);
            failed = 1;
        }
    }
    gp32_input_script_destroy(script);
    if (!failed) puts("PASS: input replay preserves authored SET order");
    return failed;
}
