#define main existing_state_speed_main
#include "/tmp/gp32-round5-pwm/tests/gp32_state_speed_test.c"
#undef main
int main(void) {
    for (unsigned speed = 100u; speed <= 150u; speed += speed == 100u ? 33u : 17u) {
        snprintf(scenario, sizeof(scenario), "same-speed source continuation %u", speed);
        gp32_t *source = fixture(speed, 1, 17u), *target = fixture(speed, 1, 0u);
        size_t size = gp32_state_size(source), live_size = 0;
        uint8_t *state = malloc(size), *live = capture_live(source, &live_size);
        CHECK(gp32_save_state_data(source, state, size) == GP32_OK, "save source");
        CHECK(gp32_load_state_data(target, state, size) == GP32_OK, "restore source");
        expect_live(target, live, live_size, "saved and live full phases equal");
        for (unsigned c = 0u; c < 400u; ++c) {
            gp32_run_cycles(source, 1u); gp32_run_cycles(target, 1u);
            for (unsigned t=0; t<5u; ++t)
                CHECK(timer_count(source->soc,t) == timer_count(target->soc,t), "continued timer edge is identical");
        }
        free(state); free(live); gp32_destroy(source); gp32_destroy(target);
    }
    printf("same-speed probe failures=%u\n", failures);
    return failures ? 1 : 0;
}
