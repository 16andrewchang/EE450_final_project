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
./monitor TXLIST XOR              # same, but requests XOR-encoded shard data
```

Compiled with `-std=c++17 -pthread`. No external dependencies. No automated test suite — testing is manual.

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

## Wire protocol

All messages are space-delimited ASCII strings unless noted.

**Client → serverM (TCP 25218)**
```
CHECK <user>                    # balance query, Caesar mode
CHECK <user> XOR                # balance query, XOR mode
TX <from> <to> <amt>            # transfer, Caesar mode
TX <from> <to> <amt> XOR        # transfer, XOR mode
```

**serverM → backends (UDP)**
```
SHIFT <enc-user>                # balance query (Caesar-encrypted name)
XOR <enc-user>                  # balance query (XOR-encrypted name)
MAXID                           # get max serial id (response is 4-byte big-endian int32)
TXLIST SHIFT / TXLIST XOR       # fetch all transactions
SHIFT <serial> <from> <to> <amt>  # commit TX (Caesar-encrypted fields)
XOR   <serial> <from> <to> <amt>  # commit TX (XOR-encrypted fields)
```

**serverM → client replies**: `BALANCE <n>`, `ERROR_USER <name>`, `ERROR_USERS <u1> <u2>`, `INSUFFICIENT <n>`, `SUCCESS <n>`

## block*.txt format

`<serial-id> <caesar-from> <caesar-to> <plain-amount>` — one record per line. Names are always stored Caesar-shift-encrypted; amounts are plain integers. Both are stored this way regardless of whether the committing client used XOR mode.

## Balance baseline and ERROR_USER detection

Every user starts with a hardcoded balance of 1000. `serverM` sums the signed transaction deltas across all three shards and adds 1000. If the total is exactly 1000 (sum of deltas is zero, meaning no matching records exist), the user is treated as not found and `ERROR_USER` is returned. There is no separate user-existence lookup.

## Code structure note

There are no shared headers. `Tx`, `encrypt/decrypt`, and the XOR helpers are duplicated verbatim across `serverA.cpp`, `serverB.cpp`, `serverC.cpp`, and `serverM.cpp`. The three backend files differ only in their port number and shard filename.

## Non-obvious data flow

**Encryption layering:** Names and amounts travel on the wire encrypted, but `block*.txt` files always store data in Caesar-shift form regardless of client encryption mode. When a client uses XOR mode, `serverM` sends XOR-encrypted payloads to backends; backends XOR-decrypt then re-shift-encrypt before appending to their shard file.

**MAXID is binary:** The `MAXID` UDP response is a 4-byte big-endian `int32_t` (network byte order), unlike all other UDP responses which are plain text strings. `serverM` uses `ntohl` to decode it.

**txchain.txt is written by serverM, not monitor:** When monitor sends `TXLIST`, serverM collects all shards, merges and sorts by serial id, writes `txchain.txt` locally, then replies `TXLIST_OK`. The monitor only receives the confirmation string.

**commit_tx randomizes backend selection:** TX commits are sent to a randomly chosen backend (not always serverA) to spread load. If that backend is unresponsive, `commit_tx` fails over to the remaining ones. The serial id ensures the backend that eventually receives the retry is idempotent.
