/* Strip only v17's explicit PCM section to obtain the unchanged v16 layout.
 * The legacy PWM and CPU-translation projections build on this conversion. */
static size_t fixture_v16(uint8_t *data, size_t size) {
    if (size < 16u || memcmp(data, "GP32STATEv0017", 14u)) return size;
    const size_t off = 16u + sizeof(gp32_state_image_t) + 32u + GP32_CONTINUATION_BYTES +
                       sizeof(arm920t_state_image_t) + sizeof(arm920t_translation_state_image_t);
    const size_t extra = 164u;
    CHECK(size >= off + extra && !memcmp(data + off, "PCM1", 4u),
          "legacy conversion starts with complete v17 PCM extension");
    if (size < off + extra || memcmp(data + off, "PCM1", 4u)) return size;
    memmove(data + off, data + off + extra, size - off - extra);
    memcpy(data, "GP32STATEv0016", 14u);
    return size - extra;
}
