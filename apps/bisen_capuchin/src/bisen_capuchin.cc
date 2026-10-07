// bisen_capuchin.cc -- Capuchin (ported) vs the RUIC engine on the Apollo4 Plus EVB.
//
// One image, one harness, one clock: both engines are linked into the same
// firmware and timed by the same code, so clock, cache, optimisation level and
// instrumentation are identical by construction (per-file flag differences are
// listed in module.mk).
//
// BENCH_MODE
//   0  bench        continuous power. Self-check every vector of both engines
//                   against host-reference outputs (bit-exact), then interleaved
//                   A/B timing with the DWT cycle counter; results over SWO.
//   1  energy loop  continuous power. Alternates a CPU-spin reference window
//                   (DAC 0, am_util_delay_ms busy-wait, then the SWO burst log)
//                   with a burst of BENCH_LOOP_COUNT back-to-back inferences of
//                   BENCH_ENGINE (DAC 3). Energy per inference = burst energy /
//                   count (gross); the DAC-0 window is a spin reference, not idle.
//   2  intermittent harvested power. Faithful to Capuchin: no checkpointing, no
//                   energy check; every boot starts inference from layer 0 and an
//                   outage loses all progress. Jobs run back to back; each
//                   completion is marked on the state DAC (5 correct, 6 wrong).
//                   BENCH_ENGINE=1 runs the RUIC engine the same way, i.e. WITHOUT
//                   its checkpoints (an ablation, not the RUIC runtime).
//
// State DAC: the harvest apps' 3-bit bus (GPIO 62/63/61 = bit 0/1/2), same pins,
// same pad configuration, same clear-then-set update. Code MEANINGS DIFFER from
// the harvest apps (there 5 = NV write committed, 6 = context restore):
//   7 boot and mode-0 self-check   3 inference (mode 1/2; Capuchin in mode 0)
//   2 RUIC inference (mode 0)   0 between inferences / spin reference
//   5 job completed, label correct   6 job completed, label wrong
//   4 never used (this firmware performs no non-volatile writes)
#include <stdint.h>
#include <string.h>

#include "am_bsp.h"
#include "am_mcu_apollo.h"
#include "am_util.h"
#include "ns_ambiqsuite_harness.h"
#include "ns_core.h"
#include "ns_peripherals_power.h"

#include "bench_vectors.h"
#include "capuchin_invoke.h"
#include "ruic_bench.h"

#ifndef BENCH_MODE
#define BENCH_MODE 0
#endif
#ifndef BENCH_ENGINE
#define BENCH_ENGINE 0          // modes 1/2: 0 Capuchin, 1 RUIC engine
#endif
#ifndef BENCH_REPS
#define BENCH_REPS 10           // mode 0: timed passes over all BENCH_N vectors
#endif
#ifndef BENCH_PRINT_ALL
#define BENCH_PRINT_ALL 0       // mode 0: 1 = one CSV line per timed inference
#endif
#ifndef BENCH_LOOP_COUNT
#define BENCH_LOOP_COUNT 200    // mode 1: inferences per burst
#endif
#ifndef BENCH_LOOP_VECS
#define BENCH_LOOP_VECS 8       // mode 1: pre-staged vectors cycled in a burst
#endif
#ifndef BENCH_IDLE_MS
#define BENCH_IDLE_MS 1000      // mode 1: idle window before each burst
#endif
#ifndef BENCH_EVENT_HOLD_US
#define BENCH_EVENT_HOLD_US 20  // mode 2: hold time of completion codes 5/6
#endif
#ifndef BENCH_SWO
#define BENCH_SWO ((BENCH_MODE) != 2)
#endif
#ifndef BENCH_STATE_DAC
#define BENCH_STATE_DAC 1
#endif
#ifndef BENCH_MCU_LOW_POWER
#define BENCH_MCU_LOW_POWER 0   // 0 = 192 MHz high-performance, as the harvest apps
#endif
#ifndef CAPUCHIN_KERNEL
#define CAPUCHIN_KERNEL 1
#endif

#if BENCH_SWO
#define LOG(...) am_util_stdio_printf(__VA_ARGS__)
#else
#define LOG(...) do { } while (0)
#endif

#if CAPUCHIN_KERNEL == 0
#define CAP_EXP_SCORES BENCH_CAP_CPU_SCORES
#define CAP_EXP_LABEL BENCH_CAP_CPU_LABEL
#else
#define CAP_EXP_SCORES BENCH_CAP_LEA_SCORES
#define CAP_EXP_LABEL BENCH_CAP_LEA_LABEL
#endif

// ---------------------------------------------------------------- state DAC
static const uint32_t kDacPins[3] = {62u, 63u, 61u};
static bool g_dac_ready = false;

static void dac_init() {
#if BENCH_STATE_DAC
    am_hal_gpio_pincfg_t cfg = {};
    cfg.GP.cfg_b.uFuncSel = 3u;
    cfg.GP.cfg_b.eGPOutCfg = AM_HAL_GPIO_PIN_OUTCFG_PUSHPULL;
    cfg.GP.cfg_b.eDriveStrength = AM_HAL_GPIO_PIN_DRIVESTRENGTH_0P1X;
    cfg.GP.cfg_b.ePullup = AM_HAL_GPIO_PIN_PULLUP_NONE;
    cfg.GP.cfg_b.eGPInput = AM_HAL_GPIO_PIN_INPUT_NONE;
    cfg.GP.cfg_b.eGPRdZero = AM_HAL_GPIO_PIN_RDZERO_READPIN;
    cfg.GP.cfg_b.eIntDir = AM_HAL_GPIO_PIN_INTDIR_LO2HI;
    for (uint32_t pin : kDacPins) {
        am_hal_gpio_output_clear(pin);
        if (am_hal_gpio_pinconfig(pin, cfg) != AM_HAL_STATUS_SUCCESS) {
            return;
        }
    }
    g_dac_ready = true;
#endif
}

static inline void dac_set(uint32_t code) {
#if BENCH_STATE_DAC
    if (!g_dac_ready) return;
    for (uint32_t pin : kDacPins) am_hal_gpio_output_clear(pin);
    for (uint32_t b = 0; b < 3; b++) {
        if (code & (1u << b)) am_hal_gpio_output_set(kDacPins[b]);
    }
#else
    (void)code;
#endif
}

// ---------------------------------------------------------------- timing
static inline void cyc_init() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}
static inline uint32_t cyc() { return DWT->CYCCNT; }

#ifdef CAPUCHIN_LAYER_PROFILE
#define CAP_MAX_LAYERS 16
static volatile uint32_t g_layer_mark[CAP_MAX_LAYERS + 1];
static volatile uint32_t g_layer_n;
extern "C" void capuchin_port_layer_end(void) {
    if (g_layer_n < CAP_MAX_LAYERS) g_layer_mark[++g_layer_n] = cyc();
}
#endif

// ---------------------------------------------------------------- inputs
#if BENCH_MODE != 1
__attribute__((unused)) static int16_t g_q10[CAPUCHIN_IN_LEN];
__attribute__((unused)) static int8_t g_i8[1024];
#endif

__attribute__((unused)) static void stage_capuchin(uint32_t v, int16_t *dst) {
    const uint8_t *p = &BENCH_PIX[v * 1024u];
    for (uint32_t i = 0; i < 1024u; i++) dst[i] = PIX_TO_Q10[p[i]];
}
__attribute__((unused)) static void stage_ruic(uint32_t v, int8_t *dst) {
    const uint8_t *p = &BENCH_PIX[v * 1024u];
    for (uint32_t i = 0; i < 1024u; i++) dst[i] = (int8_t)((int)p[i] - 128);
}

__attribute__((unused)) static uint32_t crc32_update(uint32_t crc, const void *data, uint32_t n) {
    const uint8_t *b = (const uint8_t *)data;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ---------------------------------------------------------------- board
static void board_init() {
    // Same bring-up as apps/bisen_camera_harvest*: neuralSPOT development power
    // profile, then explicit MCU performance mode, FPU on.
    ns_core_config_t core = {.api = &ns_core_V1_0_0};
    if (ns_core_init(&core) != NS_STATUS_SUCCESS) {
        while (true) { }
    }
    if (ns_power_config(&ns_development_default) != NS_STATUS_SUCCESS) {
        while (true) { }
    }
#if BENCH_SWO
    ns_itm_printf_enable();
#endif
    (void)am_hal_pwrctrl_mcu_mode_select(
#if BENCH_MCU_LOW_POWER
        AM_HAL_PWRCTRL_MCU_MODE_LOW_POWER);
#else
        AM_HAL_PWRCTRL_MCU_MODE_HIGH_PERFORMANCE);
#endif
    am_hal_sysctrl_fpu_enable();
    am_hal_sysctrl_fpu_stacking_enable(true);
    dac_init();
    dac_set(7);
    cyc_init();
}

__attribute__((unused)) static uint32_t core_mhz() { return BENCH_MCU_LOW_POWER ? 96u : 192u; }

static void banner() {
    LOG("\n==== bisen_capuchin  mode=%d engine=%d reps=%d N=%d ====\n",
        BENCH_MODE, BENCH_ENGINE, BENCH_REPS, BENCH_N);
    LOG("capuchin: upstream 76b6eb2 + port, kernel=%s (CAPUCHIN_KERNEL=%d)\n",
        capuchin_kernel_name(), CAPUCHIN_KERNEL);
    LOG("capuchin MODEL_ARRAY sha256 %s\n", BENCH_MODEL_ARRAY_SHA256);
    LOG("ruic:     %s\n", ruic_engine_desc());
    LOG("core clock %u MHz (DWT cycles), vectors = MNIST t10k[%d..%d]\n", core_mhz(),
        BENCH_MNIST_TEST_FIRST, BENCH_MNIST_TEST_FIRST + BENCH_N - 1);
}

// ================================================================ mode 0
#if BENCH_MODE == 0
static uint32_t g_t_cap[BENCH_REPS][BENCH_N];    // load + infer
static uint32_t g_t_load[BENCH_REPS][BENCH_N];   // load only
static uint32_t g_t_ruic[BENCH_REPS][BENCH_N];   // nn_run (includes its input copy)
static uint32_t g_sort[BENCH_REPS * BENCH_N];

static void stats(const char *name, const uint32_t *v, uint32_t n) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) { g_sort[i] = v[i]; sum += v[i]; }
    for (uint32_t i = 1; i < n; i++) {          // insertion sort, n <= a few thousand
        uint32_t x = g_sort[i], j = i;
        while (j > 0 && g_sort[j - 1] > x) { g_sort[j] = g_sort[j - 1]; j--; }
        g_sort[j] = x;
    }
    const uint32_t mhz = core_mhz();
    // median: mean of the two middle values for even n; p05/p95: nearest rank
    const uint32_t med = (n & 1u) ? g_sort[n / 2] : (uint32_t)(((uint64_t)g_sort[n / 2 - 1] + g_sort[n / 2] + 1u) / 2u);
    const uint32_t mn = g_sort[0], mx = g_sort[n - 1];
    const uint32_t p05 = g_sort[(n * 5 + 99) / 100 - 1], p95 = g_sort[(n * 95 + 99) / 100 - 1];
    LOG("STAT,%s,n=%u,min=%u,p05=%u,median=%u,p95=%u,max=%u,mean=%u cycles,median_us=%u.%02u\n",
        name, n, mn, p05, med, p95, mx, (uint32_t)(sum / n), med / mhz, ((med % mhz) * 100u) / mhz);
}

static void run_bench() {
    int16_t cs[CAPUCHIN_OUT_LEN];
    int8_t rs[10];

    // ---- 1. self-check: every vector, both engines, all ten scores
    uint32_t cap_ok = 0, ruic_ok = 0, cap_lab = 0, ruic_lab = 0, crc_c = 0, crc_r = 0;
    for (uint32_t v = 0; v < BENCH_N; v++) {
        stage_capuchin(v, g_q10);
        capuchin_load_input(g_q10);
        int lc = capuchin_infer(cs);
        cap_ok += memcmp(cs, &CAP_EXP_SCORES[v * 10u], sizeof cs) == 0 && lc == CAP_EXP_LABEL[v];
        cap_lab += (lc == BENCH_LABEL[v]);
        crc_c = crc32_update(crc_c, cs, sizeof cs);

        stage_ruic(v, g_i8);
        int lr = ruic_infer(g_i8, rs);
        ruic_ok += memcmp(rs, &BENCH_RUIC_SCORES[v * 10u], sizeof rs) == 0 && lr == BENCH_RUIC_LABEL[v];
        ruic_lab += (lr == BENCH_LABEL[v]);
        crc_r = crc32_update(crc_r, rs, sizeof rs);
    }
    const bool valid = (cap_ok == BENCH_N) && (ruic_ok == BENCH_N);
    LOG("CHECK,capuchin,bitexact=%u/%d,correct=%u/%d,crc32=%08x\n", cap_ok, BENCH_N, cap_lab, BENCH_N, crc_c);
    LOG("CHECK,ruic,bitexact=%u/%d,correct=%u/%d,crc32=%08x\n", ruic_ok, BENCH_N, ruic_lab, BENCH_N, crc_r);
    LOG("CHECK,%s\n", valid ? "PASS" : "FAIL -- timings below are NOT valid for comparison");

    // ---- 2. timing, interleaved; the engine order alternates every inference
    for (uint32_t r = 0; r < BENCH_REPS; r++) {
        for (uint32_t v = 0; v < BENCH_N; v++) {
            stage_capuchin(v, g_q10);
            stage_ruic(v, g_i8);
            for (uint32_t k = 0; k < 2; k++) {
                const bool cap_turn = (k == ((r + v) & 1u));
                if (cap_turn) {
                    dac_set(3);
                    uint32_t t0 = cyc();
                    capuchin_load_input(g_q10);
                    uint32_t t1 = cyc();
                    (void)capuchin_infer(cs);
                    uint32_t t2 = cyc();
                    dac_set(0);
                    g_t_load[r][v] = t1 - t0;
                    g_t_cap[r][v] = t2 - t0;
                } else {
                    dac_set(2);
                    uint32_t t0 = cyc();
                    (void)ruic_infer(g_i8, rs);
                    uint32_t t1 = cyc();
                    dac_set(0);
                    g_t_ruic[r][v] = t1 - t0;
                }
            }
#if BENCH_PRINT_ALL
            LOG("T,%u,%u,%u,%u,%u\n", r, v, g_t_cap[r][v], g_t_load[r][v], g_t_ruic[r][v]);
#endif
        }
    }
    stats("capuchin_invoke", &g_t_cap[0][0], BENCH_REPS * BENCH_N);
    stats("capuchin_load_input", &g_t_load[0][0], BENCH_REPS * BENCH_N);
    stats("ruic_nn_run", &g_t_ruic[0][0], BENCH_REPS * BENCH_N);

#ifdef CAPUCHIN_LAYER_PROFILE
    // ---- 3. per-layer breakdown (hook build only; adds a few cycles per layer)
    uint64_t acc[CAP_MAX_LAYERS] = {0};
    uint32_t nl = 0;
    for (uint32_t v = 0; v < BENCH_N; v++) {
        stage_capuchin(v, g_q10);
        capuchin_load_input(g_q10);
        g_layer_n = 0;
        g_layer_mark[0] = cyc();
        (void)capuchin_infer(cs);
        nl = g_layer_n;
        for (uint32_t i = 0; i < nl; i++) acc[i] += g_layer_mark[i + 1] - g_layer_mark[i];
    }
    for (uint32_t i = 0; i < nl; i++) {
        LOG("LAYER,%u,mean_cycles=%u\n", i, (uint32_t)(acc[i] / BENCH_N));
    }
#endif
    LOG("DONE,%s\n", valid ? "valid" : "INVALID");
    dac_set(valid ? 5 : 6);
}
#endif

// ================================================================ mode 1
#if BENCH_MODE == 1
static int16_t g_q10_set[BENCH_LOOP_VECS][CAPUCHIN_IN_LEN];
static int8_t g_i8_set[BENCH_LOOP_VECS][1024];

static void run_energy_loop() {
    for (uint32_t v = 0; v < BENCH_LOOP_VECS; v++) {
        stage_capuchin(v, g_q10_set[v]);
        stage_ruic(v, g_i8_set[v]);
    }
    int16_t cs[CAPUCHIN_OUT_LEN];
    int8_t rs[10];
    (void)cs; (void)rs;
    for (uint32_t burst = 0;; burst++) {
        dac_set(0);
        am_util_delay_ms(BENCH_IDLE_MS);
        uint32_t ok = 0;
        uint64_t total = 0;      // summed per inference: a burst can exceed the 22 s DWT wrap
        dac_set(3);
        uint32_t prev = cyc();
        for (uint32_t i = 0; i < BENCH_LOOP_COUNT; i++) {
            const uint32_t v = i % BENCH_LOOP_VECS;
#if BENCH_ENGINE == 0
            capuchin_load_input(g_q10_set[v]);
            ok += (capuchin_infer(cs) == BENCH_LABEL[v]);
#else
            ok += (ruic_infer(g_i8_set[v], rs) == BENCH_LABEL[v]);
#endif
            const uint32_t now = cyc();
            total += (uint32_t)(now - prev);
            prev = now;
        }
        dac_set(0);
        LOG("BURST,%u,engine=%d,count=%d,correct=%u,kcycles=%u,cycles_per_inf=%u\n", burst, BENCH_ENGINE,
            BENCH_LOOP_COUNT, ok, (uint32_t)(total / 1000u), (uint32_t)(total / BENCH_LOOP_COUNT));
    }
}
#endif

// ================================================================ mode 2
#if BENCH_MODE == 2
static void run_intermittent() {
    int16_t cs[CAPUCHIN_OUT_LEN];
    int8_t rs[10];
    (void)cs; (void)rs;
    // Volatile job index: restarts at 0 on every boot, like everything else.
    for (uint32_t job = 0;; job++) {
        const uint32_t v = job % BENCH_N;
        dac_set(3);
#if BENCH_ENGINE == 0
        stage_capuchin(v, g_q10);
        capuchin_load_input(g_q10);
        const int label = capuchin_infer(cs);
#else
        stage_ruic(v, g_i8);
        const int label = ruic_infer(g_i8, rs);
#endif
        dac_set(label == BENCH_LABEL[v] ? 5 : 6);
        if (BENCH_EVENT_HOLD_US) am_util_delay_us(BENCH_EVENT_HOLD_US);
    }
}
#endif

int main(void) {
    board_init();
    banner();
#if BENCH_MODE == 0
    run_bench();
    while (true) {
        am_hal_sysctrl_sleep(AM_HAL_SYSCTRL_SLEEP_NORMAL);
    }
#elif BENCH_MODE == 1
    run_energy_loop();
#elif BENCH_MODE == 2
    run_intermittent();
#else
#error "BENCH_MODE must be 0, 1 or 2"
#endif
    return 0;
}
