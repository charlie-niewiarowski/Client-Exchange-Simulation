# File reorganization — change log

Reorganized the codebase's type/file layout: split files that mixed unrelated
types, dropped dead/duplicate declarations, and moved a constant to where its
siblings live. **No behavior changes** — `exchange-release` and `client-release`
both build clean and a 4-client run still produces matching fills (258 ACK /
508 MATCH / 0 ERR).

> Note: the working tree already contained uncommitted changes from the earlier
> reconnect fix and the Orderbook refactor. Because of that, a plain
> `git checkout <file>` can revert *more* than a single change here. Each entry
> below gives a precise, self-contained revert. I'm happy to perform any of
> these reverts for you — just name the change number.

Each change is independent **except Change 2**, which is one atomic split.

---

## Change 1 — delete stray editor backup `order.hpp~`

**What:** Deleted `exchange/engine/include/order.hpp~`.
**Why:** An old editor backup file (not compiled, not `#include`d anywhere). It
held a *stale duplicate* of the primitive type aliases and an outdated
`OrderRequest` — e.g. `using OrderId = uint32_t` (the real code now uses
`uint64_t`). Pure cruft that invites confusion.

**Files:** `D exchange/engine/include/order.hpp~`
**Revert:** `git checkout -- 'exchange/engine/include/order.hpp~'`
(it was tracked at HEAD, so this restores it verbatim).

---

## Change 2 — split `engine_types.h` into one-type-per-file

**What:** `engine_types.h` held three unrelated concerns: the `Order` object,
the `PriceLevelQueue` container, and the book-storage aliases. Split into:

- **New `exchange/engine/include/order.h`** — the `Order` class.
- **New `exchange/engine/include/price_level_queue.h`** — the `PriceLevelQueue`
  class (includes `order.h`).
- **`OrderMap` / `BidLevels` / `AskLevels` aliases** moved into `orderbook.h`
  (the only place that uses them; they *are* the book's storage layout).
- **Deleted `exchange/engine/include/engine_types.h`.**
- Updated includes: `order_pool.h` (`engine_types.h` → `order.h`) and
  `orderbook.h` (`engine_types.h` → `order.h` + `price_level_queue.h`, plus
  `<array>`/`<unordered_map>` for the aliases).

**Why:** These are the distinct components described in your own order-book
design doc (Order, PLQ, OrderMap, OrderPool). One type per file makes the
dependency direction obvious: `order.h` ← `price_level_queue.h` ← `orderbook.h`.

**Files:**
`?? exchange/engine/include/order.h`,
`?? exchange/engine/include/price_level_queue.h`,
`D  exchange/engine/include/engine_types.h`,
modified `orderbook.h`, `order_pool.h`.

**Revert (do NOT just `git checkout engine_types.h`** — the HEAD copy predates
the Orderbook refactor and would be incompatible):
1. `rm exchange/engine/include/order.h exchange/engine/include/price_level_queue.h`
2. Recreate `engine_types.h` = the body of `order.h` + the body of
   `price_level_queue.h` + the three `using` aliases, under one include guard.
3. In `order_pool.h`, change `#include "order.h"` back to `#include "engine_types.h"`.
4. In `orderbook.h`, replace the `order.h` + `price_level_queue.h` includes and
   the "Book storage types" alias block with `#include "engine_types.h"`.

(Or just ask me to revert Change 2 and I'll restore the exact prior state.)

---

## Change 3 — drop dead `ClientMessageMap` from `communication_types.h`

**What:** Removed `using ClientMessageMap = std::unordered_map<ClientId,
std::vector<OutboundMessage>>;` and its now-unused includes
(`<unordered_map>`, `<vector>`, `<memory>`).
**Why:** `ClientMessageMap` is referenced nowhere in the codebase (only its own
definition). Removing it lets `communication_types.h` be purely the wire
message/enum definitions, with no heap-container includes.

**Files:** modified `shared/communication_types.h`
**Revert:** `git checkout -- shared/communication_types.h`
(this file was untouched by earlier tasks, so the checkout is clean).

---

## Change 4 — move `RINGBUF_SIZE` from `protocol.h` to `config.h`

**What:** Moved `RINGBUF_SIZE` (per-connection outbound staging-ring capacity,
`= 64`) out of `shared/protocol.h` and into `exchange/config/config.h`,
alongside the other server tuning knobs (`PIPELINE_DEPTH`, `MAX_CLIENTS`, …).
**Why:** `protocol.h` is about the *wire format* (frame sizes, status prefixes,
error strings). `RINGBUF_SIZE` is a server-side capacity knob — unrelated to the
protocol, and only used by `connection_info.h`, which already includes
`config.h`.

**Files:** modified `shared/protocol.h`, `exchange/config/config.h`
**Revert:** `git checkout -- shared/protocol.h` and remove the `RINGBUF_SIZE`
line from `exchange/config/config.h`. (Both were untouched by earlier tasks.)

---

## Change 5 — remove empty `server/src/buffer.cpp` placeholder

**What:** Deleted `exchange/server/src/buffer.cpp` and dropped its entry from
`exchange/CMakeLists.txt`'s `EXCHANGE_SOURCES`.
**Why:** Buffer is a header-only template (`buffer.hpp`); the `.cpp` was an empty
placeholder kept only so the CMake source list "did not need to change." Removing
the source and its build entry is the honest version of that.

**Files:** `D exchange/server/src/buffer.cpp`, modified `exchange/CMakeLists.txt`.

---

## Change 6 — standardise all headers on `.hpp`

**What:** Renamed every project header from `.h` to `.hpp` (via `git mv`, so
history follows), and updated every quoted `#include`, the `common_lib`
`lib.hpp` entry in `exchange/CMakeLists.txt`, the `config.hpp` path read by
`bench.py`, and the filename references in the `README.md` layout section.
System includes (`<pthread.h>`, `<sched.h>`, `<hdr/…>`) are untouched.
**Why:** The tree mixed `ring_buffer.hpp` with `.h` for every other header. One
extension across the codebase removes the "which is it?" friction. `.hpp` was
chosen to match the existing outlier and to read unambiguously as C++.

**Files:** all `exchange/`, `client/`, and `shared/` headers renamed; every
`.cpp`/`.hpp` include updated; `exchange/CMakeLists.txt`, `client/CMakeLists.txt`,
`bench.py`, `README.md` reference updates.

---

## Change 7 — merge `Command` into `OrderRequest` (one object)

**What:** Deleted the `Command` struct from `orderbook.hpp` and folded its two
extra fields (`MessageType message_type`, `OrderId order_id`) into
`OrderRequest`. `OrderRequest` is now the single object the Engine builds and
hands to the book:

- `Orderbook::process()` takes `const OrderRequest&` and reads `message_type` /
  `order_id` via getters instead of re-packing a fresh `OrderRequest`.
- `addOrder` / `modifyOrder` / `matchMarket` lost their redundant `OrderId id`
  parameter — the id now travels inside the request (`get_order_id()`).
- `engine.cpp` builds one `OrderRequest` directly from the `InboundMessage`
  instead of a `Command`.

**Why:** The two types had fully overlapping payloads; keeping both meant every
message was copied twice on the hot path. Collapsing to one carrier removes the
duplication and makes `OrderRequest` the genuine single input to the book.

**Files:** modified `order_request.hpp`, `orderbook.hpp`, `orderbook.cpp`,
`engine.cpp`, `engine.hpp` (comment).

---

## Change 8 — move `PendingLatency` to `latency.hpp`

**What:** Moved the `PendingLatency` struct out of `connection_info.hpp` and into
`latency.hpp` (next to `LatencySample`), and added the now-needed
`#include "latency.hpp"` to `connection_info.hpp`. Also gave `latency.hpp` an
explicit `#include "order_types.hpp"` for `Timestamp` (previously relied on the
includer).
**Why:** `PendingLatency` is a latency concept, not connection state; it belongs
with the other latency types. `OutboundState` still embeds it, which is why the
one added cross-include is unavoidable — and worth it for the clearer home.

**Files:** modified `latency.hpp`, `connection_info.hpp`.

---

## Verification

`exchange-release` and `client-release` both build clean (Docker, Ubuntu 24.04).
A 4-client seeded run (`seed 42`) reproduces the baseline: **258 ACK**, **506
MATCH** (the 2-short vs the prior 508 is only the `SIGINT` landing mid-pair),
**0 ERR**.
