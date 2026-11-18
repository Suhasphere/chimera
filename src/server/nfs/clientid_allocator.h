// SPDX-FileCopyrightText: 2025-2026 Chimera-NAS Project Contributors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once

/*
 * NFSv4 client ID allocator: the locked public API.
 *
 * The NFS client registry decides *whether* SETCLIENTID or EXCHANGE_ID needs a
 * new client ID; the allocator only issues a fresh opaque uint64_t in its
 * advertised safety scope.  It never sees owners, verifiers, principals,
 * XIDs, leases or sessions, and nothing outside the allocator and the
 * diagnostic decoder may interpret the bits it returns (clientid_format.h).
 *
 * Configuration is one JSON object (locked schema, unknown keys rejected at
 * every level):
 *
 *   {
 *     "schema":  1,
 *     "backend": "local",            "local" (Types 1, 2); "test" in test builds
 *     "type":    2,                  see the Type registry in clientid_format.h
 *     "kwargs":  { ... },            owned and validated by the backend + Type
 *     "common":  {                   owned and validated by the wrapper
 *       "max_pending":            4096,
 *       "defer_retry_limit":      5,
 *       "defer_retry_backoff_ms": 10
 *     }
 *   }
 *
 * Completion contract for clientid_allocator_allocate_async():
 *   - return 0: accepted; `done` runs exactly once, never inline, on the
 *     `home` event loop, with no allocator lock held;
 *   - return an errno: rejected; `done` never runs;
 *   - callers submit from the thread that owns `home`; a backend may finish
 *     on any thread and the wrapper hands the result back to `home`;
 *   - at most common.max_pending accepted requests are outstanding;
 *   - an ID that is allocated but never published is abandoned, never reused;
 *   - there is no cancellation: the caller keeps `arg` alive until `done`.
 *
 * Statuses form a closed set, each with a fixed class:
 *
 *   0          allocated
 *   EAGAIN     transient: max_pending reached, backend not ready, refill in
 *              progress, store timeout/unreachable, queueing failed before
 *              acceptance
 *   ENOMEM     transient: no memory for the request
 *   ESHUTDOWN  transient: the allocator is being destroyed
 *   ENOSPC     permanent: this allocator's namespace is exhausted
 *   EIO        permanent: backend failure needing an operator, or any
 *              backend error outside this set
 *   EINVAL     permanent: caller bug
 *
 * The registry maps transient statuses to NFS4ERR_DELAY and permanent ones to
 * NFS4ERR_SERVERFAULT (never a permanent condition to DELAY: the Linux client
 * retries NFS4ERR_DELAY without limit while establishing its lease).
 */

#include <errno.h>
#include <stdint.h>
#include <jansson.h>

struct evpl;
struct clientid_allocator;

struct clientid_allocator_config {
    const json_t *json;   /* the resolved configuration object */
};

typedef void (*clientid_alloc_cb)(
    void    *arg,
    int      status,
    uint64_t clientid);

/* Configuration preflight.  0 = supported; EINVAL = malformed or out of
 * range (including any unknown key); ENOTSUP = valid but this build cannot run
 * it (unknown backend, a Type the backend does not support, a reserved
 * Version).  It cannot certify that an external namespace grant is unique. */
int
clientid_allocator_supported(
    const struct clientid_allocator_config *config);

/* Validates again (even if supported() already ran), logs the redacted
 * configuration once, and builds the allocator.  Performs no network I/O. */
int
clientid_allocator_create(
    const struct clientid_allocator_config *config,
    struct clientid_allocator             **out);

int
clientid_allocator_allocate_async(
    struct clientid_allocator *allocator,
    struct evpl               *home,
    clientid_alloc_cb          done,
    void                      *arg);

/* The caller must stop starting allocations and let every accepted callback
 * run first.  Call it on a home thread, or after the home event loops have
 * been destroyed.  Returns ESHUTDOWN to any allocation attempted once
 * destruction has begun. */
void
clientid_allocator_destroy(
    struct clientid_allocator *allocator);

/* 1 if `status` is transient (retry later: NFS4ERR_DELAY), 0 if it is OK or
 * permanent. */
static inline int
clientid_allocator_status_is_transient(int status)
{
    return status == EAGAIN || status == ENOMEM || status == ESHUTDOWN;
} /* clientid_allocator_status_is_transient */
