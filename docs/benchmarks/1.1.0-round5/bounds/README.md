# Direct span overflow and live mapping bounds

The final `s3c2400_fastmem` fixture has 199 checks. Its independently listed
spans cover normal BIOS/RAM access, read/write permissions, exact ends,
one-past zero-length access, crossing lengths, `SIZE_MAX` and `SIZE_MAX-15`,
and three live RAM allocation states. It also retains the complete-range
certificate checks with a copied bus across standalone state loads that grow
and shrink RAM.

The new direct-span cases expose 18 failed assertions before the bounds fix;
all 199 checks pass afterward. Raw build/test logs and before/after source
SHA-256 values are retained. This historical comparison is based on local
`4f727138e5ba9913e8adcfdedc79ad51edaa7618`; the later integrated PWM changes
also pass the same final fixture on both host architectures and sanitizers.

The fix first proves the offset is inside the allocation, then compares the
requested byte count with `size - offset`. It does not add the possibly huge
length before comparing. The BIOS remains read-only; valid zero-length
one-past spans remain valid. These extreme host spans are distinct from the
ordinary 1/2/4-byte guest CPU accesses.

The final CTest entry is `s3c2400_fastmem`. See
[verification](../verification/README.md) for full build flags and all 64 tests,
and [ASan](../asan/README.md) for the focused memory run. The optional audit
history preserves the exact historical source checkpoints referenced by
the source manifests.
