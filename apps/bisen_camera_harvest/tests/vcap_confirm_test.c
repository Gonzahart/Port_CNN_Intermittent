// Host test for the VCAP stop/resume confirmation (src/vcap_confirm.c).
//   cc -std=c99 -Wall -Wextra -Werror -I../src vcap_confirm_test.c \
//      ../src/vcap_confirm.c -o /tmp/vcap_confirm_test && /tmp/vcap_confirm_test
// Exercises the pure logic only; it is not hardware validation.
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "vcap_confirm.h"

// Raw readings as bisen_policy.cc would classify them (restore implies
// compute; critical implies neither).
enum { CRIT, LOW, MID, HIGH };  // <5.8, 5.8-6.2, 6.2-6.8, >=6.8 V
static uint32_t feed(vcap_confirm_t *c, int level) {
    return vcap_confirm_update(c, level >= MID, level >= HIGH, level == CRIT);
}
static int working(const vcap_confirm_t *c) { return c->compute && c->restore; }

static void test_defaults_match_raw(void) {
    // N = 1: confirmed flags equal the raw flags after every valid reading,
    // including priming, critical readings and invalid readings in between.
    vcap_confirm_t c;
    vcap_confirm_init(&c, 1u, 1u);
    uint32_t x = 12345u;
    for (int i = 0; i < 100000; ++i) {
        x = x * 1103515245u + 12345u;
        if (((x >> 8) & 31u) == 0u) {
            vcap_confirm_invalid(&c);
            continue;
        }
        if (((x >> 8) & 1023u) == 1u) vcap_confirm_forget(&c);
        const int level = (int)((x >> 16) & 3u);
        feed(&c, level);
        assert(c.compute == (level >= MID));
        assert(c.restore == (level >= HIGH));
        assert(!vcap_confirm_stop_pending(&c));
    }
    // A zero-initialised struct (no init call) also behaves as N = 1.
    vcap_confirm_t z = {0};
    feed(&z, MID);
    assert(z.compute && !z.restore);
    feed(&z, LOW);
    assert(!z.compute);
}

static void test_priming(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    // Fresh session above work100: compute primes from one reading, as the
    // single-reading policy did; restore still needs three readings.
    assert(feed(&c, MID) & VC_EV_PRIMED);
    assert(c.compute && !c.restore);
    // Primed below work100: not working, and a stop is not pending.
    vcap_confirm_forget(&c);
    feed(&c, LOW);
    assert(!c.compute && !vcap_confirm_stop_pending(&c));
    // Primed at >= 6.8 V: compute at once, restore after resume_n readings.
    vcap_confirm_forget(&c);
    feed(&c, HIGH);
    assert(c.compute && !c.restore);
    feed(&c, HIGH);
    assert(!c.restore);
    assert(feed(&c, HIGH) & VC_EV_RESUME);
    assert(working(&c));
}

static void test_single_outlier_ignored(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    feed(&c, MID);
    // One low outlier while working: still working, stop pending.
    uint32_t ev = feed(&c, LOW);
    assert(ev & VC_EV_STOP_VOTE);
    assert(!(ev & VC_EV_STOP));
    assert(c.compute && vcap_confirm_stop_pending(&c));
    // A normal reading clears the pending stop.
    feed(&c, MID);
    assert(c.compute && !vcap_confirm_stop_pending(&c));
    // Two low readings separated by a normal one never stop.
    feed(&c, LOW); feed(&c, LOW); feed(&c, MID);
    feed(&c, LOW); feed(&c, LOW); feed(&c, MID);
    assert(c.compute);

    // Waiting: one high outlier must not resume.
    for (int i = 0; i < 3; ++i) feed(&c, LOW);
    assert(!c.compute && !c.restore);
    ev = feed(&c, HIGH);
    assert(ev & VC_EV_RESUME_VOTE);
    assert(!working(&c));
    feed(&c, MID);  // breaks the resume run
    feed(&c, HIGH); feed(&c, HIGH);
    assert(!working(&c));
}

static void test_n_consecutive_switch(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    feed(&c, MID);
    assert(!(feed(&c, LOW) & VC_EV_STOP));
    assert(!(feed(&c, LOW) & VC_EV_STOP));
    assert(feed(&c, LOW) & VC_EV_STOP);  // third consecutive reading stops
    assert(!c.compute && !c.restore && !vcap_confirm_stop_pending(&c));

    // Mid-band readings while waiting confirm compute but not restore: the
    // post-wait resume edge (compute && restore) is not met below 6.8 V.
    for (int i = 0; i < 5; ++i) feed(&c, MID);
    assert(c.compute && !c.restore);
    // Exactly resume_n consecutive readings >= 6.8 V resume.
    assert(!(feed(&c, HIGH) & VC_EV_RESUME));
    assert(!(feed(&c, HIGH) & VC_EV_RESUME));
    assert(feed(&c, HIGH) & VC_EV_RESUME);
    assert(working(&c));

    // Straight from a stop: compute and restore confirm on the same three
    // readings (one rising does not restart the other's count).
    for (int i = 0; i < 3; ++i) feed(&c, LOW);
    feed(&c, HIGH); feed(&c, HIGH);
    assert(!c.compute && !c.restore);
    assert(feed(&c, HIGH) & VC_EV_RESUME);
    assert(working(&c));

    // Asymmetric counts.
    vcap_confirm_init(&c, 2u, 4u);
    feed(&c, HIGH); for (int i = 0; i < 3; ++i) feed(&c, HIGH);
    assert(working(&c));
    feed(&c, LOW);
    assert(c.compute);
    feed(&c, LOW);
    assert(!c.compute);
    for (int i = 0; i < 3; ++i) feed(&c, HIGH);
    assert(!working(&c));
    feed(&c, HIGH);
    assert(working(&c));
}

static void test_restore_drops_immediately(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    for (int i = 0; i < 3; ++i) feed(&c, HIGH);
    assert(working(&c));
    feed(&c, MID);  // below the resume edge: restore off, compute still on
    assert(c.compute && !c.restore && !vcap_confirm_stop_pending(&c));
}

static void test_invalid_readings(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    feed(&c, MID);
    // Invalid readings are not votes and reset a pending stop run.
    feed(&c, LOW); feed(&c, LOW);
    assert(vcap_confirm_stop_pending(&c));
    vcap_confirm_invalid(&c);
    assert(c.compute && !vcap_confirm_stop_pending(&c));
    feed(&c, LOW); feed(&c, LOW);
    assert(c.compute);  // run restarted: only two votes since the invalid one
    feed(&c, LOW);
    assert(!c.compute);
    // ...and a pending resume run.
    feed(&c, HIGH); feed(&c, HIGH);
    vcap_confirm_invalid(&c);
    feed(&c, HIGH); feed(&c, HIGH);
    assert(!working(&c));
    feed(&c, HIGH);
    assert(working(&c));
    // Many invalid readings change nothing on their own.
    for (int i = 0; i < 10; ++i) vcap_confirm_invalid(&c);
    assert(working(&c));
    // An unprimed confirmer stays unprimed through invalid readings.
    vcap_confirm_forget(&c);
    vcap_confirm_invalid(&c);
    assert(!c.primed && !c.compute && !c.restore);
}

static void test_critical_bypass(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    for (int i = 0; i < 3; ++i) feed(&c, HIGH);
    assert(working(&c));
    // One critical reading stops at once, with no confirmation.
    uint32_t ev = feed(&c, CRIT);
    assert(ev & VC_EV_CRITICAL);
    assert(!c.compute && !c.restore);
    // Also while a stop is already pending.
    vcap_confirm_init(&c, 5u, 3u);
    feed(&c, MID);
    feed(&c, LOW);
    assert(vcap_confirm_stop_pending(&c));
    assert(feed(&c, CRIT) & VC_EV_CRITICAL);
    assert(!c.compute && !vcap_confirm_stop_pending(&c));
    // Critical while already waiting is not a new bypass.
    assert(!(feed(&c, CRIT) & VC_EV_CRITICAL));
    // Critical as the first reading of a session primes "not working".
    vcap_confirm_forget(&c);
    feed(&c, CRIT);
    assert(c.primed && !c.compute);
    // Critical breaks a resume run.
    feed(&c, HIGH); feed(&c, HIGH); feed(&c, CRIT);
    feed(&c, HIGH); feed(&c, HIGH);
    assert(!working(&c));
}

static void test_counters_reset_on_transitions(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 3u, 3u);
    feed(&c, MID);
    feed(&c, LOW); feed(&c, LOW); feed(&c, LOW);  // stop
    assert(c.stop_votes == 0u && c.compute_votes == 0u &&
           c.restore_votes == 0u);
    for (int i = 0; i < 3; ++i) feed(&c, HIGH);   // resume
    assert(working(&c));
    assert(c.stop_votes == 0u && c.compute_votes == 0u &&
           c.restore_votes == 0u);
    // After the resume a fresh stop needs the full count again.
    feed(&c, LOW); feed(&c, LOW);
    assert(c.compute);
    // Session start (forget) clears everything, including a pending stop.
    vcap_confirm_forget(&c);
    assert(!c.primed && c.stop_votes == 0u && !vcap_confirm_stop_pending(&c));
    assert(c.stop_n == 3u && c.resume_n == 3u);
}

static void test_init_clamps(void) {
    vcap_confirm_t c;
    vcap_confirm_init(&c, 0u, 0u);
    assert(c.stop_n == 1u && c.resume_n == 1u);
}

int main(void) {
    test_defaults_match_raw();
    test_priming();
    test_single_outlier_ignored();
    test_n_consecutive_switch();
    test_restore_drops_immediately();
    test_invalid_readings();
    test_critical_bypass();
    test_counters_reset_on_transitions();
    test_init_clamps();
    printf("vcap_confirm_test: PASS\n");
    return 0;
}
