Distributed Transaction System

---

## One Sentence Description

> "I built a distributed transaction system in C++ on Linux where a central coordinator server handles client requests over TCP and fans out to three backend servers over UDP to query and commit transactions across a sharded ledger."

---

## The Big Picture

```
client / monitor
      │
      │ TCP (reliable, connection-based)
      ▼
   serverM  ← coordinator, the brain of the system
      │
      │ UDP (lightweight, fire-and-forget)
      ├──────────────────────────────────┐
      ▼                ▼                 ▼
   serverA          serverB           serverC
  block1.txt       block2.txt        block3.txt
  (shard 1)        (shard 2)         (shard 3)
```

- The ledger is **sharded** — split across three backend servers
- Each backend owns one slice of the transaction history
- A user's balance = 1000 (starting) + sum of all their deltas across all three shards
- `serverM` is the only process that talks to both clients and backends

---

## Why TCP for clients, UDP for backends

**TCP (clients ↔ serverM)**
- Client needs to know if their transfer succeeded or failed
- TCP guarantees delivery and ordering — a simple blocking `recv()` is all the client needs
- Connection-based — `connect()` on client, `accept()` on serverM

**UDP (serverM ↔ backends)**
- Backend calls are internal, high-frequency, and latency-sensitive
- UDP is connectionless and lightweight — no handshake overhead
- Reliability is added manually on top with timeouts and retries

---

## Extension 1 — Mutex on the Shared UDP Socket (Concurrency)

> "One thing I extended was adding a mutex to serialize UDP access, which allows multiple clients to be served simultaneously."

### The problem

`serverM` handles each TCP client in a detached `std::thread`. All those threads share **one** UDP socket. Without protection:

```
thread1 (Alice):  sendto serverA "SHIFT Dolfh"
thread2 (Bob):    sendto serverA "SHIFT Ere"      ← two sends flying at once
thread1:          recvfrom ← picks up Bob's reply  ← WRONG answer for Alice
thread2:          recvfrom ← picks up Alice's reply ← WRONG answer for Bob
```

### The fix — `udpMtx`

```cpp
std::mutex udpMtx;

// inside query_balance():
std::lock_guard<std::mutex> lk(udpMtx);   // only one thread at a time
int n = udp_rpc(request, port, buffer);    // send + recv back-to-back
// mutex auto-released when lk goes out of scope
```

### What this enables

```
thread1 (Alice):  lock udpMtx → GOT IT
                  sendto serverA, recvfrom ← Alice's reply ✓
                  release mutex
thread2 (Bob):    lock udpMtx → UNBLOCKED
                  sendto serverA, recvfrom ← Bob's reply ✓
```

Many clients can connect simultaneously. Their threads run in parallel for everything except the UDP coordination step, which is serialized by the mutex.

---

## Extension 2 — Random Backend Selection (Load Balancing)

> "I also implemented random backend selection for transaction commits, which acts like a simple load balancer so one server does not get all the writes."

### The problem without randomization

Without randomization every single transaction always goes to serverA first:

```
TX1 → serverA  ← all the work
TX2 → serverA
TX3 → serverA
serverB, serverC sit idle
```

### The fix — rotate the attempt order randomly

```cpp
int order[3] = {0, 1, 2};
int start = std::rand() % 3;         // random starting point: 0, 1, or 2
std::rotate(order, order+start, order+3);  // shift the array

// start=0 → try A, B, C
// start=1 → try B, C, A
// start=2 → try C, A, B
```

### The result

```
TX1 → serverB first  ← spread evenly
TX2 → serverC first
TX3 → serverA first
TX4 → serverB first
```

This is the same idea behind a **load balancer** — distribute work evenly across nodes so no single node becomes a bottleneck.

### Built-in failover

If the randomly chosen backend is down, the code falls over to the next one:

```cpp
for (int i : order) {
    bool ok = send_tx_locked(ports[i], payload);
    if (ok) return ids[i];       // success
    tried.insert(ids[i]);        // failed — mark it, try next
}
```

So random selection gives load balancing, and the loop gives fault tolerance — two benefits from one design.

---

## Fault Tolerance 

> "The system handles retries safely using serial IDs to prevent duplicate transactions."

When `serverM` retries a commit after a timeout, the same transaction might arrive at a backend twice. `seen_ids` prevents double-writing:

```cpp
// serverA.cpp
if (seen_ids.count(serial)) {
    sendto(..., "LOG_OK", ...);   // already have it, just ack again
} else {
    // write to disk first, then ack
    fout << serial << sender << receiver << amt;
    seen_ids.insert(serial);
    sendto(..., "LOG_OK", ...);
}
```

The serial ID is always `max(all backends) + 1` — globally unique across the whole system.

---

## Encryption

Two modes, selectable per request:

| Mode | How | Output | Weakness |
|---|---|---|---|
| Caesar shift | rotate letters +3, digits +3 | still readable text | trivially broken by eye |
| XOR `0xFF` | flip every bit | binary garbage, not printable | fixed key, breakable with known plaintext |

The fix for fixed key:
```cpp
  std::string generate_xor_key(size_t length) {
      std::string key(length, 0);

      // read random bytes from the OS
      std::ifstream urandom("/dev/urandom", std::ios::binary);
      urandom.read(&key[0], length);

      return key;
  }
```
How to run:
  # Clone or copy the project, then build
  make

  # Open 5 terminals (or use & to background):
  ./serverA &
  ./serverB &
  ./serverC &
  ./serverM &

  # Test balance check
  ./client Alice

  # Test transfer
  ./client Alice Bob 100

  # Test XOR encryption mode
  ./client Alice XOR
  ./client Alice Bob 100 XOR

  # Test monitor (creates txchain.txt)
  ./monitor TXLIST

## Socket Diagram

```
                    CLIENT PROCESS
                   ┌─────────────┐
                   │  sock (fd3) │
                   │  TCP        │
                   └──────┬──────┘
                          │ connect to 25218
                          │
          ┌───────────────▼────────────────────────────┐
          │              serverM PROCESS               │
          │                                            │
          │  clientListen (fd3)   monitorListen (fd4)  │
          │  TCP port 25218       TCP port 26218        │
          │         │                    │             │
          │    accept()             accept()           │
          │         │                    │             │
          │  clientSock (fd6)     monSock (fd7)        │
          │  TCP (per client)     TCP (per monitor)    │
          │                                            │
          │         udpSock (fd5)                      │
          │         UDP port 24218                     │
          │         (send + receive)                   │
          └───────┬──────────┬──────────┬──────────────┘
                  │          │          │
          send to │  send to │  send to │
          port    │  port    │  port    │
          21218   │  22218   │  23218   │
                  │          │          │
     ┌────────────▼─┐ ┌──────▼───────┐ ┌▼─────────────┐
     │   serverA    │ │   serverB    │ │   serverC    │
     │  sockfd(fd3) │ │  sockfd(fd3) │ │  sockfd(fd3) │
     │  UDP 21218   │ │  UDP 22218   │ │  UDP 23218   │
     └──────────────┘ └──────────────┘ └──────────────┘

                   MONITOR PROCESS
                   ┌─────────────┐
                   │  sock (fd3) │
                   │  TCP        │
                   └──────┬──────┘
                          │ connect to 26218
```

Each process has its own fd table so fd3 in client, fd3 in serverA, fd3 in monitor are all independent.
