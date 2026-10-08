// vcap_confirm.c -- see vcap_confirm.h.
#include "vcap_confirm.h"

static void clear_votes(vcap_confirm_t *c) {
    c->stop_votes = 0u;
    c->compute_votes = 0u;
    c->restore_votes = 0u;
}

void vcap_confirm_init(vcap_confirm_t *c, uint32_t stop_n, uint32_t resume_n) {
    if (!c) return;
    c->stop_n = stop_n < 1u ? 1u : stop_n;
    c->resume_n = resume_n < 1u ? 1u : resume_n;
    vcap_confirm_forget(c);
}

void vcap_confirm_forget(vcap_confirm_t *c) {
    if (!c) return;
    c->primed = 0u;
    c->compute = 0u;
    c->restore = 0u;
    clear_votes(c);
}

void vcap_confirm_invalid(vcap_confirm_t *c) {
    if (!c) return;
    clear_votes(c);
}

int vcap_confirm_stop_pending(const vcap_confirm_t *c) {
    return c && c->primed && c->compute && c->stop_votes > 0u;
}

uint32_t vcap_confirm_update(vcap_confirm_t *c, int raw_compute,
                             int raw_restore, int raw_critical) {
    if (!c) return 0u;
    // A zero-initialised struct (no init call yet) still behaves as N = 1.
    const uint32_t stop_n = c->stop_n < 1u ? 1u : c->stop_n;
    const uint32_t resume_n = c->resume_n < 1u ? 1u : c->resume_n;
    const int was_working = c->primed && c->compute && c->restore;
    uint32_t ev = 0u;

    if (raw_critical) {
        // Safety bypass: the sleep floor acts on the first reading.
        if (c->primed && c->compute) ev |= VC_EV_CRITICAL;
        c->primed = 1u;
        c->compute = 0u;
        c->restore = 0u;
        clear_votes(c);
        return ev;
    }

    if (!c->primed) {
        // Session start: compute is decided by this reading alone, as the
        // single-reading policy did; restore must still be confirmed below.
        c->primed = 1u;
        c->compute = raw_compute ? 1u : 0u;
        c->restore = 0u;
        clear_votes(c);
        ev |= VC_EV_PRIMED;
    } else if (c->compute) {
        if (raw_compute) {
            c->stop_votes = 0u;
        } else if (++c->stop_votes >= stop_n) {
            c->compute = 0u;
            c->restore = 0u;
            clear_votes(c);
            return ev | VC_EV_STOP;
        } else {
            ev |= VC_EV_STOP_VOTE;
        }
    } else {
        if (!raw_compute) {
            c->compute_votes = 0u;
        } else if (++c->compute_votes >= resume_n) {
            // Keep restore_votes: the same readings are confirming restore.
            c->compute = 1u;
            c->compute_votes = 0u;
            c->stop_votes = 0u;
        }
    }

    if (!raw_restore) {
        c->restore = 0u;
        c->restore_votes = 0u;
    } else if (!c->restore && ++c->restore_votes >= resume_n) {
        c->restore = 1u;
        c->restore_votes = 0u;
        c->stop_votes = 0u;
    }

    const int now_working = c->compute && c->restore;
    if (now_working && !was_working) {
        ev |= VC_EV_RESUME;
    } else if (!now_working && raw_compute && raw_restore) {
        ev |= VC_EV_RESUME_VOTE;
    }
    return ev;
}
