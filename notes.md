**Key insight for interview:**
> "XOR with `0xFF` is self-inverse — encrypt and decrypt are the exact same function. The output lands above ASCII 127 so it is no longer readable text. However, a fixed key means a real attacker could break it — in production you would use a random key. This project uses it to demonstrate the concept of encrypting data in transit."



---

## Resume Bullets Mapped to Code

| Resume line | What it actually means |
|---|---|
| "Multi-service distributed system" | serverM + serverA/B/C, ledger sharded across three files |
| "TCP and UDP messaging protocols" | TCP for client sessions, UDP for coordinator→backend RPCs |
| "Structured transaction logging" | serial IDs, append-only `block*.txt`, globally sorted by `TXLIST` |
| "Encryption on transaction messages" | Caesar shift and XOR, encrypted on wire, Caesar stored on disk |
| *(extension)* mutex on UDP socket | enables true concurrent client handling |
| *(extension)* random backend selection | load balancing + fault tolerance in one design |

---

## Questions They Might Ask

**Why not use TCP for backends too?**
> UDP is faster and has no connection overhead. Since `serverM` and the backends are all on the same machine (localhost), packet loss is essentially zero. The retry logic in `udp_rpc()` adds reliability manually only when needed.

**What happens if all three backends are down?**
> `commit_tx()` tries all three and returns 0. `serverM` replies `INSUFFICIENT` to the client — not ideal but the system degrades gracefully rather than crashing.

**What happens if two clients transfer at the same time?**
> They both get threads. UDP queries are serialized by `udpMtx`. Serial IDs are assigned based on `MAXID` which is also queried under the mutex — so two concurrent transfers get different serial IDs and both commit safely.

**Why store data in Caesar-shift form on disk?**
> Consistency — the disk format never changes regardless of what wire encryption the client used. This means `TXLIST` always reads the same format and only needs to handle one decryption path.