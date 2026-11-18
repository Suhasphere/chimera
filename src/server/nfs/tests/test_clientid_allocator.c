// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Unit tests for the NFSv4 client ID allocator: the Type 1 / Type 2 layouts,
 * the locked configuration schema, the closed status set, and the wrapper's
 * completion contract (deferred, exactly once, on the home loop, capped by
 * max_pending, cross-thread hand-back).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "evpl/evpl.h"
#include "clientid_allocator_internal.h"
#include "clientid_format.h"

#define CHECK(cond) do { \
            if (!(cond)) { \
                fprintf(stderr, "%s:%d: CHECK failed: %s\n", \
                        __FILE__, __LINE__, #cond); \
                abort(); \
            } \
} while (0)

static json_t *
cfg_parse(const char *text)
{
    json_error_t err;
    json_t      *j = json_loads(text, 0, &err);

    if (!j) {
        fprintf(stderr, "bad test JSON: %s\n", err.text);
        abort();
    }
    return j;
} /* cfg_parse */

static int
supported(const char *text)
{
    json_t                          *j   = cfg_parse(text);
    struct clientid_allocator_config cfg = { .json = j };
    int                              rc  = clientid_allocator_supported(&cfg);

    json_decref(j);
    return rc;
} /* supported */

static struct clientid_allocator *
create(const char *text)
{
    json_t                          *j   = cfg_parse(text);
    struct clientid_allocator_config cfg = { .json = j };
    struct clientid_allocator       *a   = NULL;

    CHECK(clientid_allocator_create(&cfg, &a) == 0);
    json_decref(j);
    return a;
} /* create */

/* ------------------------------------------------------------------ */

static void
test_layout(void)
{
    struct clientid_decoded d;
    char                    buf[192];

    /* Golden vectors (allocator-private encoder, shared decoder). */
    CHECK(clientid_type1_encode(42, 3, 105, 100) == 0x102a300069000064ULL);
    CHECK(clientid_type1_encode(42, 0, 105, 1) == 0x102a000069000001ULL);

    CHECK(clientid_decode(0x102a300069000064ULL, &d) == 0);
    CHECK(d.type == 1 && d.version == 0);
    CHECK(d.payload.type1_v0.node_id == 42 && d.payload.type1_v0.instance == 3 &&
          d.payload.type1_v0.generation == 105 && d.payload.type1_v0.sequence == 100);

    /* Extremes round-trip. */
    CHECK(clientid_decode(clientid_type1_encode(1023, 15, 0xFFFFF, 0xFFFFFF), &d) == 0);
    CHECK(d.payload.type1_v0.node_id == 1023 && d.payload.type1_v0.instance == 15 &&
          d.payload.type1_v0.generation == 0xFFFFF && d.payload.type1_v0.sequence == 0xFFFFFF);

    /* An ID minted by a different Type 1 allocator instance decodes to its
     * own minting fields; nothing depends on the active allocator. */
    CHECK(clientid_decode(clientid_type1_encode(7, 2, 9, 5), &d) == 0);
    CHECK(d.payload.type1_v0.node_id == 7 && d.payload.type1_v0.instance == 2 &&
          d.payload.type1_v0.generation == 9 && d.payload.type1_v0.sequence == 5);

    /* Reserved Type 0, unknown Types, and Type 1 Versions 1..3: ENOTSUP,
     * never a fallback to Type 1 / V0. */
    CHECK(clientid_decode(0x0000000000000001ULL, &d) == ENOTSUP && d.type == 0);
    CHECK(clientid_decode(0x2000000000000001ULL, &d) == ENOTSUP && d.type == 2);
    CHECK(clientid_decode(0xF000000000000001ULL, &d) == ENOTSUP);
    CHECK(clientid_decode(0x1400000001000001ULL, &d) == ENOTSUP);   /* Ver 1 */
    CHECK(clientid_decode(0x1800000001000001ULL, &d) == ENOTSUP);   /* Ver 2 */
    CHECK(clientid_decode(0x1C00000001000001ULL, &d) == ENOTSUP);   /* Ver 3 */

    /* Type 1 / V0 with Node ID 0, Sequence 0 or Generation 0: EINVAL. */
    CHECK(clientid_decode(clientid_type1_encode(0, 0, 1, 1), &d) == EINVAL);
    CHECK(clientid_decode(clientid_type1_encode(1, 0, 1, 0), &d) == EINVAL);
    CHECK(clientid_decode(clientid_type1_encode(1, 0, 0, 1), &d) == EINVAL);
    /* A pre-allocator ID (node 0x102a, counter 5) decodes as Generation 0,
     * which Type 1 never issues. */
    CHECK(clientid_decode(0x102a000000000005ULL, &d) == EINVAL);

    /* Invalid out. */
    CHECK(clientid_decode(0x102a300069000064ULL, NULL) == EINVAL);

    clientid_format(0x102a000069000001ULL, buf, sizeof(buf));
    CHECK(strstr(buf, "generation 105") != NULL);
} /* test_layout */

static void
test_config(void)
{
    /* Valid. */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":42,\"instance\":0,\"generation\":105}}") == 0);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":42,\"instance\":0,\"generation\":7},"
                    "\"common\":{\"max_pending\":8,"
                    "\"defer_retry_limit\":0,\"defer_retry_backoff_ms\":0}}") == 0);
    /* Type 2 is not implemented (unassigned). */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":2,\"kwargs\":"
                    "{\"version\":0}}") == ENOTSUP);

    /* Generation 0 is reserved. */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":42,\"instance\":0,\"generation\":0}}") == EINVAL);
    /* Node ID 0 and > 1023, Instance > 15, Generation > 0xFFFFF. */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":0,\"instance\":0,\"generation\":1}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1024,\"instance\":0,\"generation\":1}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":16,\"generation\":1}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0,\"generation\":1048576}}") == EINVAL);
    /* Missing Generation: no silent default. */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0}}") == EINVAL);

    /* Unknown keys are rejected at every level (a typo must not be ignored). */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0,\"generaton\":5,"
                    "\"generation\":5}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0,\"generation\":1},\"extra\":true}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0,\"generation\":1},\"common\":{\"max_pendng\":3}}") ==
          EINVAL);

    /* Reserved Type 0 is EINVAL; an unimplemented Type, backend, schema or
     * Version is ENOTSUP. */
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":0,\"kwargs\":{}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":15,\"kwargs\":{}}") == ENOTSUP);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":3,\"kwargs\":{}}") == ENOTSUP);
    CHECK(supported("{\"schema\":1,\"backend\":\"etcd\",\"type\":1,\"kwargs\":{}}") == ENOTSUP);
    CHECK(supported("{\"schema\":2,\"backend\":\"local\",\"type\":1,\"kwargs\":{}}") == ENOTSUP);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":1,\"node_id\":1,\"instance\":0,\"generation\":1}}") == ENOTSUP);

    /* Malformed. */
    CHECK(supported("[]") == EINVAL);
    CHECK(supported("{\"backend\":\"local\",\"type\":1,\"kwargs\":{}}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1}") == EINVAL);
    CHECK(supported("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
                    "{\"version\":0,\"node_id\":1,\"instance\":0,\"generation\":1},\"common\":{\"max_pending\":0}}") ==
          EINVAL);

    CHECK(clientid_allocator_status_is_transient(EAGAIN));
    CHECK(clientid_allocator_status_is_transient(ENOMEM));
    CHECK(clientid_allocator_status_is_transient(ESHUTDOWN));
    CHECK(!clientid_allocator_status_is_transient(0));
    CHECK(!clientid_allocator_status_is_transient(ENOSPC));
    CHECK(!clientid_allocator_status_is_transient(EIO));
    CHECK(!clientid_allocator_status_is_transient(EINVAL));
} /* test_config */

/* ------------------------------------------------------------------ */

struct result {
    int      calls;
    int      status;
    uint64_t id;
};

static void
record(
    void    *arg,
    int      status,
    uint64_t id)
{
    struct result *r = arg;

    r->calls++;
    r->status = status;
    r->id     = id;
} /* record */

/* Run the loop until results r[0..n) have each been called.  (Extra passes
 * with nothing pending would block in epoll, so never spin blindly.) */
static void
spin_until(
    struct evpl   *evpl,
    struct result *r,
    int            n)
{
    for (;;) {
        int done = 1;

        for (int i = 0; i < n; i++) {
            if (!r[i].calls) {
                done = 0;
            }
        }
        if (done) {
            return;
        }
        evpl_continue(evpl);
    }
} /* spin_until */

static void
test_type1_deferred_and_exhaustion(struct evpl *evpl)
{
    struct clientid_allocator      *a;
    struct clientid_allocator_stats st;
    struct result                   r[3] = { 0 };
    struct clientid_decoded         d;

    a = create("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
               "{\"version\":0,\"node_id\":42,\"instance\":0,\"generation\":105}}");

    /* Never inline: nothing has run when allocate_async returns. */
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[0]) == 0);
    CHECK(r[0].calls == 0);
    spin_until(evpl, &r[0], 1);
    CHECK(r[0].calls == 1 && r[0].status == 0);
    CHECK(r[0].id == 0x102a000069000001ULL);   /* first ID of the incarnation */

    /* Exhaustion: the last Sequence is issued once, then ENOSPC; Generation
     * never changes. */
    clientid_allocator_local_set_next_sequence(a, CLIENTID_T1_SEQUENCE_MAX);
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[1]) == 0);
    spin_until(evpl, &r[1], 1);
    /* Exactly once: further passes did not call the first one again. */
    CHECK(r[0].calls == 1);
    CHECK(r[1].calls == 1 && r[1].status == 0);
    CHECK(clientid_decode(r[1].id, &d) == 0);
    CHECK(d.payload.type1_v0.generation == 105 &&
          d.payload.type1_v0.sequence == CLIENTID_T1_SEQUENCE_MAX);

    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[2]) == 0);
    spin_until(evpl, &r[2], 1);
    CHECK(r[2].calls == 1 && r[2].status == ENOSPC && r[2].id == 0);
    CHECK(!clientid_allocator_status_is_transient(r[2].status));

    clientid_allocator_get_stats(a, &st);
    CHECK(st.issued == 2 && st.failed == 1 && st.pending == 0 && st.remaining == 0);

    clientid_allocator_destroy(a);
} /* test_type1_deferred_and_exhaustion */

static void
test_max_pending(struct evpl *evpl)
{
    struct clientid_allocator *a;
    struct result              r[4] = { 0 };

    a = create("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
               "{\"version\":0,\"node_id\":42,\"instance\":0,\"generation\":7},"
               "\"common\":{\"max_pending\":2}}");

    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[0]) == 0);
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[1]) == 0);
    /* Third while two are pending: back-pressure, no callback. */
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[2]) == EAGAIN);
    CHECK(clientid_allocator_status_is_transient(EAGAIN));
    spin_until(evpl, r, 2);
    CHECK(r[0].calls == 1 && r[1].calls == 1 && r[2].calls == 0);
    CHECK(r[0].id == clientid_type1_encode(42, 0, 7, 1));
    CHECK(r[1].id == clientid_type1_encode(42, 0, 7, 2));

    /* Room again. */
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[3]) == 0);
    spin_until(evpl, &r[3], 1);
    CHECK(r[3].calls == 1 && r[3].status == 0);

    CHECK(clientid_allocator_allocate_async(a, NULL, record, &r[3]) == EINVAL);
    clientid_allocator_destroy(a);
} /* test_max_pending */

static void
test_many_one_deferral(struct evpl *evpl)
{
    /* 2000 completions on one loop in one pass: far more than libevpl's 256
     * deferral slots, which per-request deferrals would overflow. */
    enum { N = 2000 };
    struct clientid_allocator *a;
    struct result             *r    = calloc(N, sizeof(*r));
    uint64_t                   prev = 0;

    a = create("{\"schema\":1,\"backend\":\"local\",\"type\":1,\"kwargs\":"
               "{\"version\":0,\"node_id\":7,\"instance\":0,\"generation\":9},"
               "\"common\":{\"max_pending\":4096}}");
    for (int i = 0; i < N; i++) {
        CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[i]) == 0);
    }
    spin_until(evpl, r, N);
    for (int i = 0; i < N; i++) {
        CHECK(r[i].calls == 1 && r[i].status == 0);
        CHECK(r[i].id > prev);   /* unique and increasing */
        prev = r[i].id;
    }
    clientid_allocator_destroy(a);
    free(r);
} /* test_many_one_deferral */

#ifdef CHIMERA_CLIENTID_TEST_BACKEND
static void
test_cross_thread(struct evpl *evpl)
{
    struct clientid_allocator *a;
    struct result              r[2] = { 0 };
    struct timespec            start, now;

    a = create("{\"schema\":1,\"backend\":\"test\",\"type\":1,\"kwargs\":"
               "{\"version\":0,\"node_id\":3,\"instance\":0,\"generation\":5,"
               "\"delay_us\":20000}}");

    clock_gettime(CLOCK_MONOTONIC, &start);
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[0]) == 0);
    CHECK(clientid_allocator_allocate_async(a, evpl, record, &r[1]) == 0);
    CHECK(r[0].calls == 0 && r[1].calls == 0);

    for (;;) {
        evpl_continue(evpl);
        if (r[0].calls && r[1].calls) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        CHECK(now.tv_sec - start.tv_sec < 10);
    }
    CHECK(r[0].status == 0 && r[0].id == clientid_type1_encode(3, 0, 5, 1));
    CHECK(r[1].status == 0 && r[1].id == clientid_type1_encode(3, 0, 5, 2));

    clientid_allocator_destroy(a);
} /* test_cross_thread */
#endif /* ifdef CHIMERA_CLIENTID_TEST_BACKEND */

int
main(void)
{
    struct evpl *evpl;

    test_layout();
    test_config();

    evpl = evpl_create(NULL);
    test_type1_deferred_and_exhaustion(evpl);
    test_max_pending(evpl);
    test_many_one_deferral(evpl);
#ifdef CHIMERA_CLIENTID_TEST_BACKEND
    test_cross_thread(evpl);
#endif /* ifdef CHIMERA_CLIENTID_TEST_BACKEND */
    evpl_destroy(evpl);

    printf("test_clientid_allocator: all checks passed\n");
    return 0;
} /* main */
