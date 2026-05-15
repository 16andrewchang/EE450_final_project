# EE450 Final Project — Distributed Transaction System

A C++/Linux backend that coordinates a banking-style transaction ledger across
four cooperating processes. One main server (`serverM`) acts as the
coordinator; three backend servers (`serverA`, `serverB`, `serverC`) each own
a disjoint shard of the ledger. Clients submit balance queries and transfer
requests over TCP, and the monitor process pulls a globally sorted view of
every transaction.

## Architecture

```
   client  ──TCP──►┐
                   │
  monitor  ──TCP──►├── serverM ──UDP──► serverA  (block1.txt)
                   │   (coord)  ──UDP──► serverB  (block2.txt)
                   │            ──UDP──► serverC  (block3.txt)
                   │
                   └─ txlog.wal (write-ahead log, fsync on append)
```

- **TCP** (`serverM` ↔ client / monitor) — request/response correctness matters.
- **UDP** (`serverM` ↔ backends) — coordination path, kept lightweight; the
  coordinator layers its own reliability on top (timeouts, retries, failover).
- **Ports** are derived from a fixed USC ID offset (`24218`, `25218`, …) so
  every component agrees on endpoints without configuration.
- **Wire encryption** — every payload carrying user data is encrypted on the
  wire. Two interchangeable schemes are supported: a Caesar shift used by
  default, and an XOR stream cipher selected by passing `XOR` on the command
  line (e.g. `./client Alice Bob 100 XOR`).

## Fault-tolerant coordination

The coordinator does not assume the UDP path is reliable.

- `udp_rpc()` in `serverM.cpp` sets `SO_RCVTIMEO` on the UDP socket and wraps
  every backend call in a bounded retry loop (`UDP_MAX_RETRIES = 3`).
- Balance queries that still time out after retries are treated as a zero
  contribution from that shard, so a single partitioned backend degrades
  rather than blocks the system.
- Transaction commits use `commit_tx()`, which **fails over** to a different
  backend when the chosen one is unresponsive. Backends dedupe by serial id,
  so a failover after a half-delivered ack does not double-write.

## Transaction logging (WAL)

Every transfer is logged before any backend mutation:

```
BEGIN <serial> <sender> <receiver> <amount> <xor_flag> ?
COMMIT <serial>
```

- `wal_append()` flushes the record under `walMtx` so concurrent client
  threads cannot interleave records.
- The `BEGIN` is written *before* the UDP RPC; the `COMMIT` is written
  *after* the backend acks. The gap between these two is the only window in
  which a crash can leave durable state ambiguous, and recovery handles
  exactly that window.

## Recovery

On startup `serverM` calls `replay_wal()`:

1. Scan `txlog.wal` once, collecting `BEGIN` and `COMMIT` records.
2. For every serial that has a `BEGIN` but no `COMMIT`, re-submit the
   transaction to the backends. The serial id is the same as the original
   attempt, so backends that already saw it return `LOG_OK` without
   appending a duplicate row.
3. Any serial that still cannot commit (all backends down) is left in the
   WAL and re-tried on the next boot.

Backends are idempotent by construction: each loads its `block*.txt` shard
at startup, builds an in-memory `seen_ids` set, and short-circuits any
re-delivered serial back to `LOG_OK`.

## Consistency under partition + concurrent load

- The WAL + idempotent backends together give **at-least-once delivery with
  exactly-once effect** for transfers. A partition during commit either
  leaves the BEGIN un-committed (recovered on restart) or the backend has
  already persisted the row (the retry is a no-op).
- The coordinator handles each TCP connection in a detached `std::thread`,
  so many balance queries and transfers can be in flight simultaneously.
  Shared state is small and guarded:
  - `udpMtx` serializes one request/response cycle on the shared UDP socket
    (so replies from a previous call cannot be misattributed to a later one).
  - `walMtx` serializes WAL appends so records stay framed.

## Files

| File           | Role                                                       |
|----------------|------------------------------------------------------------|
| `serverM.cpp`  | Coordinator: TCP front door, UDP RPC, WAL, recovery, threads. |
| `serverA.cpp`  | UDP backend, owns shard `block1.txt`, idempotent appends.  |
| `serverB.cpp`  | UDP backend, owns shard `block2.txt`, idempotent appends.  |
| `serverC.cpp`  | UDP backend, owns shard `block3.txt`, idempotent appends.  |
| `client.cpp`   | TCP client for `CHECK` / `TX` requests.                    |
| `monitor.cpp`  | TCP client for `TXLIST` requests; emits `txchain.txt`.     |
| `txlog.wal`    | Append-only WAL written by `serverM` (created at runtime). |
| `block1/2/3.txt` | Per-shard transaction logs.                              |

## Build & run

```
make
./serverM &
./serverA &
./serverB &
./serverC &
./client Alice              # balance check (Caesar)
./client Alice Bob 100      # transfer
./client Alice XOR          # balance check (XOR)
./monitor TXLIST            # produce txchain.txt
```

`make` builds with `-pthread` so `serverM`'s thread-per-connection model
links cleanly.
