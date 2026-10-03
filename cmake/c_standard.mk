# Shared Makefile C-standard policy. Older C23-capable GCC/Clang accept
# -std=c2x but reject the newer -std=c23 spelling. GCC 8 rejects both.
GP32EMU_C_STD ?= c23
GP32EMU_C_STD_CC ?= $(CC)
GP32EMU_C_STD_PROBE ?= cmake/c_standard_probe.c
GP32EMU_REQUIRE_C23 ?= 0

ifeq ($(OS),Windows_NT)
GP32EMU_C_STD_NULL := NUL
else
GP32EMU_C_STD_NULL := /dev/null
endif

GP32EMU_C_STD_PRIMARY := $(strip $(shell $(GP32EMU_C_STD_CC) -x c -std=$(GP32EMU_C_STD) -fsyntax-only $(GP32EMU_C_STD_PROBE) >$(GP32EMU_C_STD_NULL) 2>&1 && echo yes || echo no))
ifeq ($(GP32EMU_C_STD_PRIMARY),yes)
GP32EMU_C_STD_FLAG := -std=$(GP32EMU_C_STD)
else
ifeq ($(GP32EMU_C_STD),c23)
GP32EMU_C_STD_C2X := $(strip $(shell $(GP32EMU_C_STD_CC) -x c -std=c2x -fsyntax-only $(GP32EMU_C_STD_PROBE) >$(GP32EMU_C_STD_NULL) 2>&1 && echo yes || echo no))
endif
ifeq ($(GP32EMU_C_STD_C2X),yes)
GP32EMU_C_STD_FLAG := -std=c2x
$(info $(GP32EMU_C_STD_CC) accepts C23 as -std=c2x)
else
GP32EMU_C_STD_FLAG := -std=c11
ifeq ($(GP32EMU_REQUIRE_C23),1)
$(error C23 is required, but $(GP32EMU_C_STD_CC) rejected -std=c23 and -std=c2x)
else
$(warning $(GP32EMU_C_STD_CC) rejected -std=$(GP32EMU_C_STD) and -std=c2x; falling back to C11)
endif
endif
endif
