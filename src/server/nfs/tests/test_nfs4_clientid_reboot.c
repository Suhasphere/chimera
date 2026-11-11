// SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * Regression test for chimera-nas/chimera#1377:
 *   "[NFSv4.0] clientid/confirm verifier counters reset to 1 each restart and
 *    clientid encodes no boot epoch; stale 4.0 clientid can be misattributed"
 *
 * RFC 7530 §9.1.1: a client ID "should be chosen so that it will not conflict
 * with a client ID previously assigned by the server.  This applies across
 * server restarts or reboots."  §16.33.5: the server "must take care to ensure
 * that these values [clientid and setclientid_confirm] are extremely unlikely
 * to ever be regenerated."  §9.6.2 / §9.6.3.4.1: a pre-reboot client learns of
 * the reboot when its old clientid draws NFS4ERR_STALE_CLIENTID.
 *
 * A server restart is simulated in-process the same way test_nfs_persist.c
 * does it: tear the client table down and initialise a fresh one with the
 * SAME node_id (node_id is deliberately restart-stable, see nfs.c).  Each
 * "boot" then drives the production SETCLIENTID / SETCLIENTID_CONFIRM code
 * (nfs4_client_setclientid / nfs4_client_setclientid_confirm) and creates the
 * implicit v4.0 session exactly as chimera_nfs4_setclientid_confirm() does.
 *
 * The RENEW check uses nfs4_session_find_by_clientid(), which is the exact
 * lookup chimera_nfs4_renew() performs (nfs4_proc_renew.c): a NULL result is
 * NFS4ERR_STALE_CLIENTID, a hit is NFS4_OK on whatever client it found.
 *
 * Expected: every check FAILS on an unfixed tree (the process exits 1 and
 * prints which RFC rule was broken), and PASSES once clientids and confirm
 * verifiers carry a per-boot component.  The test does not depend on how the
 * fix is shaped -- only on the observable protocol behaviour.
 */

#include "common/thread.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common/logging.h"
#include "nfs_common.h"
#include "nfs4_session.h"
#include "nfs4_lease.h"
#include "nfs4_recovery.h"
#include "nfs4_drc.h"
#include "evpl/evpl.h"
#include "nfs4_state.h"

/* Non-fatal check: record the failure and keep going, so one run reports
 * every symptom of the defect rather than stopping at the first. */
static int failures;

#define EXPECT(cond, ...) do { \
            if (!(cond)) { \
                fprintf(stderr, "%s:%d: FAIL: %s\n    ", \
                        __FILE__, __LINE__, #cond); \
                fprintf(stderr, __VA_ARGS__); \
                fprintf(stderr, "\n"); \
                failures++; \
            } \
} while (0)

/* Hard precondition: the harness itself is broken, not the code under test. */
#define REQUIRE(cond) do { \
            if (!(cond)) { \
                fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", \
                        __FILE__, __LINE__, #cond); \
                abort(); \
            } \
} while (0)

/* Referenced from nfs4_callback.c (linked in); unused here. */
void
chimera_nfs4_open_resume_after_probe(struct nfs_request *req)
{
    (void) req;
} /* chimera_nfs4_open_resume_after_probe */

/* Every boot of the simulated server uses this node_id, as a real restart of
 * the same machine (or one with server.nfs4_node_id pinned) does. */
#define TEST_NODE_ID 5

/* AUTH_SYS root, the most common v4.0 principal.  Both clients share it so the
 * principal checks (CLID_INUSE) cannot mask the defect. */
static const struct nfs4_client_principal root_sys = {
    .flavor          = 1, /* AUTH_SYS */
    .uid             = 0,
    .gid             = 0,
    .machinename     = NULL,
    .machinename_len = 0,
};

/* What one simulated client learned from SETCLIENTID. */
struct registered {
    uint64_t clientid;
    uint8_t  confirm[NFS4_VERIFIER_SIZE];
};

static void
boot(struct nfs4_client_table *table)
{
    nfs4_client_table_init(table, TEST_NODE_ID);
} /* boot */

static void
crash(struct nfs4_client_table *table)
{
    nfs4_client_table_destroy_unified(table, NULL, NULL);
    nfs4_client_table_free(table);
} /* crash */

/* SETCLIENTID only (leaves an unconfirmed record). */
static void
setclientid(
    struct nfs4_client_table *table,
    const char               *owner,
    uint64_t                  verifier,
    struct registered        *out)
{
    struct nfs4_setclientid_result r;

    nfs4_client_setclientid(table, owner, (int) strlen(owner), verifier,
                            &root_sys, 0, &r);
    REQUIRE(r.status == NFS4_OK);
    REQUIRE(r.destroy_unified == NULL);

    out->clientid = r.clientid;
    memcpy(out->confirm, r.confirm, NFS4_VERIFIER_SIZE);
} /* setclientid */

/* SETCLIENTID + SETCLIENTID_CONFIRM + the implicit v4.0 session, mirroring
 * chimera_nfs4_setclientid_confirm(). */
static void
mount_v40(
    struct nfs4_client_table *table,
    const char               *owner,
    uint64_t                  verifier,
    struct registered        *out)
{
    struct nfs_client   *destroy = NULL;
    struct nfs4_session *s;

    setclientid(table, owner, verifier, out);

    REQUIRE(nfs4_client_setclientid_confirm(table, out->clientid, out->confirm,
                                            &root_sys, &destroy) == NFS4_OK);
    REQUIRE(destroy == NULL);

    s = nfs4_session_find_by_clientid(table, out->clientid);
    if (!s) {
        s = nfs4_create_session(table, out->clientid, 1, 0, 0,
                                NULL, NULL, NULL, false);
    }
    REQUIRE(s != NULL);
    nfs4_session_put(s);
} /* mount_v40 */

/* What chimera_nfs4_renew() would answer for `clientid`, and on whose behalf. */
static uint64_t
renew_resolves_to(
    struct nfs4_client_table *table,
    uint64_t                  clientid,
    bool                     *stale)
{
    struct nfs4_session *s = nfs4_session_find_by_clientid(table, clientid);
    uint64_t             owner_id;

    if (!s || !s->client_unified) {
        *stale = true;
        if (s) {
            nfs4_session_put(s);
        }
        return 0;
    }
    *stale   = false;
    owner_id = s->client_unified->client_id;
    nfs4_session_put(s);
    return owner_id;
} /* renew_resolves_to */

/* ------------------------------------------------------------------ *
*  1. A clientid issued after a restart must not equal one issued    *
*     before it (RFC 7530 §9.1.1, §16.33.5).                         *
* ------------------------------------------------------------------ */
static void
test_clientid_not_reissued_after_restart(void)
{
    struct nfs4_client_table table;
    struct registered        a, b;

    boot(&table);
    mount_v40(&table, "Linux NFSv4.0 client-A", 0xA0A0A0A0ULL, &a);
    crash(&table);

    boot(&table);
    mount_v40(&table, "Linux NFSv4.0 client-B", 0xB0B0B0B0ULL, &b);

    EXPECT(a.clientid != b.clientid,
           "boot #2 handed client B clientid 0x%016llx, the same value client A "
           "held before the restart (RFC 7530 §9.1.1: must not conflict with a "
           "clientid previously assigned, \"across server restarts\")",
           (unsigned long long) b.clientid);

    crash(&table);
    printf("ran: clientid_not_reissued_after_restart "
           "(A=0x%016llx B=0x%016llx)\n",
           (unsigned long long) a.clientid, (unsigned long long) b.clientid);
} /* test_clientid_not_reissued_after_restart */

/* ------------------------------------------------------------------ *
*  2. The issue's scenario: a partitioned pre-restart client sends   *
*     RENEW with its old clientid after another client registered.   *
*     It must get NFS4ERR_STALE_CLIENTID, not be matched to B.       *
* ------------------------------------------------------------------ */
static void
test_stale_renew_after_restart(void)
{
    struct nfs4_client_table table;
    struct registered        a, b;
    uint64_t                 hit;
    bool                     stale;

    /* Boot #1: A mounts and (in the real scenario) takes a lock, then is
     * partitioned from the server. */
    boot(&table);
    mount_v40(&table, "Linux NFSv4.0 client-A", 0xA0A0A0A0ULL, &a);
    crash(&table);

    /* Boot #2: B mounts first. */
    boot(&table);
    mount_v40(&table, "Linux NFSv4.0 client-B", 0xB0B0B0B0ULL, &b);

    /* The partition heals; A's renewal daemon sends RENEW(old clientid). */
    hit = renew_resolves_to(&table, a.clientid, &stale);

    EXPECT(stale,
           "RENEW(0x%016llx) from pre-restart client A resolved to live client "
           "0x%016llx (client B) and would be answered NFS4_OK instead of "
           "NFS4ERR_STALE_CLIENTID (RFC 7530 §9.6.2, §9.6.3.4.1 step 8): A never "
           "learns of the restart and never reclaims its locks",
           (unsigned long long) a.clientid, (unsigned long long) hit);

    crash(&table);
    printf("ran: stale_renew_after_restart\n");
} /* test_stale_renew_after_restart */

/* ------------------------------------------------------------------ *
*  3. A SETCLIENTID_CONFIRM from before the restart (delayed or      *
*     retransmitted) must not confirm a different client's record    *
*     after it (RFC 7530 §16.33.5: setclientid_confirm values must   *
*     not be regenerated either).                                    *
* ------------------------------------------------------------------ */
static void
test_stale_confirm_after_restart(void)
{
    struct nfs4_client_table table;
    struct registered        a, b;
    struct nfs_client       *destroy = NULL;
    nfsstat4                 st;

    /* Boot #1: A sends SETCLIENTID and gets {clientid, confirm}; its
     * SETCLIENTID_CONFIRM is delayed in the network. */
    boot(&table);
    setclientid(&table, "Linux NFSv4.0 client-A", 0xA0A0A0A0ULL, &a);
    crash(&table);

    /* Boot #2: B sends SETCLIENTID and has not confirmed yet. */
    boot(&table);
    setclientid(&table, "Linux NFSv4.0 client-B", 0xB0B0B0B0ULL, &b);

    EXPECT(memcmp(a.confirm, b.confirm, NFS4_VERIFIER_SIZE) != 0,
           "boot #2 regenerated the same setclientid_confirm verifier that "
           "boot #1 issued (RFC 7530 §16.33.5)");

    /* A's stale confirm finally arrives. */
    st = nfs4_client_setclientid_confirm(&table, a.clientid, a.confirm,
                                         &root_sys, &destroy);

    EXPECT(st == NFS4ERR_STALE_CLIENTID,
           "pre-restart SETCLIENTID_CONFIRM(0x%016llx) returned %d instead of "
           "NFS4ERR_STALE_CLIENTID (%d): it confirmed client B's record",
           (unsigned long long) a.clientid, (int) st,
           (int) NFS4ERR_STALE_CLIENTID);

    if (destroy) {
        nfs_client_destroy(destroy, NULL, NULL, false);
    }
    crash(&table);
    printf("ran: stale_confirm_after_restart\n");
} /* test_stale_confirm_after_restart */

/* ------------------------------------------------------------------ *
*  4. Across many restarts, the first clientid of each boot must     *
*     never repeat (catches a fix that only changes the value once,  *
*     or that derives the boot component from too coarse a clock).   *
* ------------------------------------------------------------------ */
#define NUM_RESTARTS 64

static void
test_first_clientid_unique_across_many_restarts(void)
{
    struct nfs4_client_table table;
    struct registered        r;
    uint64_t                 first[NUM_RESTARTS];
    int                      repeats = 0;

    for (int i = 0; i < NUM_RESTARTS; i++) {
        boot(&table);
        mount_v40(&table, "Linux NFSv4.0 client-A", 0xA0A0A0A0ULL, &r);
        first[i] = r.clientid;
        crash(&table);
    }

    for (int i = 0; i < NUM_RESTARTS; i++) {
        for (int j = i + 1; j < NUM_RESTARTS; j++) {
            if (first[i] == first[j]) {
                repeats++;
            }
        }
    }

    EXPECT(repeats == 0,
           "%d pair(s) of restarts (out of %d restarts) issued the same first "
           "clientid (e.g. 0x%016llx)",
           repeats, NUM_RESTARTS, (unsigned long long) first[0]);

    printf("ran: first_clientid_unique_across_many_restarts\n");
} /* test_first_clientid_unique_across_many_restarts */

/* ------------------------------------------------------------------ *
*  5. Sanity (must pass before AND after the fix): within one boot,  *
*     RENEW with a live clientid still resolves to that client.      *
* ------------------------------------------------------------------ */
static void
test_live_renew_same_boot(void)
{
    struct nfs4_client_table table;
    struct registered        a, b;
    uint64_t                 hit;
    bool                     stale;

    boot(&table);
    mount_v40(&table, "Linux NFSv4.0 client-A", 0xA0A0A0A0ULL, &a);
    mount_v40(&table, "Linux NFSv4.0 client-B", 0xB0B0B0B0ULL, &b);

    EXPECT(a.clientid != b.clientid, "two clients in one boot share a clientid");

    hit = renew_resolves_to(&table, a.clientid, &stale);
    EXPECT(!stale && hit == a.clientid,
           "RENEW for live client A did not resolve to A (stale=%d hit=0x%016llx)",
           (int) stale, (unsigned long long) hit);

    hit = renew_resolves_to(&table, b.clientid, &stale);
    EXPECT(!stale && hit == b.clientid,
           "RENEW for live client B did not resolve to B (stale=%d hit=0x%016llx)",
           (int) stale, (unsigned long long) hit);

    crash(&table);
    printf("ran: live_renew_same_boot\n");
} /* test_live_renew_same_boot */

int
main(void)
{
    chimera_log_init();

    test_live_renew_same_boot();
    test_clientid_not_reissued_after_restart();
    test_stale_renew_after_restart();
    test_stale_confirm_after_restart();
    test_first_clientid_unique_across_many_restarts();

    if (failures) {
        fprintf(stderr,
                "FAIL: %d check(s) failed -- issue #1377 reproduces "
                "(clientid / setclientid_confirm reused across restarts)\n",
                failures);
        return 1;
    }

    printf("PASS: NFSv4.0 clientids and confirm verifiers are unique across "
           "restarts\n");
    return 0;
} /* main */
