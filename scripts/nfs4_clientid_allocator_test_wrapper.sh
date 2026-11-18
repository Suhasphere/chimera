#!/bin/bash
# SPDX-FileCopyrightText: 2026 Chimera-NAS Project Contributors
#
# SPDX-License-Identifier: Unlicense

# Usage: nfs4_clientid_allocator_test_wrapper.sh <chimera_binary> <pynfs_dir> <test_script>
#
# NFSv4 client ID allocator, end to end: runs one daemon per scenario in a
# network namespace and drives it with pynfs's NFSv4.0 client
# (src/server/nfs/tests/nfs4_clientid_allocator_e2e.py):
#
#   default     no grant and no allocator config   -> NFSv4 not offered
#   type1       CHIMERA_NODE_ID=42 INSTANCE=0 GENERATION=105 -> Type 1 IDs
#   notoffered  CHIMERA_NODE_ID only (incomplete)  -> NFSv4 not offered
#   coalesce    "test" backend, 400 ms per allocation -> retransmit coalescing
#
# Unlike the other wrappers this one does not use chimera_clientid_test_env:
# each scenario sets (or withholds) the grant itself.

set -u

SAVED_LD_PRELOAD="${LD_PRELOAD:-}"
unset LD_PRELOAD

CHIMERA_BINARY=$1; shift
PYNFS_DIR=$1; shift
TEST_SCRIPT=$1; shift

NETNS_NAME="clidalloc_$$_$(date +%s%N)"
BUILD_DIR=$(dirname "$(dirname "$CHIMERA_BINARY")")
SESSION_DIR=$(mktemp -d "${BUILD_DIR}/clidalloc_session_XXXXXX")
PYCOMPAT_DIR="${SESSION_DIR}/pycompat"
CHIMERA_PID=""
TEST_TIMEOUT="${CLIDALLOC_TIMEOUT:-120}"

stop_chimera() {
    if [ -n "$CHIMERA_PID" ]; then
        kill -TERM "$CHIMERA_PID" 2>/dev/null || true
        for i in $(seq 1 150); do
            kill -0 "$CHIMERA_PID" 2>/dev/null || break
            sleep 0.02
        done
        kill -9 "$CHIMERA_PID" 2>/dev/null || true
        wait "$CHIMERA_PID" 2>/dev/null || true
        CHIMERA_PID=""
    fi
}

cleanup() {
    stop_chimera
    ip netns delete "${NETNS_NAME}" 2>/dev/null || true
    rm -rf "$SESSION_DIR"
}
trap cleanup EXIT

if [ ! -f "${PYNFS_DIR}/nfs4.0/nfs4lib.py" ]; then
    echo "pynfs not found at ${PYNFS_DIR}; skipping"
    exit 77
fi

# write_config <file> <extra server JSON members, may be empty>
write_config() {
    cat > "$1" << EOF
{
    "common": { "rcu_reclaim_threads": 4 },
    "server": {
        "nfs_enabled": true,
        "threads": 4,
        "delegation_threads": 4,
        "nfs4_grace_time": 1,
        "external_portmap": false$2
    },
    "filesystems": { "fs0": { "module": "memfs" } },
    "mounts": { "share": { "module": "memfs", "path": "fs0" } },
    "exports": { "/share": { "path": "/share" } }
}
EOF
}

ip netns add "${NETNS_NAME}"
ip netns exec "${NETNS_NAME}" ip link set lo up
ulimit -l unlimited

mkdir -p "${PYCOMPAT_DIR}"
cat > "${PYCOMPAT_DIR}/sitecustomize.py" <<'EOF'
try:
    from xdrlib3 import Packer
except ImportError:
    Packer = None

if Packer is not None:
    _pack_string = Packer.pack_string

    def _chimera_pack_string(self, s):
        if isinstance(s, str):
            s = s.encode()
        return _pack_string(self, s)

    Packer.pack_string = _chimera_pack_string
EOF
export PYTHONPATH="${PYCOMPAT_DIR}:${PYNFS_DIR}:${PYTHONPATH:-}"
export PYNFS_DIR

# run_scenario <name> <extra server JSON> <log line that must appear> [env...]
run_scenario() {
    local scenario=$1 extra=$2 must=$3; shift 3
    local config="${SESSION_DIR}/${scenario}.json"
    local log="${SESSION_DIR}/chimera.${scenario}.log"

    write_config "$config" "$extra"
    if [ -n "${SAVED_LD_PRELOAD}" ]; then
        ip netns exec "${NETNS_NAME}" env "$@" LD_PRELOAD="${SAVED_LD_PRELOAD}" \
            "$CHIMERA_BINARY" -c "$config" > "$log" 2>&1 &
    else
        ip netns exec "${NETNS_NAME}" env "$@" \
            "$CHIMERA_BINARY" -c "$config" > "$log" 2>&1 &
    fi
    CHIMERA_PID=$!

    for i in $(seq 1 1500); do
        if grep -q "Server is ready." "$log" &&
           ip netns exec "${NETNS_NAME}" bash -c "echo > /dev/tcp/127.0.0.1/2049" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$CHIMERA_PID" 2>/dev/null; then
            echo "chimera daemon exited prematurely (${scenario})"
            cat "$log"
            exit 1
        fi
        sleep 0.02
    done
    sleep 2

    if ! grep -q "$must" "$log"; then
        echo "FAIL (${scenario}): log line not found: ${must}"
        cat "$log"
        exit 1
    fi

    local expect=0x0
    [ "$scenario" = "type1" ] && expect=0x102a000069000001

    ip netns exec "${NETNS_NAME}" timeout "${TEST_TIMEOUT}" \
        python3 "$TEST_SCRIPT" --host 127.0.0.1 --port 2049 --export share \
            --scenario "$scenario" --expect "$expect"
    local rc=$?
    stop_chimera
    if [ "$rc" != "0" ]; then
        echo "=== ${scenario} failed (rc ${rc}) ==="
        cat "$log"
        exit 1
    fi
}

run_scenario default "" "No NFSv4 client ID namespace"
run_scenario type1 "" "Type 1 (supervisor grant) node 42 instance 0" \
    CHIMERA_NODE_ID=42 CHIMERA_INSTANCE=0 CHIMERA_GENERATION=105
run_scenario notoffered "" "NFSv4 is NOT offered" CHIMERA_NODE_ID=42
run_scenario coalesce ',
        "nfs4_clientid_allocator": { "schema": 1, "backend": "test", "type": 1,
            "kwargs": { "version": 0, "node_id": 7, "instance": 0, "generation": 3,
                        "delay_us": 400000 } }' "Type 1 (supervisor grant) node 7"

echo "all allocator scenarios passed"
exit 0
