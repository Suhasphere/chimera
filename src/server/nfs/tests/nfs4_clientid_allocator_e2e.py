#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense
"""
End-to-end checks for the NFSv4 client ID allocator against a real daemon,
driven by scripts/nfs4_clientid_allocator_test_wrapper.sh (one scenario per
daemon start):

  default    no supervisor grant and no allocator configuration: NFSv4 is
             not offered (fail closed), rather than minting IDs nobody
             granted.
  type1      CHIMERA_NODE_ID/INSTANCE/GENERATION set by a "supervisor": the
             first client ID of the incarnation is exactly
             Type 1 / node / instance / generation / sequence 1.
  notoffered an incomplete supervisor grant: NFSv4 is not offered at all
             (the NFSv4 program is not registered), rather than registering
             clients it cannot give IDs to.
  coalesce   the "test" backend delays every allocation: an identical
             SETCLIENTID that arrives on another connection while the first
             is pending (a retransmission after a reconnect) gets the SAME
             client ID and confirm verifier, with no NFS4ERR_DELAY; a
             SETCLIENTID for the same owner with a different verifier gets
             NFS4ERR_DELAY; once the first completes, the RFC cases apply
             unchanged.

This test decodes client IDs only to check the layout from outside; the
server never does.
"""
import argparse
import os
import sys
import threading
import time

_PYNFS = os.environ.get("PYNFS_DIR", "/opt/pynfs")
sys.path.insert(0, os.path.join(_PYNFS, "nfs4.0"))
sys.path.insert(1, os.path.join(_PYNFS, "nfs4.0", "lib"))

import nfs4lib                                              # noqa: E402
from nfs4lib import op4                                     # noqa: E402
from xdrdef.nfs4_const import *                             # noqa: E402
from xdrdef.nfs4_type import (nfs_client_id4, cb_client4,   # noqa: E402
                              clientaddr4)
import rpc.rpc as rpc                                       # noqa: E402

nfs4lib.SHOW_TRAFFIC = False
FAILED = []


def check(label, ok, detail=""):
    print("    %-52s %s  %s" % (label, "ok" if ok else "FAIL", detail))
    if not ok:
        FAILED.append(label)


def new_client(args, who):
    sec = rpc.SecAuthSys(0, b"chimalloc", 0, 0, [])
    return nfs4lib.NFS4Client(("alloc-%s-%d" % (who, os.getpid())).encode(),
                              args.host, args.port,
                              homedir=[args.export.encode()], sec_list=[sec])


def setclientid_op(owner, verifier):
    # Fixed callback arguments, so two connections send byte-identical
    # SETCLIENTID arguments (pynfs gives each client its own cb program).
    cb = cb_client4(0x40000000, clientaddr4(b"tcp", b"0.0.0.0.0.0"))
    return op4.setclientid(nfs_client_id4(verifier, owner), cb, 0)


def compound_once(c, ops):
    """One COMPOUND, without pynfs's automatic retry on NFS4ERR_DELAY (which
    is what a real client does too, so the plain compound() would hide it)."""
    from xdrdef.nfs4_type import COMPOUND4args
    p, un_p = c.nfs4packer, c.nfs4unpacker
    p.reset()
    p.pack_COMPOUND4args(COMPOUND4args(argarray=ops, tag=c.create_tag(),
                                       minorversion=0))
    un_p.reset(c.call(NFSPROC4_COMPOUND, p.get_buffer()))
    res = un_p.unpack_COMPOUND4res()
    un_p.done()
    return res


def setclientid(c, owner, verifier):
    res = compound_once(c, [setclientid_op(owner, verifier)])
    if res.status != NFS4_OK:
        return res.status, None, None
    ok = res.resarray[0].switch.switch
    return NFS4_OK, ok.clientid, ok.setclientid_confirm


def fields(cid):
    return {"type": cid >> 60, "version": (cid >> 58) & 3}


def scenario_default(args):
    scenario_notoffered(args)


def scenario_type1(args):
    a = new_client(args, "A")
    cid, _ = a.init_connection()
    expect = int(args.expect, 0)
    print("client ID: %#x (expected %#x)" % (cid, expect))
    check("first ID is Type 1 / node / instance / generation / seq 1",
          cid == expect, "%#x" % cid)
    b = new_client(args, "B")
    cid2, _ = b.init_connection()
    check("second ID is the next sequence", cid2 == expect + 1, "%#x" % cid2)


def scenario_notoffered(args):
    try:
        a = new_client(args, "A")
        a.null()
        a.compound([op4.renew(1)])
        check("NFSv4 not offered", False, "an NFSv4 call succeeded")
    except Exception as e:  # RPC version/program mismatch
        check("NFSv4 not offered", True, type(e).__name__)


def scenario_coalesce(args):
    owner = ("alloc-coalesce-%d" % os.getpid()).encode()
    verifier = b"verif001"
    conns = [new_client(args, "C%d" % i) for i in range(3)]
    results = {}

    def run(key, c, verf, delay):
        time.sleep(delay)
        t0 = time.time()
        results[key] = setclientid(c, owner, verf) + (time.time() - t0,)

    threads = [
        threading.Thread(target=run, args=("original", conns[0], verifier, 0.0)),
        threading.Thread(target=run, args=("retransmit", conns[1], verifier, 0.15)),
        threading.Thread(target=run, args=("different", conns[2], b"verif002", 0.15)),
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    o, r, d = results["original"], results["retransmit"], results["different"]
    print("original   : %s %s %s %.2fs" % (o[0], o[1] and hex(o[1]), o[2] and o[2].hex(), o[3]))
    print("retransmit : %s %s %s %.2fs" % (r[0], r[1] and hex(r[1]), r[2] and r[2].hex(), r[3]))
    print("different  : %s %.2fs" % (d[0], d[3]))
    check("original succeeds after the allocator delay", o[0] == NFS4_OK and o[3] > 0.2)
    check("identical request while pending: same reply, no DELAY",
          r[0] == NFS4_OK and r[1] == o[1] and r[2] == o[2])
    check("different verifier while pending: NFS4ERR_DELAY",
          d[0] == NFS4ERR_DELAY, str(d[0]))

    # After completion nothing is coalesced: the RFC cases apply.  An
    # unconfirmed record with the same verifier is replaced (new ID).
    s, cid_after, conf_after = setclientid(conns[1], owner, verifier)
    check("after completion: RFC 7530 replacement (new ID)",
          s == NFS4_OK and cid_after not in (None, o[1]),
          "%s %s" % (s, cid_after and hex(cid_after)))
    if s == NFS4_OK:
        s = conns[1].compound([op4.setclientid_confirm(cid_after, conf_after)]).status
        check("and it confirms", s == NFS4_OK, str(s))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=2049)
    ap.add_argument("--export", default="share")
    ap.add_argument("--scenario", required=True,
                    choices=["default", "type1", "notoffered", "coalesce"])
    ap.add_argument("--expect", default="0")
    args = ap.parse_args()

    print("scenario %s" % args.scenario)
    globals()["scenario_" + args.scenario](args)
    if FAILED:
        print("FAILED: %s" % ", ".join(FAILED))
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
