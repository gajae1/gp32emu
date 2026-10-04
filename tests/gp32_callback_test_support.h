/* Existing short callback fixtures inspect completed output. Explicitly
 * service the pending callback through public one-instruction budgets so
 * those checks keep their meaning without restoring a synchronous runner. */
static inline int test_finish_callbacks(gp32_t *g) {
    for (unsigned i = 0; i < 500000u && g->direct_hle_callback_running; ++i)
        if (gp32_run_cycles(g, 1u) != GP32_OK) return 0;
    return !g->direct_hle_callback_running && g->direct_callback.owner != DIRECT_CB_FAULT;
}
/* Supply a foreground interval to the consumer arithmetic fixtures. Their
 * register values may deliberately be nonexecutable. Production display
 * waits use normal guest CPU execution, covered separately by gp32_wait_test. */
static inline int test_hle_interval(gp32_t *g, uint32_t cycles) {
    while (cycles) {
        uint32_t n = cycles > 32768u ? 32768u : cycles;
        uint32_t clock = direct_run_clock_hz(g);
        arm920t_add_idle_cycles(g->cpu, n);
        direct_account_elapsed(g, n, clock);
        s3c2400_tick(g->soc, n);
        direct_hle_tick(g, n, clock);
        if (!test_finish_callbacks(g)) return 0;
        direct_update_fw_tick(g);
        cycles -= n;
    }
    return 1;
}
static inline int test_call_guest_function3(gp32_t *g, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2) {
    if (g->direct_tick.clock || g->direct_callback.owner) return 0;
    g->direct_tick.clock = direct_run_clock_hz(g);
    g->direct_tick.phase = DIRECT_TICK_TIMER_CALLS;
    g->direct_tick.due[0].callback = fn;
    g->direct_tick.due[0].tps = 1000u;
    g->direct_tick.due[0].fires = 1u;
    if (!direct_begin_guest_function3(g, DIRECT_CB_TIMER, fn, r0, r1, r2)) return 0;
    return test_finish_callbacks(g);
}
static inline int test_call_guest_callback(gp32_t *g, uint32_t fn) {
    return test_call_guest_function3(g, fn, 0u, 0u, 0u);
}
static inline void test_hle_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    direct_hle_tick(g, cycles, clock);
    if (!test_finish_callbacks(g)) abort();
}
static inline void test_sdk_sound_tick(gp32_t *g, uint32_t cycles, uint32_t clock) {
    test_hle_tick(g, cycles, clock);
}
static inline gp32_status_t test_run_cycles(gp32_t *g, uint32_t cycles) {
    gp32_status_t status = gp32_run_cycles(g, cycles);
    return status != GP32_OK ? status : test_finish_callbacks(g) ? GP32_OK : GP32_ERR_CPU_FAULT;
}
