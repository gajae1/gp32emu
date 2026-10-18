/* Exact GCC 13 / x86-64 GP32SOC9 output from legacy-writer.c.
 * Baseline 85d36d55a47fc6da191e2ad6fd2e5079a4e91e4f; size 848482 bytes.
 * SHA-256 e1f722a14171d1e4dccb7f2990701951976e5da369d57024297b0670b303efe2.
 * Unlisted bytes are zero. The legacy host-layout format is ABI-specific. */
static const size_t legacy_pwm_size = 848482u;
static const struct { size_t offset, length; unsigned char value; } legacy_pwm_fills[] = {
    {8u, 524288u, 0xffu},
    {831544u, 8192u, 0xffu},
};
static const struct { size_t offset; unsigned length; unsigned char data[32]; } legacy_pwm_runs[] = {
    {0u, 8u, {0x47u, 0x50u, 0x33u, 0x32u, 0x53u, 0x4fu, 0x43u, 0x39u}},
    {524297u, 1u, {0x10u}},
    {831508u, 1u, {0xf0u}},
    {831512u, 2u, {0x40u, 0x01u}},
    {831536u, 1u, {0x1fu}},
    {841941u, 1u, {0x40u}},
    {841952u, 1u, {0x7fu}},
    {842208u, 3u, {0x09u, 0x98u, 0x58u}},
    {842212u, 1u, {0xc8u}},
    {842224u, 1u, {0x75u}},
    {842248u, 1u, {0x97u}},
    {842260u, 1u, {0x09u}},
    {842272u, 1u, {0x1fu}},
    {842280u, 1u, {0x1fu}},
    {842288u, 1u, {0x1fu}},
    {842296u, 1u, {0x1fu}},
    {842304u, 1u, {0x0bu}},
    {842576u, 2u, {0x44u, 0xacu}},
    {842580u, 2u, {0x44u, 0xacu}},
    {842592u, 1u, {0x01u}},
    {848428u, 1u, {0xc0u}},
    {848452u, 1u, {0x1fu}},
};
