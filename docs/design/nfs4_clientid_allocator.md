# NFSv4 Client ID Allocator

This document describes how the NFS server mints NFSv4 client IDs
(`clientid4`): the 64-bit layout, the allocator API and its contract, how
SETCLIENTID and EXCHANGE_ID use it, and the guarantees each allocation scheme
gives. It is the Phase 1 design: it fixes the layout rule and the public API
so that later allocators (for example one backed by a shared store) plug in
without changing either.

## Why

A client ID used to be `node_id << 48 | counter`, with a `node_id` that is the
same on every boot and a counter that restarted at 1 (issue #1377). The first
clients after a restart were handed exactly the IDs of the first clients
before it, so an NFSv4.0 client that missed the restart resolved its old ID
to another client and never learned it had to reclaim. RFC 7530 §9.1.1 and
§16.33.5 require client IDs (and `setclientid_confirm` values) not to be
regenerated across restarts.

Minting is now a separate module with a stated contract, so the uniqueness
guarantee is a property of the allocator, not of the registry.

## Layout

A client ID is opaque to clients and to every server layer except the
allocator and the diagnostic decoder. The only globally defined field is
**Type**, bits 63..60 of the host `uint64_t` (and so the first nibble on the
wire, since XDR encodes it big-endian). Type names the allocation scheme; the
remaining 60 bits belong to the Type, which may define its own Version.

| Type | Scheme | Status |
|---:|---|---|
| 0 | — | Reserved, never emitted |
| 1 | Supervisor grant | The only Type minted in Phase 1 |
| 2–15 | — | Unassigned; assigned by a change to this document |

A Type number is **never reused**, even after its Type is retired: a client
keeps its ID for as long as it keeps renewing, across failovers.

### Type 1 / Version 0 (supervisor grant)

```text
 63..60   59..58    57..48       47..44        43..24         23..0
+--------+--------+-------------+------------+--------------+------------+
| Type 1 | Ver 0  | Node ID(10) | Instance(4)| Generation(20)| Sequence(24)|
+--------+--------+-------------+------------+--------------+------------+
```

Node ID 1–1023, Instance 0–15, Generation **1**–1,048,575 (0 is reserved),
Sequence 1–16,777,215. Example: node 42, instance 3, generation 105,
sequence 100 is `0x102a300069000064`.

The supervisor that starts Chimera owns these values. Its obligations
(Chimera checks ranges only):

1. a unique `(Node ID, Instance)` per running Chimera process, fleet-wide;
2. a strictly increasing Generation per `(Node ID, Instance)`, from 1;
3. the Generation recorded durably before Chimera starts with it;
4. continuation from the fleet's high-water mark when a host is replaced and
   reuses its Node ID;
5. a crash-loop budget: each start uses one generation (about 24 days at one
   restart every 2 seconds), so back off between restarts;
6. when generations run out, a new Node ID or Instance from the operator.

The allocator never advances Generation itself.

### Older-format IDs

IDs minted before the allocator (`node_id << 48 | counter`) can start with any
nibble, so their Type bits mean nothing. Type 1 avoids them by never issuing
Generation 0: an old ID whose top nibble happens to be 1, such as
`0x102a000000000005`, decodes as Generation 0. Any future Type must state its
own rule.

## Opacity and the shared decoder

A client ID is an opaque 64-bit identity at ordinary call sites. Registry,
session, state, recovery and reply-cache code store, compare, hash, persist and
restore the full value and never mask or shift it:

- persisted IDs are stored and restored verbatim, including IDs in older or
  unknown formats; a decode failure never gates a lookup or a hydration;
- hydration never advances the allocator: a decoded Sequence describes the
  namespace that minted the ID (another node, instance or generation);
- ownership comes from recovery records (`owner_node`) and heartbeats, never
  from the ID;
- collision checks compare whole IDs and owners: a newly allocated ID that is
  already in the client table is discarded and another requested (up to 3
  times, then `NFS4ERR_SERVERFAULT`); a persisted session naming an ID held by
  a different owner is not attached (the client sees `NFS4ERR_BADSESSION`);
- stateid identity is independent of the client ID (see "Layout stateids").

When metadata is genuinely wanted, code may call the shared, pure decoder in
`clientid_format.h`:

```c
struct clientid_type1_v0_info { uint16_t node_id; uint8_t instance;
                                uint32_t generation; uint32_t sequence; };
struct clientid_decoded { uint8_t type;
                          union { struct clientid_type1_v0_info type1_v0; } payload;
                          uint8_t version; };
int clientid_decode(uint64_t clientid, struct clientid_decoded *out);
/* 0 = decoded; ENOTSUP = Type 0, an unknown Type, or Type 1 Version 1-3;
 * EINVAL = Node ID 0, Generation 0 or Sequence 0, or a NULL out. */
```

It dispatches on the Type bits only and is independent of the active
allocator, so it decodes IDs minted by any Type 1 allocator instance. Permitted
uses: logs, diagnostics, the `chimera-clientid` tool, and observing recovery.
A successful decode does not authenticate an ID or prove its provenance (an
older ID can look like Type 1), and a decoded Node ID is where the ID was
minted, not who owns the client now. Its result must never drive routing,
ownership, restore or expire decisions, allocator counters or stateid
identity. Only allocator backends encode.

## API

`src/server/nfs/clientid_allocator.h`:

```c
int  clientid_allocator_supported(const struct clientid_allocator_config *config);
int  clientid_allocator_create(const struct clientid_allocator_config *config,
                               struct clientid_allocator **out);
int  clientid_allocator_allocate_async(struct clientid_allocator *allocator,
                                       struct evpl *home,
                                       clientid_alloc_cb done, void *arg);
void clientid_allocator_destroy(struct clientid_allocator *allocator);
int  clientid_allocator_status_is_transient(int status);   /* inline */
```

Completion contract: an accepted allocation (return 0) calls `done` exactly
once, never inline, on the `home` event loop, with no allocator lock held; a
rejected one (return errno) never calls it. A backend may finish on any
thread; the common wrapper hands the result back. There is no cancellation;
the caller keeps `arg` alive. Abandoned IDs are never reused.

### Configuration

One JSON object, `server.nfs4_clientid_allocator` in the Chimera config:

```json
{
  "schema": 1,
  "backend": "local",
  "type": 2,
  "kwargs": { "version": 0 },
  "common": { "max_pending": 4096, "defer_retry_limit": 5, "defer_retry_backoff_ms": 10 }
}
```

- Unknown keys are rejected at every level (`EINVAL`); `ENOTSUP` means valid
  but not runnable by this build. A typo must never be ignored.
- `kwargs` belongs to the backend and Type; `common` to the wrapper.
- Type 1 kwargs: `version`, `node_id`, `instance`, `generation`.
- Secrets are referenced, never inlined, and redacted in the startup log.
- The configuration is fixed after `create()` and logged once at startup.

Resolution at startup: the configured object if present; otherwise, if any of
`CHIMERA_NODE_ID`, `CHIMERA_INSTANCE`, `CHIMERA_GENERATION` is set, local
Type 1 from all three (they must all be decimal). With neither, or with an
invalid result, **NFSv4 is not offered** (fail closed): the NFSv4 program is
not registered, so clients fall back to NFSv3 instead of mounting and failing
every registration. With NFSv3 also disabled (`nfs3_disabled`) there is
nothing to serve and startup fails.

Tests stand in for the supervisor: every harness that starts an NFS server
sets the three variables, with a Generation from the millisecond clock so each
restart gets a fresh one (`scripts/chimera_clientid_test_env.sh` for shell
wrappers, `src/common/test_clientid_env.h` for in-process servers, and an
equivalent helper in the Python daemon tests).

Backends: `local` (Type 1) and, in builds with tests, `test` (Type 1 with a
configurable completion delay from a worker thread).

### Status codes and NFS mapping

| Status | Meaning | Class | SETCLIENTID / EXCHANGE_ID reply |
|---|---|---|---|
| `EAGAIN` | `max_pending` reached, backend not ready, refill, store timeout, queueing failed before acceptance | transient | `NFS4ERR_DELAY` |
| `ENOMEM` | No memory for the request | transient | `NFS4ERR_DELAY` |
| `ESHUTDOWN` | Allocator being destroyed | transient | `NFS4ERR_DELAY` |
| `ENOSPC` | Namespace exhausted | permanent | `NFS4ERR_SERVERFAULT` |
| `EIO` | Operator needed, or any backend error outside this set | permanent | `NFS4ERR_SERVERFAULT` |
| `EINVAL` | Caller bug | permanent | `NFS4ERR_SERVERFAULT` |

A permanent condition is never answered `NFS4ERR_DELAY`: the Linux client
retries `NFS4ERR_DELAY` without limit while establishing its lease
(`nfs4_handle_reclaim_lease_error()`), and fails the mount on any other error.
Both errors are valid for both operations (RFC 7530 §13.2, RFC 8881 §15.2);
`NFS4ERR_RESOURCE` is not used because NFSv4.1 does not define it for
EXCHANGE_ID.

Exhaustion is logged at 90% and 99% of a run's sequence space, and once when
it runs out; new registrations then fail until restart, existing clients are
unaffected.

### The common wrapper

Every backend sits under a wrapper (`clientid_allocator.c`) that enforces the
contract once: the schema, the closed status set (anything else becomes `EIO`
and is logged), `max_pending` back-pressure, `ESHUTDOWN`, and delivery.
Delivery uses **one** evpl deferral per home loop and a per-loop list of
completed requests — never one deferral or timer per request, because libevpl
keeps armed deferrals and timers in fixed 256-entry arrays (fixed separately
to grow). A backend completing on another thread signals a per-loop doorbell;
if that signal fails it is retried `defer_retry_limit` times with backoff,
then Chimera aborts with a detailed log, because the callback is owed exactly
once, must not run inline, and losing it would leak the registration.

## Registration

`nfs4_registration.c` drives SETCLIENTID and EXCHANGE_ID:

1. Classify the request under the client-table lock. The classifiers take a
   candidate ID; without one, a case that needs a fresh ID changes nothing and
   reports `need_id`. Cases that need no new ID (RFC 8881 Case 2, the
   confirmed-record update, a v4.0 callback update) are answered at once.
2. Otherwise reserve the owner (keyed by the exact owner bytes; never the XID
   or the verifier) and ask the allocator.
3. While reserved, a request for the same owner that is **identical** (same
   verifier, principal, minor version, and callback arguments for v4.0 or
   flags and state-protection mode for v4.1) is **coalesced**: it waits and
   gets the same reply, on its own connection. Without an XID it cannot be
   told from a retransmission, and answering both alike is what a reply cache
   would do. Any other request for the owner gets `NFS4ERR_DELAY`.
4. When the allocator answers, classify again with the new ID (the client may
   have been confirmed, fenced or superseded meanwhile), run the collision
   check, finalize, remove the reservation, then reply to the original and to
   every coalesced request, each on the thread that received it.

The reservation is gone before the first reply, so Linux's second
EXCHANGE_ID (sent only after the first reply) is classified normally and gets
RFC 8881 Case 4 (C1 then C2). A retransmission arriving after completion also
follows the RFC cases; the client uses the reply to the request it actually
received an answer for.

`setclientid_confirm` verifiers stay in the NFS layer (a per-boot high half
and a random-start counter): `(client ID, confirm)` is unique because client
IDs are, and a confirm value never repeats for one client ID within a run.

## Changing authority

Exactly one authority owns a `(Type, Node ID, Instance)` namespace at a time.
A later backend that takes over a Type 1 namespace (for example a durable
local record) must start above the previous authority's high-water mark,
supplied as `kwargs.generation_floor`, or the node must take a new Node ID or
Instance.

## Future store-backed Types

A Type whose numbers come from a shared store must use an atomic conditional
write with exactly one winner, a strongly consistent store, durability before
acknowledgement, claims that never expire (no TTL leases, no lifecycle
rules), refills off the request path (block leases, not a write per ID), and a
stated response to a store restore or rollback. Chimera's current KV backends
are per-node stores (memkv in process, sqlite and cairn on local disk), so
their put-if-absent is atomic only within one node's store.

## Layout stateids

pNFS layout stateids carry the state table's epoch, like OPEN, LOCK and
delegation stateids, fixed in the layout state when it is created
(`stateid_epoch`) and reused by every re-encoding: a LAYOUTGET seqid bump and
CB_LAYOUTRECALL. They used to carry the low 32 bits of the client ID, which
made generic validation (TEST_STATEID) reject live layouts. Acquiring a layout
state from the state table now takes a reference, matching the reference
`nfs_state_table_release()` drops, so FREE_STATEID on a layout and teardown
racing it cannot underflow or free early.

## Diagnostics

`chimera-clientid decode [--json] <id>...` prints the fields of a Type 1 /
Version 0 ID or reports an unsupported or invalid encoding. It does not infer
the backend and knows nothing about live ownership.

## Tests

- `test_clientid_allocator`: the decoder (golden vector, `ENOTSUP` for Type 0,
  unknown Types and Versions 1–3, `EINVAL` for Node ID, Generation or Sequence
  0), configuration rejection, the status set, deferred exactly-once delivery,
  `max_pending`, exhaustion, 2,000 completions on one loop, cross-thread
  hand-back.
- `test_nfs_persist` (`test_clientid_registration`): `need_id`, coalescing,
  `NFS4ERR_DELAY`, collision retry and give-up, error mapping, Case 4 and
  Case 2, hydration with a pre-allocator ID and with a foreign owner.
- `clientid_allocator_e2e` (netns, pynfs): NFSv4 not offered without a grant,
  Type 1 from the environment, NFSv4 not offered on an incomplete grant, and a
  SETCLIENTID retransmitted on another connection while a delayed allocation
  is pending.
- `test_state_table` (`test_layout_stateid_epoch_and_refcount`): layout
  stateids carry the table epoch whatever the client ID, TEST_STATEID accepts
  them, a seqid bump keeps the identity, and acquire/release balance across a
  concurrent teardown.
- `v40_stale_clientid`: the #1377 regression across a server crash.
