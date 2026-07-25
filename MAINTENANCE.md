# Maintenance Guide

How code in this repository is written, organized, and reviewed. This is a
conventions document, not a tutorial — it records what the code *already* does
where it is consistent, and flags where it is not (see
[Known inconsistencies](#known-inconsistencies)).

Companion documents:

- `README.md` — what the project is, how to build and run it, config reference.
- `REORGANIZATION.md` — a historical change log for the file-layout refactor.
- `bench/README.md` — the Python benchmark suite.

---

## 1. Guiding principles

The project is a latency benchmark first and a piece of software second. That
ordering drives most of the rules below.

1. **The hot path is sacred.** Anything on the request path — recv → validate →
   ring → match → ring → send — does no allocation, no locking, no syscalls
   beyond the unavoidable ones, and no I/O. Everything it needs is preallocated
   at startup.
2. **Cost must be visible.** If a construct hides an allocation, a copy, or a
   branch, prefer the explicit version. Struct layouts carry byte-offset
   comments precisely so the cost stays visible during review.
3. **Ownership is stated, not inferred.** Every non-owning pointer, reference
   member, and cross-thread handoff says in a comment who owns the storage and
   what must outlive what.
4. **Concurrency is documented at the point of use.** Every atomic carries its
   memory order explicitly, and every multi-step protocol (the two-phase close
   in `server.hpp`, the SPSC ring in `ring_buffer.hpp`) is described in a
   comment block next to the state it governs.
5. **Configuration is compile-time.** There is no runtime config file and no
   getopt-style flag soup. Knobs are macros in a `config.hpp`; changing one is
   a rebuild.
6. **Optional work is compiled out.** Diagnostics, logging, and test accessors
   live behind `#if` so a release build carries none of their cost.

---

## 2. Repository layout

```
exchange/       Exchange server
  app/          Entry point (main.cpp) + top-level Exchange class
  engine/       Matching engine
    include/    Headers, one primary type per file
    src/        Definitions for non-inline, non-template members
    tests/      Unit tests + the shared test_runner.hpp
  server/       TCP gateway (same include/src/tests split)
  include/      Exchange-internal shared code (ring_buffer.hpp, lib.hpp)
  config/       config.hpp — every exchange compile-time knob
client/         Load generator (app/, include/, src/, config/, same rules)
infra/          Types shared by BOTH programs
protocol/       Wire protocol definitions
bench/          Python benchmark suite
```

Rules:

- **A header lives in the narrowest scope that needs it.** Only put something in
  `infra/` or `protocol/` when both the exchange and the client include it.
  `exchange/include/` is for code shared *within* the exchange.
- **One primary type per header,** named after that type in `snake_case`:
  `Order` → `order.hpp`, `PriceLevelQueue` → `price_level_queue.hpp`. Small
  satellite types that are meaningless on their own may share the file (e.g.
  `ProcessResult` sits in `orderbook.hpp` because it is `Orderbook::process`'s
  return type).
- **Type aliases live where the storage lives,** not in a grab-bag header.
  `OrderMap` / `BidLevels` / `AskLevels` are defined in `orderbook.hpp` because
  they *are* the book's storage layout.
- **Headers are `.hpp`.** (`.h` was standardized away in `056c69e`.)
- **Dependency direction must stay obvious** from the include graph:
  `order.hpp` ← `price_level_queue.hpp` ← `orderbook.hpp` ← `engine.hpp`. Never
  add an include that points back up that chain.

---

## 3. File structure

### 3.1 Banner comment

Every file opens with a comment explaining **what the type is and how it fits
into the system** — not just its name. This is the single highest-value
convention in the codebase; `orderbook.hpp` is the reference example:

```cpp
//
// Orderbook — an autonomous price-time-priority order book.
//
// The Orderbook owns the full book state (the OrderPool, the per-price
// PriceLevelQueues, and the OrderId -> Order* lookup map) and all of the
// mutating logic: adding, cancelling, modifying, and matching orders.
//
// It is driven exclusively through process(OrderRequest): the Engine translates
// each inbound wire message into an OrderRequest and hands it over. ...
//
```

The banner should answer: what is this, who owns it, who drives it, and what
comes out. IDE-generated `// Created by <user> on <date>.` stubs are legacy —
replace them with a real banner whenever you touch such a file.

### 3.2 Include guards

`#ifndef NAME_H` / `#define NAME_H` / `#endif // NAME_H`. Guard names derive
from the file: `orderbook.hpp` → `ORDERBOOK_H`. `#pragma once` is not used.

The closing `#endif` carries the guard name as a comment.

### 3.3 Include order

Project headers first (quoted, roughly dependency order), then a blank line,
then standard and system headers (angled, alphabetical):

```cpp
#include "order.hpp"
#include "price_level_queue.hpp"
#include "order_types.hpp"
#include "config.hpp"

#include <array>
#include <unordered_map>
```

In `.cpp` files the corresponding header comes first, on its own.

### 3.4 Section banners

Two levels, both already used throughout:

```cpp
//=============================================================================
// Section name
//=============================================================================
//
// Optional prose explaining the section.
```

for file-level sections, and

```cpp
    //===== data containers + facilitating members ======
```

for grouping members inside a class body. Use them once a file or class body
exceeds roughly a screen; do not decorate short files.

---

## 4. Naming

| Kind | Convention | Example |
|---|---|---|
| Types (class, struct, enum class, alias) | `PascalCase` | `Orderbook`, `InboundMessage`, `OrderId` |
| Enumerators | `SCREAMING_CASE` (engine) / `PascalCase` (OUCH) | `Side::BID`, `ouch::InboundType::EnterOrder` |
| Free functions | `snake_case` | `validate_message`, `pin_to_core`, `now_ns` |
| Public member functions | `snake_case` | `is_filled()`, `pop_trade()`, `order_count()` |
| Private data members | `snake_case_` (trailing underscore) | `best_bid_price_`, `out_ring_` |
| Local variables / parameters | `snake_case` | `order_request`, `next_write` |
| Macros | `SCREAMING_CASE` | `MAX_PRICE`, `PIPELINE_DEPTH` |
| `constexpr` constants | `SCREAMING_CASE` | `INBOUND_BSIZE`, `ERROR_CHANNEL_SIZE` |
| Namespaces | `lowercase` | `ouch` |
| Files | `snake_case.hpp` / `.cpp` | `price_level_queue.hpp` |

Private *member functions* are currently mixed (`addOrder` vs
`update_best_bid`) — see [Known inconsistencies](#known-inconsistencies).

Numeric literals use digit separators past four digits: `1'000'000`,
`524'288`, `100'000'000`.

---

## 5. C++ style

Target is **C++23** (`set(CMAKE_CXX_STANDARD 23)` in all three `CMakeLists.txt`).

### 5.1 Formatting

- 4-space indent, no tabs.
- Opening brace on the same line, for everything including functions.
- `public:` / `private:` at class-body indentation (not indented).
- Pointer/reference binds to the type in declarations (`OutboundRing& out_ring_`,
  `Order *prev_`) — currently mixed; prefer binding to the type.
- Single-statement early returns may be one-liners: `if (x) return false;`
- **Vertical alignment is used deliberately** to make related declarations
  scan as a table, and is worth preserving when you edit around it:

  ```cpp
  Orderbook(const Orderbook&)            = delete;
  Orderbook& operator=(const Orderbook&) = delete;
  Orderbook(Orderbook&&)                 = delete;
  Orderbook& operator=(Orderbook&&)      = delete;
  ```

- Trailing comments align in columns when several appear in a row (see the
  `Order` member block).

### 5.2 Classes

- **Special members are explicit.** Any type that owns resources or is
  referenced across threads deletes copy *and* move explicitly and declares its
  destructor, even when defaulted (`Orderbook`, `Server`).
- **Single-argument constructors are `explicit`** (`explicit Orderbook(OutboundRing&)`,
  `explicit RingBuffer(uint32_t N)`).
- **Getters are `[[nodiscard]]`** and `const`.
- **Value parameters that are not modified are `const`**: `void fill(const Quantity quantity)`,
  `inline void pin_to_core(const int core)`.
- **Non-mutating members are `const`.**
- `friend` is used sparingly and only for the owner of the type
  (`friend class Orderbook;` in `Order`).

### 5.3 Data layout

Hot structs document their layout inline and are sized to a cache-line
boundary. Preserve these comments when you add or reorder a field — an
un-updated offset comment is worse than none:

```cpp
private:
    Timestamp recv_tsc;           // 8 bytes  @ 0
    OrderId id_;                  // 8 bytes  @ 8/24
    Order *prev_{nullptr};        // 8 bytes  @ 16/32
    ...
    uint8_t padding[7];           // 6 bytes  @ 42/58
}; // 48/64 bytes (sizeof verified by compiler)
```

The `X/Y` offsets are the `DIAGNOSTICS=0` and `DIAGNOSTICS=1` layouts. Adding a
field under `#if DIAGNOSTICS` shifts everything after it — always re-derive both
columns.

Cross-thread structures pad each independently-written field to its own cache
line with `alignas(64)` (`RingBuffer`'s `write_idx_` / `read_idx_`).

Members are initialized with brace default-member-initializers
(`Price best_bid_price_{MIN_PRICE};`, `int epoll_in_ = -1;`) rather than in a
constructor body, wherever the value is a constant.

### 5.4 Concurrency

- **Every atomic operation names its memory order.** Never rely on the
  `seq_cst` default. The established pattern is relaxed load of your own index,
  acquire load of the other side's, release store of your own:

  ```cpp
  size_t write_idx = write_idx_.load(std::memory_order_relaxed);
  if (next_write == read_idx_.load(std::memory_order_acquire)) return false;
  buffer[write_idx] = item;
  write_idx_.store(next_write, std::memory_order_release);
  ```

- **Ring buffers are SPSC and power-of-two sized.** Capacity is masked, not
  modulo'd, and the constructor asserts the power-of-two invariant. One
  producer thread and one consumer thread — document which is which at the
  declaration site.
- **Multi-step teardown protocols get a comment block** naming which thread
  sets each flag, which reads it, with what ordering, and what invariant the
  sequence guarantees. `Server::condemned_inbound_` / `condemned_outbound_` is
  the model.
- Threads are `std::jthread` members; cores are pinned via `pin_to_core()` with
  the core number coming from `config.hpp`.

### 5.5 Error handling

- **Validation returns `bool`,** it does not throw. `validation.hpp` holds pure
  predicates, deliberately separated from `server.cpp` so they are unit-testable
  without a socket.
- **Wire errors are data,** carried as `ServerError` enumerators and mapped to
  strings by a `constexpr` switch with a `default` arm.
- **`std::exception` is not used on the hot path.** The one exception
  (`Order::fill` throwing `std::logic_error`) is flagged below.
- Syscall failures on the setup path log to `std::cerr` with `strerror(errno)`
  and return `false` / `std::nullopt`; the caller decides whether that is fatal.
- `std::optional` expresses "may not exist" for setup results and per-fd slots
  (`std::array<std::optional<InboundState>, N>`).

### 5.6 Templates

Templates are declared in the class body and **defined below it in the same
header**, not inline in the body (`RingBuffer`). Concrete instantiations get
named aliases at the bottom of the file:

```cpp
using InboundRing  = RingBuffer<InboundMessage>;
using OutboundRing = RingBuffer<OutboundMessage>;
```

Prefer `if constexpr` over runtime branches for config-driven behavior
(`if constexpr (EXPECTED_THROUGHPUT > 0)` in `LoadGenerator`).

---

## 6. Configuration and feature flags

### 6.1 The two-tier config file

Each program has exactly one `config/config.hpp`, split into two labeled
sections:

```cpp
//=============================================================================
// user-level macros (feel free to tinker around)
//=============================================================================
...
//=============================================================================
// development macros (DO NOT TOUCH UNLESS FOR A REASON)
//=============================================================================
```

**User-level** knobs are safe to change without reading the code: market
parameters, host/port, core assignments, price bounds. **Development** knobs
have invariants attached (power-of-two ring sizes, pipeline depth matched
against the client, preallocation counts).

When you add a knob:

1. Put it in the correct section.
2. Give it a comment stating units and any invariant
   (`// per-connection outbound staging ring capacity (must be a power of two)`).
3. **Add a row to the config table in `README.md`.** These tables are the user
   documentation; a knob missing from them is undiscoverable.
4. If it must match a value on the other side of the wire (`PIPELINE_DEPTH`
   exists in both configs), say so in both comments.

### 6.2 Feature flags

`LOGGING`, `DIAGNOSTICS`, and `TESTING` are 0/1 macros guarding `#if` blocks.

- `DIAGNOSTICS` adds per-stage TSC timestamps — **it changes struct layout and
  wire timing**, so it is never on for a benchmark run.
- `LOGGING` adds the trade ring and stdout printing.
- `TESTING` exposes inspection accessors (`order_count()`, `has_order()`) and
  synchronous `Engine::step()`.

Rules: keep guarded blocks small and at the *edge* of a type (extra members,
extra accessors) rather than threaded through logic. A file must compile with
every flag both on and off — **build all four release/debug targets with each
flag flipped before merging a change that touches guarded code.**

---

## 7. Tests

### 7.1 Framework

Hand-rolled, in `tests/test_runner.hpp` (one copy per module). No GoogleTest,
no external dependency. Assertions are `do/while(0)` macros that print
`FAIL [file:line]` with the stringified expression and `return false`:

`ASSERT_TRUE`, `ASSERT_FALSE`, `ASSERT_EQ`, `ASSERT_NE`, `ASSERT_STREQ`.

Because assertions `return false`, **every test function is
`static bool name()` and ends with `return true;`.**

### 7.2 Structure of a test file

1. Banner explaining the scope and *why it is fast/isolated* ("no networking —
   pure logic, runs instantly"; "tests are single-threaded ... no threads are
   spawned").
2. `static` `make_*` helpers that build wire messages positionally.
3. A fixture `struct` owning fresh rings + the system under test, with helpers
   like `submit()` and `drain()`. **Each test constructs its own fixture — no
   shared state between tests.**
4. Tests grouped under section banners by behavior (`NEW order — book
   placement`, `Validate New`).
5. A registration list mapping names to functions.

### 7.3 What to test

- Pure logic first: validation predicates, book state transitions, matching
  outcomes.
- Assert on *observable* state via the `#if TESTING` accessors
  (`order_count()`, `bids_at(price)`, `bid_level_count()`) and on drained
  outbound messages — never on private internals directly.
- Cover the boundary conditions the validators encode: zero price, quantity
  ceiling, invalid enum values, wrong message type.
- New behavior in the engine or validators ships with tests in the same commit.

---

## 8. Build

- Three `CMakeLists.txt`: root aggregates, `exchange/` and `client/` define
  targets. C++23 set in each.
- Every program has a **release** and a **debug** target:
  - release: `-O3 -march=native` (exchange adds `-g -fno-omit-frame-pointer`
    for perf profiling)
  - debug: `-fsanitize=address,undefined`, `-g3`
- **Run the debug target under ASan/UBSan after any change to memory layout,
  the ring buffers, the connection lifecycle, or the pool.** This is the main
  safety net the project has.
- Third-party code comes in via `FetchContent` pinned to a tag
  (`HdrHistogram_c` at `0.11.9`). Pin versions; never track a branch.
- Include directories are listed per target with a trailing comment naming what
  each one provides:

  ```cmake
  target_include_directories(client-release PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.hpp, client_state.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.hpp
      ${CMAKE_CURRENT_SOURCE_DIR}/../infra  # protocol headers
  )
  ```

- Adding a `.cpp` means adding it to `EXCHANGE_SOURCES` (exchange) or the
  `add_executable` lists (client — note both release and debug lists must be
  updated).

---

## 9. Documentation

- **`README.md` is the contract with the user.** Repository layout, build/run
  instructions, and both config tables must be updated in the same commit as
  the change they describe.
- Portability caveats stay explicit and up front (`__rdtsc`, `-march=native`,
  x86-64 only, Rosetta caveat). If you add a platform dependency, say so there.
- Performance numbers in the README carry the commit they were measured at.
- Comments explain **why**, not what. The good ones in this codebase state an
  invariant (`N must be a power of two because of mask`), name an owner
  (`owns every Order; must outlive orders_/bids_/asks_ pointers`), or record a
  unit (`Starting mid-price in integer ticks`).

---

## 10. Git

- Work on a topic branch named `<kind>/<subject>`: `refactor/OUCH-protocol`.
- Commits are short and describe the change, not the diff
  (`Rename shared/ to infra/`, `Add Python benchmark suite (bench/)`).
- Never commit build output. `.gitignore` covers `build`, `client/build`,
  `exchange/build`, `bench/results`, `bench/logs`, `.idea`.
- Delete dead files rather than leaving them; editor backups (`*~`) and stale
  duplicates are cruft that actively misleads (see `REORGANIZATION.md`, Change 1).
- Large structural refactors get a change-log document with per-change revert
  instructions — the `REORGANIZATION.md` pattern.

---

## 11. Review checklist

Before merging:

- [ ] Builds clean: all four targets (`exchange-release`, `exchange-debug`,
      `client-release`, `client-debug`).
- [ ] Debug target run under ASan/UBSan if memory layout, rings, pool, or
      connection lifecycle changed.
- [ ] Engine and validation tests pass; new behavior has new tests.
- [ ] Compiles with `LOGGING`, `DIAGNOSTICS`, `TESTING` both on and off.
- [ ] No allocation, locking, or I/O added to the hot path.
- [ ] Struct offset comments and `sizeof` totals re-derived if a member moved.
- [ ] Every new atomic access names its memory order.
- [ ] New config macro is in the right section, commented with units and
      invariants, and added to the README table.
- [ ] New header has a real banner, a guard, correct include order, and lives
      in the narrowest scope that needs it.
- [ ] A functional smoke run still produces matching fills (the
      `REORGANIZATION.md` standard: N-client run, ACK/MATCH/0 ERR).

---

## Known inconsistencies

Open items — decisions we should make rather than defaults to copy.

1. **Private member function casing.** `Orderbook` and `Server` use
   `camelCase` for private methods (`addOrder`, `matchOrders`, `setupEpoll`,
   `handleRequests`) while public members and free functions use `snake_case` —
   and `Orderbook::update_best_bid` / `update_best_ask` are `snake_case` inside
   the same class. Needs one rule.
2. **Guard names say `_H`, files are `.hpp`.** `orderbook.hpp` guards on
   `ORDERBOOK_H`. Harmless, but pick one. Legacy guard names are worse:
   `UNTITLED_MACROS_H` (exchange config) and `TOYEXCHANGE_ORDERGATEWAY_HPP`
   (`server.hpp`) name files and a project that no longer exist.
3. **Banner comments are half-migrated.** `order.hpp`, `orderbook.hpp`,
   `validation.hpp`, `ouch.hpp` have real banners; `order_types.hpp`,
   `ring_buffer.hpp`, `lib.hpp`, `protocol.hpp`, both `config.hpp` files and
   `load_generator.cpp` still carry `// Created by ...` stubs.
4. **Tests are not in the build.** None of the four test files
   (`engine_test.cpp`, `test_validation.cpp`, `test_server_pipeline.cpp`,
   `test_integration.cpp`) appear in any `CMakeLists.txt`, and the README never
   says how to run them. They can only be compiled by hand. This undercuts the
   whole test section above — worth fixing first.
5. **`test_runner.hpp` is duplicated** in `engine/tests/` and `server/tests/`.
   Candidate for a single copy under a shared test directory.
6. **`Order::fill` throws `std::logic_error` on the hot path,** which
   contradicts §5.5. It is an invariant violation, so an `assert` (debug-only)
   or a `bool` return would match the rest of the codebase.
7. **`OUTBOUND_BSIZE` is 32, but its own comment describes frames of 24 and 33
   bytes** and calls the frame "fixed 64 bytes (cache-line aligned)". The
   comment and the constant disagree; at minimum the ERROR arithmetic (33) does
   not fit in 32.
8. **`lib.hpp` uses `std::cerr` and `errno` without including `<iostream>` or
   `<cerrno>`,** and includes the libstdc++-internal `<bits/cpu-set.h>`. It
   compiles only by transitive luck.
9. **`REORGANIZATION.md` still refers to `.h` files** throughout, predating the
   `.hpp` standardization. Either annotate it as historical or update it.
10. **`protocol/` vs `infra/` is mid-flight.** The current branch moves
    `protocol.hpp` out of `infra/` into a new `protocol/` directory alongside
    `ouch.hpp`, but `README.md` and the `CMakeLists.txt` include paths still
    point at `infra/`. Both need updating before this lands.
11. **Test naming diverges between modules.** Engine tests are
    `new_bid_rests_on_book` (snake_case); server validation tests are
    `validate_new_BidLimitValid` (mixed).
12. **`exchange/server/desc.txt`** is a tracked plain-text design note (the
    server's intended request pipeline) sitting in a source directory with no
    counterpart elsewhere. Either make "a `desc.txt` design note per module" a
    stated convention, or fold it into the server banner / README.
