//
// ckpt.h -- persist an in-progress inference so it survives power loss.
//
// Written for reactive checkpointing: the power manager decides that energy is
// about to run out and calls ckpt_save() once. This is not called per tile.
// Deciding *when* to call it is outside this library -- see nn_engine.h.
//
// Only the live state is written (see nn_live): the part of the current layer's
// input still to be read, and the part of its output computed so far. Input rows
// the layer has already consumed are excluded -- nothing will read them again.
// Everything else in the context is stale or not yet written, so writing it
// would burn energy for nothing.
//
// Layout, per slot:
//     offset 0    : 16-byte header, written LAST
//     offset 16   : input buffer,   padded up to a 16-byte boundary
//     ...         : finished output, padded up to a 16-byte boundary
//     ...         : accumulator band, padded up to a 16-byte boundary
//
// The band is the part-accumulated output rows the input-stationary kernels
// keep open. It is what buys the fine resume position and it is the largest
// single cost of the dataflow -- k output rows of int32 in convolution. It is
// empty at a layer boundary, so a checkpoint taken there is much cheaper.
//
// Two slots are alternated. The header carries a sequence number and a CRC over
// the payload, and is exactly one 16-byte write -- the smallest unit the
// Apollo4's MRAM can program -- so the commit is atomic. If power fails partway
// through a save, that slot's header is either absent or stale, and the
// previous slot is still intact and still restorable.
//
// Endurance is not a concern in this regime: the Apollo4 datasheet gives
// 100,000 program cycles (section 29.5, table 30), and a save happens once per
// power failure rather than once per tile.
//
#ifndef CKPT_H
#define CKPT_H

#include <stdint.h>

#include "nn_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

// Approximate checkpointing. Off by default, and deliberately so: it trades
// away the exactness the rest of this library is built on. With it off a
// restored inference is bit-identical to an uninterrupted one, which is what
// the 46,380-position suite checks. With it on, the accumulator band is
// restored to the precision the exporter worked out per layer (nn_layer_t
// acc_drop), the answer survives on 99.83% of resume positions, and logits move
// by at most one -- but "bit-identical" is no longer true, so test_ckpt's exact
// comparison will report differences. That is the test working, not failing.
//
// Turn on with -DCKPT_APPROX=1. Override the per-layer depth with
// -DCKPT_ACC_DROP_BITS=n for precision sweeps.
#ifndef CKPT_APPROX
#define CKPT_APPROX 0
#endif

#define CKPT_ALIGN 16       // MRAM programs in 4-word (16-byte) blocks
#define CKPT_NUM_SLOTS 2
#define CKPT_MAGIC 0x4B504331u  /* "KPC1" */

#define CKPT_ALIGN_UP(n) (((n) + (CKPT_ALIGN - 1)) & ~(uint32_t)(CKPT_ALIGN - 1))

// Exactly 16 bytes: one MRAM block, so writing it is the atomic commit.
// in_len and out_len are not stored -- they are recomputed from (layer, unit)
// through the descriptor table, which keeps the header down to a single block.
typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint16_t layer;
    uint16_t unit;
    uint32_t crc;
} ckpt_hdr_t;

#define CKPT_HDR_SZ CKPT_ALIGN

// A slot holds the largest payload any single stop point produces. The exporter
// works that out by walking every position (NN_LIVE_MAX); sizing it instead as
// "every buffer plus the whole accumulator band" would reserve a state that
// cannot occur, and on this device two such slots no longer fit the 16 KB of
// MRAM set aside for them. If a header predates NN_LIVE_MAX, fall back to that
// conservative sum -- correct, just wasteful.
#ifdef NN_LIVE_MAX
#define CKPT_SLOT_SZ (CKPT_HDR_SZ + NN_LIVE_MAX)
#else
#define CKPT_SLOT_SZ (CKPT_HDR_SZ + CKPT_ALIGN_UP(NN_BUF0_SZ) + \
                      CKPT_ALIGN_UP(NN_BUF1_SZ) + \
                      CKPT_ALIGN_UP(NN_ACC_LEN * sizeof(int32_t)))
#endif

// Storage backend. ckpt_sram.c implements this for host testing; a device
// build swaps in an MRAM implementation over am_hal_mram_main_words_program.
// Offsets and lengths handed to ckpt_dev_write are always 16-byte aligned.
//
// Note for the device implementation: while MRAM is being programmed,
// instructions cannot be fetched from it (datasheet p.70), so the write path
// must be linked into SRAM or TCM.
void ckpt_dev_init(void);
int ckpt_dev_write(uint32_t slot, uint32_t off, const void *src, uint32_t len);
int ckpt_dev_read(uint32_t slot, uint32_t off, void *dst, uint32_t len);

// Wear counters, so you can watch what is actually being programmed rather than
// assume. Maintained by the device backend only -- writes on the host cost
// nothing, so these stay zero there. Endurance is 100,000 program cycles per
// cell, so `programs` is the number that matters, not `bytes`.
extern uint32_t g_ckpt_dev_programs;
extern uint32_t g_ckpt_dev_bytes;

// API

// Prepare: scans both slots so ckpt_save picks the right one to overwrite.
void ckpt_init(void);

// Persist the live state. Returns 0 on success.
int ckpt_save(nn_ctx_t *c);

// Restore the newest valid checkpoint into c. Returns 1 if one was found and
// passed its CRC, 0 if there is nothing to resume.
int ckpt_restore(nn_ctx_t *c);

// Invalidate both slots -- call once an inference has been consumed, so a
// later reset does not resume finished work.
void ckpt_clear(void);

// Read a slot's header without disturbing anything. Returns 1 if the magic
// matches, i.e. something was actually committed there. Diagnostic only: it
// tells you whether a save reached storage, which ckpt_restore alone cannot --
// restore returning 0 looks identical for "nothing was written" and "written
// but the payload failed its CRC". Never modifies the store.
int ckpt_peek(uint32_t slot, ckpt_hdr_t *out);

// How many bytes the next ckpt_save would write, and roughly how long that
// takes. The power manager uses these to size its energy reserve.
// The timing figure comes from the datasheet's 1.5 kB/ms burst write rate;
// that value sits in the table's Max column, so treat it as an estimate until
// it has been measured on hardware.
uint32_t ckpt_bytes(nn_ctx_t *c);
uint32_t ckpt_write_us(nn_ctx_t *c);

// ---------------------------------------------------------------------------
// Scan checkpoints.
//
// The sensor scan and the inference never hold live state at the same time:
// preprocessing consumes the frame into buf0 before layer 0 runs, and the frame
// is dead from then on. So the two share the same slots and the same payload
// area rather than each reserving their own, and a record says which kind it is
// through its header -- `layer` holds CKPT_SCAN_LAYER, a value no real layer
// index can take, and `unit` carries the next pixel instead of the next unit.
// The two are mutually exclusive by construction: whichever record is newer is
// the one restored, and the other restore call returns 0.
//
// Only the pixels already read are written, so a save partway through the scan
// costs proportionally less. The frame itself stays in SRAM and is marked after
// every pixel; MRAM is programmed ONCE, when power is about to fail. Marking
// each pixel into MRAM instead would be 1024 program cycles per frame, which
// spends the part's 100,000-cycle endurance in about a hundred frames.
#define CKPT_SCAN_LAYER    0xFFFFu
#define CKPT_SCAN_PIXEL_SZ 2u        /* frame elements are uint16_t */

// Persist the first `next_pixel` pixels of `frame`. Returns 0 on success, or
// -1 if that many pixels do not fit the slot's payload area.
int ckpt_save_scan(const void *frame, uint16_t next_pixel);

// Restore a scan checkpoint into `frame`, writing at most `cap` bytes. Returns
// 1 and sets *next_pixel when the newest record is a scan record whose CRC
// passes, 0 otherwise. `next_pixel` may be NULL.
int ckpt_restore_scan(void *frame, uint32_t cap, uint16_t *next_pixel);

// 1 when the newest committed record is a scan record, i.e. the scan should be
// resumed rather than the inference. ckpt_restore returns 0 in that case.
//
// Sets *next_pixel to that record's position without restoring anything, so a
// caller can look at the frame over the same span both before and after the
// restore. Comparing different spans would make the two trivially differ and
// prove nothing. `next_pixel` may be NULL.
int ckpt_scan_pending(uint16_t *next_pixel);

// What ckpt_save_scan would cost at this position, for sizing an energy
// reserve. Same estimate basis as ckpt_write_us.
uint32_t ckpt_scan_bytes(uint16_t next_pixel);
uint32_t ckpt_scan_write_us(uint16_t next_pixel);

// Invalidate a scan record once the frame it describes is complete. Leaving it
// on media is not merely wasteful: a later restore would reload that frame's
// first pixels and then scan the rest into the same buffer, splicing two
// different frames together. Returns 1 if a record was actually cleared.
// Reads are free, so this programs nothing when there is no record.
int ckpt_scan_invalidate(void);

#ifdef __cplusplus
}
#endif
#endif  // CKPT_H
