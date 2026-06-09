# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & run

```bash
make                  # builds all six binaries
make clean            # removes binaries

# Launch order matters — backends must be up before serverM serves traffic
./serverA &
./serverB &
./serverC &
./serverM &
./client Alice                    # balance check (Caesar shift)
./client Alice Bob 100            # transfer
./client Alice XOR                # balance check (XOR)
./client Alice Bob 100 XOR        # transfer (XOR)
./monitor TXLIST                  # writes txchain.txt via serverM
```

Compiled with `-std=c++17 -pthread`. No external dependencies.

## Port layout

All ports are derived from `USC_ID = 218`:

| Process  | Protocol | Port  |
|----------|----------|-------|
| serverA  | UDP      | 21218 |
| serverB  | UDP      | 22218 |
| serverC  | UDP      | 23218 |
| serverM  | UDP      | 24218 |
| serverM  | TCP (clients) | 25218 |
| serverM  | TCP (monitor) | 26218 |

## Architecture

`serverM` is the coordinator. It owns one shared UDP socket (port 24218) and two TCP listening sockets. Each incoming TCP connection is handled in a detached `std::thread`. `udpMtx` serializes all UDP send/recv cycles so concurrent threads don't interleave datagrams.

The three backends (`serverA/B/C`) are identical except for their port and shard file (`block1/2/3.txt`). Each loads its shard into an in-memory `std::vector<Tx>` at startup and maintains a `seen_ids` set for idempotent TX deduplication.

## Non-obvious data flow

**Encryption layering:** Names and amounts travel on the wire encrypted, but `block*.txt` files always store data in Caesar-shift form regardless of client encryption mode. When a client uses XOR mode, `serverM` sends XOR-encrypted payloads to backends; backends XOR-decrypt then re-shift-encrypt before appending to their shard file.

**MAXID is binary:** The `MAXID` UDP response is a 4-byte big-endian `int32_t` (network byte order), unlike all other UDP responses which are plain text strings. `serverM` uses `ntohl` to decode it.

**txchain.txt is written by serverM, not monitor:** When monitor sends `TXLIST`, serverM collects all shards, merges and sorts by serial id, writes `txchain.txt` locally, then replies `TXLIST_OK`. The monitor only receives the confirmation string.

**commit_tx randomizes backend selection:** TX commits are sent to a randomly chosen backend (not always serverA) to spread load. If that backend is unresponsive, `commit_tx` fails over to the remaining ones. The serial id ensures the backend that eventually receives the retry is idempotent.
