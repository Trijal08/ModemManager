/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * The configuration the CP expects at boot.
 *
 * This replaces a generated replay of one captured Android boot. Every entry
 * here is either a device constant whose invariance was measured, a value
 * derived at run time from this device's own state, or a documented standard.
 * Nothing is carried over because it happened to appear in a capture.
 *
 * Copyright (C) 2026 Trijal Saha
 */

#ifndef MM_SIT_BOOT_CONFIG_H
#define MM_SIT_BOOT_CONFIG_H

#include <glib.h>

typedef enum {
    /* Before the radio is brought up. */
    MM_SIT_BOOT_PHASE_BOOT,
    /* Once the radio is on, the bulk of the configuration. */
    MM_SIT_BOOT_PHASE_RADIO,
    /*
     * Immediately before an IMS context is asked for. Stock runs a short block
     * here and the CP's last impression before the request is what it contains.
     */
    MM_SIT_BOOT_PHASE_PRE_PDN,
} MMSitBootPhase;

typedef enum {
    MM_SIT_BOOT_SUB_NONE,
    /* @0x0c: this SIM's MCCMNC, ASCII, NUL-padded to the field width. */
    MM_SIT_BOOT_SUB_MCCMNC,
    /* @0x0c: the logical stack this request is about. */
    MM_SIT_BOOT_SUB_STACK,
    /* @0x09: the current time, as the CP's six-byte calendar. */
    MM_SIT_BOOT_SUB_WALLCLOCK,
    /*
     * A 16-bit millisecond counter at payload+2, little-endian. Stock's runs
     * at exactly 1000/s and wraps at 65536 (~65s): across one capture it read
     * 407 at +10.38s and 42577 at +52.55s, and (42577-407)/(52.55-10.38) is
     * 1000.0. A replayed constant tells the CP the same instant every frame.
     */
    MM_SIT_BOOT_SUB_FRAMETIME,
    /* The whole payload is built at run time; see mm_sit_boot_build_payload(). */
    MM_SIT_BOOT_SUB_BUILD,
} MMSitBootSub;

typedef enum {
    /* Sent on every logical stack. */
    MM_SIT_BOOT_STACKS_ALL,
    /*
     * Sent only on stack 0. Stock does this for these three in all five
     * captures, across two baseband builds, so it is a property of the
     * primary stack and not of the SIM that happened to be in it.
     */
    MM_SIT_BOOT_STACKS_PRIMARY,
} MMSitBootStacks;

typedef struct {
    guint16          id;
    /* What this configures, for the reader and for the logs. */
    const gchar     *what;
    MMSitBootPhase   phase;
    /*
     * How many times stock sends this before its IMS context request, per
     * stack: [0] on ch 0xf5, [1] on 0xf6. { 0, 0 } means once, so an entry
     * that does not care need say nothing.
     *
     * Only counts identical in all three boot captures are stated. The ones
     * that vary between them -- 0x0106, 0x0107, 0x0703, 0x0943, 0x0944,
     * 0x0c20, 0x0c33, 0x0d17, 0x0d3a, 0x0d3b, 0x0d3c, 0x0108, 0x0740 -- have
     * no single right answer, and pinning one capture's number is exactly how
     * the replay table this replaced went stale.
     */
    guint8           repeat[2];
    MMSitBootSub     sub;
    MMSitBootStacks  stacks;
    guint16          len;
    const guint8    *payload;
} MMSitBootRequest;

/* The whole configuration, in the order it is sent. */
const MMSitBootRequest *mm_sit_boot_requests (gsize *n_requests);

/* How many times to send this on `stack`. */
guint mm_sit_boot_request_repeat (const MMSitBootRequest *request, guint stack);

/* True when this request belongs on `stack`. */
gboolean mm_sit_boot_request_wanted (const MMSitBootRequest *request, guint stack);

/*
 * Builds the payload for a MM_SIT_BOOT_SUB_BUILD request. Returns a newly
 * allocated buffer and its length, or NULL if this request does not build one.
 */
guint8 *mm_sit_boot_build_payload (const MMSitBootRequest *request, gsize *len);

#endif /* MM_SIT_BOOT_CONFIG_H */
