/* Test-only conversion of a current capture into the frozen v14 wire layout.
 * New-format round trips still use the original capture. Legacy fixtures must
 * remove the explicit PCM/PWM and translation extensions before changing the
 * magic or older tails. */
#include "pcm_state_fixture.h"
static size_t fixture_v14(uint8_t *data, size_t size) {
    size = fixture_v16(data, size);
    if (size >= 16u && !memcmp(data, "GP32STATEv0016", 14u)) {
        CHECK(size >= S3C2400_PWM_STATE_BYTES &&
              !memcmp(data + size - S3C2400_PWM_STATE_BYTES, "PWM1", 4u),
              "legacy conversion starts with complete v16 PWM extension");
        if (size < S3C2400_PWM_STATE_BYTES ||
            memcmp(data + size - S3C2400_PWM_STATE_BYTES, "PWM1", 4u)) return size;
        size -= S3C2400_PWM_STATE_BYTES;
        memcpy(data, "GP32STATEv0015", 14u);
    }
    const size_t off=16u+sizeof(gp32_state_image_t)+32u+
        GP32_CONTINUATION_BYTES+sizeof(arm920t_state_image_t);
    const size_t extra=sizeof(arm920t_translation_state_image_t);
    CHECK(size>=off+extra && !memcmp(data,"GP32STATEv0015",14u),
          "legacy conversion starts with complete v15 CPU extension");
    if(size<off+extra || memcmp(data,"GP32STATEv0015",14u))return size;
    memmove(data+off,data+off+extra,size-off-extra);
    memcpy(data,"GP32STATEv0014",14u);
    return size-extra;
}
static int fixture_cpu_prefix(arm920t_t *cpu,arm920t_state_image_t *out) {
    state_io_t count=state_io_counter();
    if(!arm920t_state_save_io(cpu,&count))return 0;
    void *data=malloc(count.pos);
    if(!data)return 0;
    state_io_t io=state_io_writer(data,count.pos);
    int ok=arm920t_state_save_io(cpu,&io);
    if(ok && io.pos>=sizeof(*out))memcpy(out,data,sizeof(*out));
    else ok=0;
    free(data);
    return ok;
}
