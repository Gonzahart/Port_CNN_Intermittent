#ifndef SCAN_H
#define SCAN_H

#include <stdint.h>
#include <stdbool.h>

// Reading the array, and deciding how much of it to read.
//
// Owns the frame buffer and the background reference. The frame lives in
// persistent RAM so a scan interrupted by a reset can carry on rather than
// start over -- the scan is the expensive part, seconds against milliseconds
// of inference.

#define BOX_SIZE        3                 // only odd numbers
#define GRID_SIZE       (32 / BOX_SIZE)   // 10
#define BOX_PRECISION   50   // BOX: |center - background| above this = box changed
#define CLS_PRECISION   30   // CLASSIFICATION: per-center change threshold
#define CLS_THRESH_PCT  12   // CLASSIFICATION: % of centers changed to trigger

#ifdef __cplusplus
extern "C" {
#endif

// The frame, 1024 values row-major. Persistent: survives a warm reset.
uint16_t *scan_frame(void);

// Read every pixel into the frame. Resumes a scan a reset interrupted, and
// checkpoints its progress as it goes.
void scan_full(void);

// True if a scan was interrupted partway and can be continued.
int  scan_resumable(void);
int  scan_next_pixel(void);

// Stepwise scanning, for a caller that owns the schedule.
//
// scan_full() runs a whole frame and decides for itself when to pause. These
// three let something else drive: begin (or resume) a frame, advance it a
// bounded number of pixels at a time, and ask whether it is finished. Between
// any two scan_step() calls the frame is coherent and scan_next_pixel() says
// exactly how far it got, so a caller can stop, persist, and come back.
void scan_begin(void);

// Abandon an in-flight scan so the next scan_begin() starts a new frame rather
// than resuming this one. Also retires any scan record on media, which would
// otherwise splice this frame's pixels onto the next one after a restore.
void scan_discard(void);
int  scan_step(int max_pixels);   // returns pixels read this call
int  scan_complete(void);

// Power-loss survival for the scan.
//
// scan_resumable() above reports the SRAM copy, which survives a warm reset but
// not a power cut. These two move the same state through MRAM. Call the save
// when power is about to fail -- once, not per pixel -- and the restore once at
// boot, before scan_full().
//
// Both return non-zero on success. A save with no scan in flight returns 0 and
// writes nothing.
int  scan_save_mram(void);
int  scan_restore_mram(void);

// What scan_save_mram would cost right now, for sizing an energy reserve.
uint32_t scan_save_bytes(void);
uint32_t scan_save_us(void);

// Adopt a scan position without touching the frame. The bytes are assumed to
// have been placed in scan_frame() already -- this only rebuilds the progress
// record so scan_full() resumes through its normal path. Returns 0 on success.
// Used by workload.h's restore path; scan_restore_mram() goes through it too.
int scan_adopt_position(uint16_t next_pixel);

// Microseconds the last completed scan took, including any resumed portion.
uint32_t scan_last_us(void);

// A cheap fingerprint of the first `pixels` frame entries. Diagnostic only:
// printing it before and after a restore is what distinguishes "the frame came
// back from MRAM" from "the frame happened to survive in SRAM".
uint32_t scan_frame_crc(uint16_t pixels);

// Re-baseline the background and reseed the frame. Call on entering a
// change-detection mode, or after anything that changes which pixels are
// masked -- otherwise the masking itself reads as motion.
void scan_reset_background(void);

// BOX mode: poll box centres, re-read the boxes that changed, and let the
// baseline follow the scene. Returns non-zero if anything changed.
// maxdiff / nover are for tuning BOX_PRECISION and may be NULL.
int  scan_refresh_changed_boxes(int *maxdiff, int *nover);

// CLASSIFICATION mode: poll box centres and return the percentage that
// changed, without re-reading anything. maxdiff may be NULL.
int  scan_change_percent(int *maxdiff);

// Adopt the last polled centres as the new baseline. Call after acting on a
// scan_change_percent() that crossed the threshold.
void scan_accept_baseline(void);

#ifdef __cplusplus
}
#endif

#endif  // SCAN_H
