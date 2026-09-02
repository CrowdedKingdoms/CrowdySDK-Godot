# Benchmarks

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # Release, or the numbers mean nothing
cmake --build build -j
./build/benchmarks/bench_codec                   # encode / parse / verify, ns per op
./build/benchmarks/bench_send                    # the send path taken apart
```

`bench_e2e_latency` measures echo round-trip against a deployment and needs the
`CROWDY_E2E_*` variables; it is not part of the offline set.

## What bench_send is for

`bench_codec` reports encode+sign as a single number, which is enough to see a
regression and not enough to act on one. `bench_send` splits the per-datagram
cost into encode, MAC and socket write, and prices the alternatives for each
side by side, so an optimisation can be aimed at the part that costs something.

It also cross-checks every MAC variant against the one-shot result before
timing any of them. A faster MAC that disagrees with the one on the wire is not
a faster MAC.

## Results, 0.24.0

Recorded on the CKS builder: AWS `b3` class, OpenSSL 3.0.13, CPU SHA extensions
present, no KPTI/Meltdown mitigations. Absolute numbers move with hardware —
the ratios are the part that travels, and the ratios are what the change was
judged on. A machine without hardware SHA will be several times slower on every
MAC line, and one with syscall mitigations enabled will be slower on the writes.

### The change in 0.24.0: a pre-keyed MAC

| | one-shot (0.23.0) | pre-keyed (0.24.0) | |
|---|---|---|---|
| encode + sign, 88-byte payload | 1225 ns | **319 ns** | 3.84x |
| verify a notification | 1276 ns | **337 ns** | 3.79x |
| 200-entity frame, encode+sign+write | 2922 ns/entity | **1824 ns/entity** | 1.60x |
| 200-entity frame through `Connection::sendActorUpdate` | — | **1912 ns/entity** | |

Encoding itself is 4.2 ns. Before the change the MAC was essentially 100% of
`encode+sign`, because the one-shot `HMAC()` rebuilt a context and re-imported
the 64-byte key for every datagram while the key changes only on token refresh.

The last row is the number a game pays: the public path adds about 90 ns of
locking and counter bookkeeping over the raw encode-and-write, which is why the
send-side mutexes were left alone.

### Why the MAC is implemented the way it is

| variant | ns | |
|---|---|---|
| one-shot `HMAC()` | 1236 | what 0.23.0 did |
| pre-keyed `EVP_MAC` + `EVP_MAC_CTX_dup` per call | 574 | 2.15x |
| the same, fed in two parts instead of concatenated | 595 | no measurable difference |
| pre-keyed, duplicated once per thread then reset | 313 | 3.95x, and what ships |
| legacy `HMAC_CTX` reset | 260 | 4.76x, but deprecated in OpenSSL 3 |

Duplicating the keyed context on every call throws away half the benefit, so
each thread duplicates once and then resets. Feeding the prefix and token
separately rather than concatenating them saves no measurable time; it is done
anyway because it removes a 1296-byte stack buffer from a call a game makes
hundreds of times a frame.

### Why there is no sendmmsg

Batching writes was the other obvious lever and it was measured before being
built. It is not worth it:

| | ns per datagram |
|---|---|
| `send()` per datagram, loopback | 1446 |
| `sendmmsg` batch of 8 / 16 / 32 / 64 | 1390 / 1430 / 1445 / 1440 |
| `send()` per datagram, recipient discarded before delivery | 758 |
| `sendmmsg` batch of 64, same destination | 732 |

Between 1.00x and 1.04x. The second pair removes loopback delivery from the
measurement, so what is left is the syscall and the route lookup — and batching
still gains only about 3%, because the kernel does the same per-datagram work
either way and only the syscall boundary is amortised. That boundary is a small
part of the cost on this machine, which has no KPTI. It would be worth more on
a machine that does, so this is a decision to revisit with a measurement rather
than a permanent no; a public batch API was not worth adding for 3%.
