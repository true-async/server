# moodycamel queues — bundled dependency

Vendored header-only lock-free queues by Cameron Desrochers, selected by the
micro-benchmark in issue #81. Used to build the inter-thread message primitives
in `src/core/thread_queue.cc` (C-ABI) and `src/core/thread_mailbox.c` (reactor
wakeup).

| File | Upstream | Role |
|---|---|---|
| `concurrentqueue.h`   | https://github.com/cameron314/concurrentqueue   | MPSC (used as multi-producer, single-consumer) |
| `readerwriterqueue.h` | https://github.com/cameron314/readerwriterqueue | SPSC fast lane |
| `atomicops.h`         | https://github.com/cameron314/readerwriterqueue | atomics + semaphore helpers required by `readerwriterqueue.h` |

| Field | Value |
|---|---|
| Retrieved | 2026-06-04 (latest `master` of each repo) |
| License   | Simplified BSD / Boost (dual) — see `LICENSE.md` |

## Why these two

Per the #81 benchmark (`~/qbench/`, i7-11700K, payload = pointer, cap = 4096):

- **MPSC → ConcurrentQueue.** The only ready-made queue that holds throughput as
  producers contend (56–68 M ops/s at P=1..8). Every bounded array + CAS-on-shared-index
  design (rigtorp MPMCQueue, `ck_ring` MPMC) collapses ~30× under producer contention
  (2.3 M ops/s at P=8). moodycamel sidesteps it with per-producer sub-queues.
- **SPSC → ReaderWriterQueue.** rigtorp SPSCQueue is marginally faster but its repo is
  frozen (~2.5 years). RWQ is from the same author, actively maintained, and within range.

Both are used in bounded mode (`try_enqueue`, no growth) behind an explicit length cap
maintained in `thread_queue.cc` — see that file.

## Updating

Header-only: drop in the new `concurrentqueue.h` / `readerwriterqueue.h` / `atomicops.h`
from upstream, review the diff, and bump the "Retrieved" date above.
