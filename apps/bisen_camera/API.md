# API reference

Everything here is in `src/workload.h`. Plain C, `extern "C"` guarded.

## Types

```c
typedef enum { WL_PHASE_IDLE, WL_PHASE_SCAN, WL_PHASE_INFER } wl_phase_t;

typedef struct {
    void    *addr;
    uint32_t bytes;      // TRUE length. Never rounded.
} wl_region_t;

typedef struct {
    wl_phase_t  phase;
    uint32_t    position;       // opaque; store and return verbatim
    uint8_t     dirty;          // 0 = nothing to save, skip the write
    uint32_t    payload_bytes;  // sum of regions, each rounded to 16
    uint32_t    write_us;       // estimate, see the caveat in README
    uint32_t    region_count;   // 0..4; this model reports at most 2
    wl_region_t region[4];
} wl_state_t;

typedef enum {
    WL_STEP_PROGRESS, WL_STEP_STOPPED, WL_STEP_COMPLETE, WL_STEP_ERROR
} wl_step_result_t;
```

## The stepper

| call | returns | notes |
|---|---|---|
| `wl_step(max_units)` | `PROGRESS` \| `STOPPED` \| `COMPLETE` \| `ERROR` | `max_units` is a ceiling, not a target. The call may do less — at a phase boundary, or on a stop. |
| `wl_request_stop()` | — | **ISR-safe.** Checked between units. |
| `wl_stop_requested()` | `int` | |
| `wl_resume()` | — | Clears the request. Nothing is re-prepared. |
| `wl_result()` | `int` | The digit. Valid only after `COMPLETE`; −1 otherwise. |
| `wl_reset()` | — | Abandons the frame *and* retires the in-flight scan record, so the next `wl_step()` genuinely starts over rather than resuming. |
| `wl_current_phase()` | `wl_phase_t` | After a successful `wl_restore_commit()` this reports the restored phase — how you confirm the restore was adopted. |

`COMPLETE` outranks a pending stop: if you request a stop just as the frame
finishes, you get `COMPLETE`, not `STOPPED`. Otherwise you would persist state
for work that no longer exists.

After `COMPLETE`, `wl_step()` keeps returning `COMPLETE` until `wl_reset()`, so
you can read the result before the next frame starts.

## State and cost

| call | notes |
|---|---|
| `wl_state(&s)` | Reads state, allocates nothing, safe any time. |
| `wl_dirty()` | Equivalent to `wl_state().dirty`. |
| `wl_payload_bytes()` | Equivalent to `wl_state().payload_bytes`. |
| `wl_write_us()` | Estimate. See the README caveat. |
| `wl_phase_last_us(phase)` | Microseconds the last completed scan or inference took, for your own energy model. |
| `wl_phase_name(phase)` | For logging. |

## Sizing a step

| call | notes |
|---|---|
| `wl_units_remaining()` | Units left in the phase running now. **While idle this reports the full scan**, not 0, because a scan is what the next `wl_step()` begins \u2014 0 would be literally true and would make any budget calculation come out zero. |
| `wl_unit_cost_us()` | Measured microseconds per unit in the phase running now. **0 means unknown**, never free. |

A unit is not a fixed amount of work: ~3,654 us during the scan, ~2.8 us during
inference \u2014 a **1,300x ratio** on this model. These two exist so a caller never
has to hardcode 1,024 and 8,094, which are exactly the model-specific numbers the
position encoding is opaque to protect.

```c
uint32_t cost = wl_unit_cost_us();
uint32_t want = cost ? (my_microseconds_of_energy / cost) : MY_DEFAULT;
uint32_t left = wl_units_remaining();
wl_step(want < left ? want : left);
```

`wl_unit_cost_us()` is an average over the last completed instance of the phase,
not a prediction for the next unit \u2014 convolution layers are heavier than dense
ones. Treat it as a planning figure.

## Watching the supply

This application **owns ADC0** (see `OWNERSHIP.md`), so a caller cannot reach the
peripheral: `am_hal_adc_initialize()` returns already-in-use. These four calls
are the way in.

| call | notes |
|---|---|
| `wl_supply_mv(&mv)` | One reading, ~214 us. Returns 0 on success, **-1 if this build has no supply source**, in which case `mv` is untouched \u2014 so a caller can tell "no source" from "rail at 0 mV". |
| `wl_wait_arm(low_mv, high_mv)` | Point the hardware window comparator at **your** band. Returns 1 armed, 0 refused. |
| `wl_wait_changed()` | Has the supply left the band? 0 when nothing is armed. Costs a register read. |
| `wl_wait_disarm()` | Stop watching. Safe unarmed, safe twice. |

**None of these decide anything.** The band is an argument, not a table lookup:
choosing thresholds is policy, and policy does not belong in the layer that owns
the peripheral.

The band is watched in **both directions**. Arm it with the rail inside, and an
excursion means either "recovered" or "still falling" \u2014 one comparator covers
both, and one `wl_supply_mv()` afterwards says which. That is one real conversion
for the whole wait instead of one per poll.

\u26a0 **A doorbell you have to look at, not one that rings.** The ADC converts and
compares unattended while the CPU sleeps, but it cannot wake anything \u2014 the ADC
interrupt vector belongs to `bisen_adc.cc`. Sleep on your own timer, wake, read
one register, sleep again. There is no callback version.

The AMAP4PEVB build defaults to the physical divider on J9.8/GPIO16/ADCSE3, so
`wl_wait_arm()` may use the shared ADC window comparator. A caller must still
check its return value and use a timed polling fallback if arming fails;
treating a refusal as armed produces a wait that never ends.

With `phase == WL_PHASE_IDLE` or `dirty == 0` there is nothing to write.
**Completion is not a persistence event** — a finished frame has no progress
that needs recovering.

## Restore

```c
int wl_restore_plan(wl_phase_t phase, uint32_t position, wl_state_t *out);
int wl_restore_commit(wl_phase_t phase, uint32_t position);
```

Two steps so you keep control of the media:

1. `wl_restore_plan()` fills `out->region[]` with the addresses and lengths your
   stored bytes must be read back into
2. you read your payload into those regions, in the same order they were saved
3. `wl_restore_commit()` makes the workload adopt the position

The regions on restore are a **pure function of `(phase, position)`** — which is
why the position is the only thing that has to be stored beside the bytes, and
why no offsets or lengths need to travel with them.

Both return 0 on success, −1 if the position is not one this build can resume
from. **Treat −1 as "start this frame over."**

## The position encoding

One `uint32_t`, so it fits an existing scalar field without widening it.

```
WL_PHASE_SCAN     pixel index, 0..1024
WL_PHASE_INFER    layer in the top 8 bits, unit in the low 24
```

The 8/24 split rather than 16/16 is deliberate: layers are bounded by the model
(7 here; 256 is generous for anything), while units scale with the network —
8,094 for this LeNet but 3.2 million for a VGG-16 first convolution. Splitting
the other way would cap a layer at 65,535 units.

**Do not decode it.** It is this module's business, and it changes with the
model. Store it and hand it back.

## Units

A *unit* is one pixel during the scan and one output element during inference.
You do not have to know which — `wl_step()` handles the transition internally,
and `wl_state().phase` tells you if you want to know.

For scale on this model: the scan is 1,024 units at ~3.5 ms each; the inference
is 8,094 units at ~2.8 µs each.

## Choosing `max_units`

There is no correct value; it trades responsiveness against per-call overhead.
Some anchors:

| `max_units` | inference granularity | note |
|---|---|---|
| 8 | ~23 µs | fine; per-call overhead starts to show |
| 64 | ~180 µs | `WL_DRIVER_CHUNK`, what both bundled drivers use |
| 1000 | ~2.8 ms | one BISen `chunk1000` |

A stop is honoured at the *next* call, so the worst-case latency between
requesting a stop and being stopped is one `max_units` of work.

The worst-case `payload_bytes` over every position in this model is **4,864**,
reached in the first convolution. It is never more, whatever `max_units` you
choose — the granularity changes where you can stop, not how much is live.

## Testing the contract

`src/wl_apitest.c` checks every promise on this page against the running build:
48 checks, one frame (~4.3 s), at boot, `WL_APITEST 0` to compile out. It leaves
the workload reset either way.

```
APITEST: 48 passed, 0 failed  (scan 3741613 us, infer 22848 us, digit 0)
```

On failure it also drives state-bus code 7, so a scope shows it with no terminal
attached. It found three real defects on its first run; see the README.

## What is in the build but not part of the API

`src/power_policy.*`, `src/energy_source.*`, `src/adc_shared.*` and `src/bisen/`
implement the
self-driving variant of this application, where it samples a supply and decides
for itself. **In this build that decision path is compiled out**
(`WL_EXTERNAL_DRIVER=1` in `module.mk`). They are present because the two
variants share one source tree; they are not something you need to call,
configure, or avoid.

`src/ckpt.*` and `src/ckpt_mram.c` are the reference checkpoint library. They
**are** linked — `ckpt_init()` reads MRAM at boot and the scan calls
`ckpt_scan_invalidate()`. Its save paths are never called, so no checkpoint is
ever written; the one program `ckpt_scan_invalidate()` can issue happens only
when a stale scan record from another build is on media. Measured writes on a
clean board: zero. This does not constrain your own storage layer in any way —
it is a different slot region and a different code path.

`WL_SELFTEST_EVERY` in `src/cam_lenet2.cc` is a stand-in driver that requests a
stop every 37 steps, reads `wl_state()`, and resumes — proving the path works
end to end without any storage. **Replace that loop with yours.** Set it to 0 to
disable.

`src/energy_source.*` selects where an energy observation comes from. The
AMAP4PEVB build selects the physical J9.8/GPIO16 divider, read by the same
`adc_shared.c` owner that services the camera. The reading drives the direct
BISen bands, wait decision, checkpoint trigger, and window-comparator limits;
it is not merely diagnostic output.
