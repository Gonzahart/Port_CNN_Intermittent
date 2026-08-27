//
// ckpt.c -- save and restore an in-progress inference. See ckpt.h.
//
#include "ckpt.h"

static uint32_t g_seq;        // sequence number of the newest valid checkpoint
static uint32_t g_next_slot;  // slot ckpt_save will write next

static uint32_t crc32(const void *data, uint32_t len, uint32_t crc) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

// Write len bytes at a 16-byte-aligned offset, padding the final partial block
// through a bounce buffer so we never read past the end of the source.
static int write_padded(uint32_t slot, uint32_t off, const void *src,
                        uint32_t len, uint32_t *next_off) {
    const uint8_t *p = (const uint8_t *)src;
    const uint32_t whole = len & ~(uint32_t)(CKPT_ALIGN - 1);

    if (whole && ckpt_dev_write(slot, off, p, whole) != 0) return -1;

    uint32_t written = whole;
    if (len > whole) {
        uint8_t tail[CKPT_ALIGN];
        for (uint32_t i = 0; i < CKPT_ALIGN; i++) {
            tail[i] = (whole + i < len) ? p[whole + i] : 0;
        }
        if (ckpt_dev_write(slot, off + whole, tail, CKPT_ALIGN) != 0) return -1;
        written += CKPT_ALIGN;
    }
    *next_off = off + written;
    return 0;
}

static int read_padded(uint32_t slot, uint32_t off, void *dst, uint32_t len,
                       uint32_t *next_off) {
    uint8_t *p = (uint8_t *)dst;
    const uint32_t whole = len & ~(uint32_t)(CKPT_ALIGN - 1);

    if (whole && ckpt_dev_read(slot, off, p, whole) != 0) return -1;

    uint32_t consumed = whole;
    if (len > whole) {
        uint8_t tail[CKPT_ALIGN];
        if (ckpt_dev_read(slot, off + whole, tail, CKPT_ALIGN) != 0) return -1;
        for (uint32_t i = 0; whole + i < len; i++) p[whole + i] = tail[i];
        consumed += CKPT_ALIGN;
    }
    *next_off = off + consumed;
    return 0;
}

// Read a slot's header and say whether it is a plausible checkpoint. The CRC
// covers the payload, so it is checked during restore, not here.
static int read_hdr_any(uint32_t slot, ckpt_hdr_t *h) {
    if (ckpt_dev_read(slot, 0, h, CKPT_HDR_SZ) != 0) return 0;
    if (h->magic != CKPT_MAGIC) return 0;
    return h->layer <= NN_NUM_LAYERS || h->layer == CKPT_SCAN_LAYER;
}

// Inference records only. A scan record's layer field is deliberately out of
// range, so this rejects it without needing to know it exists.
static int read_hdr(uint32_t slot, ckpt_hdr_t *h) {
    return read_hdr_any(slot, h) && h->layer <= NN_NUM_LAYERS;
}

// Newest committed record of either kind. The sequence number is shared across
// both, so "newest" is meaningful between them and the two phases cannot both
// claim to be current.
static int newest_any(ckpt_hdr_t *out, uint32_t *slot_out) {
    int found = 0;
    uint32_t best_seq = 0;
    for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
        ckpt_hdr_t h;
        if (read_hdr_any(s, &h) && (!found || h.seq > best_seq)) {
            found = 1;
            best_seq = h.seq;
            *out = h;
            *slot_out = s;
        }
    }
    return found;
}

void ckpt_init(void) {
    ckpt_dev_init();
    g_seq = 0;
    g_next_slot = 0;

    // Newest valid slot wins; the next save goes to the other one, so a failed
    // save never destroys the checkpoint we would fall back to.
    int found = -1;
    for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
        ckpt_hdr_t h;
        // Either kind: the sequence counter must account for scan records too,
        // or the next inference save would reuse a number already on media.
        if (read_hdr_any(s, &h) && (found < 0 || h.seq > g_seq)) {
            g_seq = h.seq;
            found = (int)s;
        }
    }
    if (found >= 0) g_next_slot = ((uint32_t)found + 1) % CKPT_NUM_SLOTS;
}

int ckpt_peek(uint32_t slot, ckpt_hdr_t *out) {
    if (slot >= CKPT_NUM_SLOTS) return 0;
    if (ckpt_dev_read(slot, 0, out, CKPT_HDR_SZ) != 0) return 0;
    return out->magic == CKPT_MAGIC;
}

// ---------------------------------------------------------------------------
// Approximate accumulators.
//
// Each partial sum is stored as `acc >> drop` in the fewest whole bytes that
// can still hold it, and restored as `stored << drop`. That is a requantization
// of the checkpoint: same idea as the int8 weights, one level in -- a shift onto
// a coarser grid, chosen so the error is smaller than the layer's requantizer
// can resolve. `drop` comes from nn_layer_t.acc_drop, which the exporter derives
// from that layer's own mult/shift.
//
// Bytes are counted from 32 - drop rather than from the values' actual range,
// so `acc >> drop` always fits and no accumulator can ever overflow its slot.
// ---------------------------------------------------------------------------

static int acc_drop_of(uint16_t layer) {
#if CKPT_APPROX
#ifdef CKPT_ACC_DROP_BITS
    (void)layer; return CKPT_ACC_DROP_BITS;
#else
    return k_layers[layer].acc_drop;
#endif
#else
    (void)layer; return 0;
#endif
}

static uint32_t acc_bytes_each(int drop) {
    uint32_t bits = 32u - (uint32_t)drop;
    uint32_t bytes = (bits + 7u) / 8u;
    return bytes > 4u ? 4u : (bytes < 1u ? 1u : bytes);
}

// Packed size of the band, and 0 when there is no band.
static uint32_t acc_packed_len(const nn_live_t *lv, int drop) {
    if (!lv->acc_len) return 0;
    return (lv->acc_len / sizeof(int32_t)) * acc_bytes_each(drop);
}

uint32_t ckpt_bytes(nn_ctx_t *c) {
    nn_live_t lv;
    nn_live(c, &lv);
    const int drop = acc_drop_of(c->layer);
    return CKPT_HDR_SZ + CKPT_ALIGN_UP(lv.in_len) + CKPT_ALIGN_UP(lv.out_len) +
           CKPT_ALIGN_UP(acc_packed_len(&lv, drop));
}

uint32_t ckpt_write_us(nn_ctx_t *c) {
    // 1.5 kB/ms == 1.5 bytes/us (Apollo4 Plus datasheet, table 30).
    return (ckpt_bytes(c) * 2u) / 3u;
}

int ckpt_save(nn_ctx_t *c) {
    nn_live_t lv;
    nn_live(c, &lv);

    const uint32_t slot = g_next_slot;
    uint32_t off = CKPT_HDR_SZ;

    // Payload first. If power dies here the header is never written, so this
    // slot stays invalid and the other slot remains the newest good one.
    if (write_padded(slot, off, lv.in, lv.in_len, &off) != 0) return -1;
    if (lv.out_len && write_padded(slot, off, lv.out, lv.out_len, &off) != 0) {
        return -1;
    }
    const int drop = acc_drop_of(c->layer);
    uint32_t crc = crc32(lv.in, lv.in_len, 0);
    if (lv.out_len) crc = crc32(lv.out, lv.out_len, crc);

    if (lv.acc_len) {
        if (drop == 0) {
            // Exact: the band goes out as-is, which is the default path and
            // must stay byte-for-byte what it always was.
            if (write_padded(slot, off, lv.acc, lv.acc_len, &off) != 0) return -1;
            crc = crc32(lv.acc, lv.acc_len, crc);
        } else {
            const uint32_t n = lv.acc_len / sizeof(int32_t);
            const uint32_t bpa = acc_bytes_each(drop);
            uint8_t buf[48];              // 3 x CKPT_ALIGN; holds a whole
            uint32_t fill = 0;            // number of accumulators for bpa 1-4
            for (uint32_t i = 0; i < n; i++) {
                const int32_t v = lv.acc[i] >> drop;
                for (uint32_t b = 0; b < bpa; b++) {
                    buf[fill++] = (uint8_t)((uint32_t)v >> (8u * b));
                }
                // CRC the value as it will be RECONSTRUCTED, not as it is
                // packed. Both sides then agree without either having to know
                // about padding or byte order.
                const int32_t back = v << drop;
                crc = crc32(&back, sizeof(back), crc);
                if (fill == sizeof(buf)) {
                    if (ckpt_dev_write(slot, off, buf, fill) != 0) return -1;
                    off += fill;
                    fill = 0;
                }
            }
            if (fill) {
                while (fill % CKPT_ALIGN) buf[fill++] = 0;
                if (ckpt_dev_write(slot, off, buf, fill) != 0) return -1;
                off += fill;
            }
        }
    }

    // Header last, in one 16-byte program. This single write is the commit.
    ckpt_hdr_t h;
    h.magic = CKPT_MAGIC;
    h.seq = g_seq + 1;
    h.layer = c->layer;
    h.unit = c->unit;
    h.crc = crc;
    if (ckpt_dev_write(slot, 0, &h, CKPT_HDR_SZ) != 0) return -1;

    g_seq = h.seq;
    g_next_slot = (slot + 1) % CKPT_NUM_SLOTS;
    return 0;
}

int ckpt_restore(nn_ctx_t *c) {
    // A newer scan record means power failed before the frame was complete, so
    // there is no inference to resume -- and any inference record still on
    // media belongs to an earlier frame. Refuse rather than restore stale work.
    {
        ckpt_hdr_t newest;
        uint32_t newest_slot;
        if (newest_any(&newest, &newest_slot) &&
            newest.layer == CKPT_SCAN_LAYER) {
            return 0;
        }
    }
    // Try slots newest first; a slot that fails its CRC is skipped, so a torn
    // write falls back to the previous good checkpoint rather than failing.
    for (;;) {
        int best = -1;
        uint32_t best_seq = 0;
        ckpt_hdr_t bh = {0, 0, 0, 0, 0};   // only read when best >= 0; the
                                           // initialiser is for the compiler,
                                           // which cannot see that


        for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
            ckpt_hdr_t h;
            if (read_hdr(s, &h) && (best < 0 || h.seq > best_seq)) {
                best = (int)s;
                best_seq = h.seq;
                bh = h;
            }
        }
        if (best < 0) return 0;

        c->layer = bh.layer;
        c->unit = bh.unit;

        nn_live_t lv;
        nn_live(c, &lv);

        uint32_t off = CKPT_HDR_SZ;
        int ok = (read_padded((uint32_t)best, off, lv.in, lv.in_len, &off) == 0);
        if (ok && lv.out_len) {
            ok = (read_padded((uint32_t)best, off, lv.out, lv.out_len, &off) == 0);
        }
        const int drop = acc_drop_of(bh.layer);

        if (ok && lv.acc_len) {
            if (drop == 0) {
                ok = (read_padded((uint32_t)best, off, lv.acc, lv.acc_len,
                                  &off) == 0);
            } else {
                const uint32_t n = lv.acc_len / sizeof(int32_t);
                const uint32_t bpa = acc_bytes_each(drop);
                // Unsigned, because for bpa == 4 the sign bit is bit 31 and
                // shifting a 1 into an int32's sign bit is undefined.
                const uint32_t sign = (bpa < 4u) ? (1u << (bpa * 8u - 1u)) : 0u;
                // 48 is a multiple of every bpa (1,2,3,4) AND of CKPT_ALIGN,
                // so a buffer always holds a whole number of accumulators and
                // the chunk boundaries match what ckpt_save wrote.
                uint8_t buf[48];
                uint32_t have = 0, take = 0;
                for (uint32_t i = 0; i < n && ok; i++) {
                    if (take == have) {          // refill
                        uint32_t want = (n - i) * bpa;
                        if (want > sizeof(buf)) want = sizeof(buf);
                        want = (want + CKPT_ALIGN - 1) & ~(uint32_t)(CKPT_ALIGN - 1);
                        if (ckpt_dev_read((uint32_t)best, off, buf, want) != 0) {
                            ok = 0; break;
                        }
                        off += want; have = want; take = 0;
                    }
                    uint32_t raw = 0;
                    for (uint32_t b = 0; b < bpa; b++) {
                        raw |= (uint32_t)buf[take++] << (8u * b);
                    }
                    // Sign-extend from bpa bytes, then undo the shift.
                    if (sign && (raw & sign)) raw |= ~((sign << 1) - 1u);
                    lv.acc[i] = ((int32_t)raw) << drop;
                }
            }
        }

        if (ok) {
            uint32_t crc = crc32(lv.in, lv.in_len, 0);
            if (lv.out_len) crc = crc32(lv.out, lv.out_len, crc);
            // Either path leaves lv.acc holding exactly the values ckpt_save
            // hashed -- the exact bytes, or the reconstructed ones. Hashing
            // them one at a time there and all at once here is the same byte
            // stream, so the two agree without knowing about the packing.
            if (lv.acc_len) crc = crc32(lv.acc, lv.acc_len, crc);
            if (crc == bh.crc) {
                // No truncation step here any more -- the packing above already
                // stores and restores at the reduced precision, so the loss is
                // in the storage format rather than applied afterwards. The CRC
                // covers the reconstructed values, so integrity is still checked
                // on exactly what the resume will use.
                g_seq = bh.seq;
                g_next_slot = ((uint32_t)best + 1) % CKPT_NUM_SLOTS;
                return 1;
            }
        }

        // Bad slot: blank its header and look again.
        ckpt_hdr_t dead;
        for (uint32_t i = 0; i < sizeof(dead) / 4; i++) ((uint32_t *)&dead)[i] = 0;
        ckpt_dev_write((uint32_t)best, 0, &dead, CKPT_HDR_SZ);
    }
}

void ckpt_clear(void) {
    ckpt_hdr_t dead;
    for (uint32_t i = 0; i < sizeof(dead) / 4; i++) ((uint32_t *)&dead)[i] = 0;

    for (uint32_t s = 0; s < CKPT_NUM_SLOTS; s++) {
        // Skip slots that are already invalid. This is called after EVERY
        // completed inference, so on a real NVM backend an unconditional write
        // would burn an endurance cycle per frame forever -- about 100,000
        // cycles in ~3 days at one frame every 2.5 s, wearing out the header
        // cells while achieving nothing. Reads are free; writes are not.
        ckpt_hdr_t h;
        if (ckpt_dev_read(s, 0, &h, CKPT_HDR_SZ) == 0 && h.magic == 0) continue;
        ckpt_dev_write(s, 0, &dead, CKPT_HDR_SZ);
    }
    g_seq = 0;
    g_next_slot = 0;
}

// ---------------------------------------------------------------------------
// Scan checkpoints. See the header for why these share the inference slots.

uint32_t ckpt_scan_bytes(uint16_t next_pixel) {
    return CKPT_HDR_SZ +
           CKPT_ALIGN_UP((uint32_t)next_pixel * CKPT_SCAN_PIXEL_SZ);
}

uint32_t ckpt_scan_write_us(uint16_t next_pixel) {
    // 1.5 kB/ms == 1.5 bytes/us, same basis as ckpt_write_us.
    return (ckpt_scan_bytes(next_pixel) * 2u) / 3u;
}

int ckpt_save_scan(const void *frame, uint16_t next_pixel) {
    const uint32_t bytes = (uint32_t)next_pixel * CKPT_SCAN_PIXEL_SZ;
    if (bytes > CKPT_SLOT_SZ - CKPT_HDR_SZ) return -1;

    const uint32_t slot = g_next_slot;
    uint32_t off = CKPT_HDR_SZ;

    // Payload first, header last, exactly as ckpt_save does: if power dies
    // during the payload the header never lands and this slot stays invalid.
    if (bytes && write_padded(slot, off, frame, bytes, &off) != 0) return -1;

    ckpt_hdr_t h;
    h.magic = CKPT_MAGIC;
    h.seq = g_seq + 1;
    h.layer = CKPT_SCAN_LAYER;
    h.unit = next_pixel;
    h.crc = crc32(frame, bytes, 0);
    if (ckpt_dev_write(slot, 0, &h, CKPT_HDR_SZ) != 0) return -1;

    g_seq = h.seq;
    g_next_slot = (slot + 1) % CKPT_NUM_SLOTS;
    return 0;
}

int ckpt_scan_pending(uint16_t *next_pixel) {
    ckpt_hdr_t h = {0, 0, 0, 0, 0};
    uint32_t slot = 0;
    if (!newest_any(&h, &slot) || h.layer != CKPT_SCAN_LAYER) return 0;
    if (next_pixel) *next_pixel = h.unit;
    return 1;
}

int ckpt_scan_invalidate(void) {
    ckpt_hdr_t h = {0, 0, 0, 0, 0};
    uint32_t slot = 0;
    if (!newest_any(&h, &slot) || h.layer != CKPT_SCAN_LAYER) return 0;

    ckpt_hdr_t dead;
    for (uint32_t i = 0; i < sizeof(dead) / 4; i++) ((uint32_t *)&dead)[i] = 0;
    if (ckpt_dev_write(slot, 0, &dead, CKPT_HDR_SZ) != 0) return 0;

    // g_next_slot is deliberately NOT touched. ckpt_save_scan already advanced
    // it to the other slot, and pointing it back here would undo the
    // alternation: every save would land on the same slot, leaving a torn write
    // with no intact predecessor to fall back on and concentrating all wear on
    // one set of cells. g_seq is left alone too -- sequence numbers must keep
    // increasing or a later record could look older than one already on media.
    return 1;
}

int ckpt_restore_scan(void *frame, uint32_t cap, uint16_t *next_pixel) {
    // Initialised for the compiler, which cannot see that newest_any fills it
    // whenever it returns non-zero. Same reason as the one in ckpt_restore.
    ckpt_hdr_t h = {0, 0, 0, 0, 0};
    uint32_t slot = 0;
    if (!newest_any(&h, &slot) || h.layer != CKPT_SCAN_LAYER) return 0;

    const uint32_t bytes = (uint32_t)h.unit * CKPT_SCAN_PIXEL_SZ;
    if (bytes > cap) return 0;

    uint32_t off = CKPT_HDR_SZ;
    if (bytes && read_padded(slot, off, frame, bytes, &off) != 0) return 0;
    if (crc32(frame, bytes, 0) != h.crc) return 0;

    if (next_pixel) *next_pixel = h.unit;
    g_seq = h.seq;
    g_next_slot = (slot + 1) % CKPT_NUM_SLOTS;
    return 1;
}
