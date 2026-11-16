# Design decisions

Each decision that shaped this project, with the alternative that was considered and the reason for the choice. The behavior these decisions lead to is specified in [SPEC.md](SPEC.md), which refers to them by number.

Numbers follow the order in which the decisions were taken, so they are not consecutive within a theme. Decisions D23 to D26 came out of measurements; the numbers behind them are in [BENCHMARKS.md](BENCHMARKS.md).

## Matching engine and order book

| # | Decision | Alternative considered | Reason |
|---|---|---|---|
| D3 | Clients supply a strictly increasing `client_order_id`; the engine assigns `OrderId`. Cancels name the `OrderId`. | Client-chosen order identifiers; arbitrary unique client identifiers. | Duplicate detection needs one integer per session instead of a history of all identifiers. |
| D4 | A session's active orders are cancelled when it closes. | Leave them resting. | Without accounts, nobody could ever cancel them. |
| D7 | One `BookUpdate` per level per command, emitted after the last trade at that level. | One `BookUpdate` per trade. | A large order sweeping one level produces one update instead of one per resting order. |
| D8 | A single cancel reject reason, `UnknownOrder`. | Separate reasons for filled, cancelled, and foreign orders. | Avoids unbounded history and does not reveal other sessions' orders. |
| D9 | The book uses standard containers: an ordered map of price levels per side, a linked list of orders per level, and a hash map from `OrderId` to the order's position. | A sorted array of levels; an array indexed by price; a custom memory pool. | It is the simplest structure with the right complexity for every operation. It is the baseline that later optimizations must beat in a benchmark (see D23 for the one that did). |
| D10 | `Price`, `Quantity` and the identifier types are plain integer aliases. Aggregates are built with designated initializers, which static analysis enforces. | A distinct wrapper type for each. | Wrapper types catch swapped arguments at compile time but need operator boilerplate. Naming every field at the construction site covers most of the same risk. |
| D21 | The number of active orders is not capped. | A global cap; a cap per session. | Without accounts, neither gives real protection: a global cap lets one client fill the book and block everyone else, and a per-session cap is bypassed by opening more connections. It is listed as a known limitation instead of adding a reject reason that protects nothing. |
| D23 | The orders of a session are chained to one another inside the book, oldest first. | An ordered set of `OrderId`s per session, kept by the engine (the first implementation). | Profiling showed that maintaining the ordered set took more time than matching: a tree to search and rebalance for every order added, filled or cancelled. The chain needs no search and no memory of its own. Measured gain in [BENCHMARKS.md](BENCHMARKS.md). |
| D26 | Orders are allocated one by one with the default allocator. | Take them from a memory pool (`std::pmr::unsynchronized_pool_resource`). | Measured: with the standard pool the engine was about 28 % slower. The default allocator already keeps recently freed small blocks at hand, and the pool adds an indirect call and a search by size to every allocation. A pool written for this one use could do better, but allocation is a small part of what is left ([BENCHMARKS.md](BENCHMARKS.md)). |

## Market data and wire protocol

| # | Decision | Alternative considered | Reason |
|---|---|---|---|
| D2 | Market data is reliable. A client that cannot keep up is disconnected and resynchronizes from a snapshot when it reconnects. | Lossy market data with gap detection and replay. | Over TCP the only source of loss is a slow client; disconnecting it needs no recovery protocol. |
| D13 | A market data subscription is served by a snapshot command that goes through the engine like an order. | Let the network thread read the book directly; maintain a copy of the book in the network thread. | The book has a single owner, so reading it from another thread would need a lock. Putting the snapshot in the event stream also fixes exactly which updates come before and after it. |
| D14 | Market data sequence numbers are assigned by the publisher and exist only on the wire. | Sequence fields inside the engine's `Trade` and `BookUpdate` events. | The engine's output stays a function of its commands alone, and replay files do not depend on a numbering that only matters to network clients. |
| D15 | The wire format is little-endian with a 3-byte header (`length`, `type`) and fixed-size bodies, encoded field by field. | Big-endian "network order"; sending C++ structs; a text protocol. | Every machine this runs on is little-endian. Field-by-field encoding makes the format independent of compiler padding and alignment. Fixed sizes make a malformed length detectable. |

## Threads and networking

| # | Decision | Alternative considered | Reason |
|---|---|---|---|
| D1 | Two threads: one I/O thread owning every socket, one matching thread. The I/O thread also consumes the event queue. | A third thread that publishes events. | A publisher writing to sockets that another thread reads and closes makes connection state shared between threads. |
| D5 | The server is first built with the engine called directly from the I/O thread, then with a dedicated engine thread. Both modes remain available. | Build the threaded pipeline directly. | Separates networking bugs from concurrency bugs, and lets the benchmark compare the two designs. |
| D12 | Threads communicate through a lock-free SPSC ring buffer written for this project. | A queue protected by a mutex; an existing library. | Writing it is one of the project's goals. It is kept only because the benchmark shows a gain over the mutex version ([BENCHMARKS.md](BENCHMARKS.md)). |
| D16 | The network side is one thread using level-triggered `epoll` on non-blocking sockets. | One thread per connection; edge-triggered `epoll`; `io_uring`. | A thread per connection would need locks around everything shared. Edge-triggered mode reports readiness only once, so a missed read stalls a connection silently; level-triggered mode cannot lose a readiness report. |
| D19 | An idle engine thread polls briefly, then sleeps in short naps; spinning forever is an option. | Always spin; block on a condition variable or an `eventfd`. | Spinning forever uses a whole core for a server that is mostly idle. Blocking needs a wake-up protocol whose missed-wake-up cases are subtle; naps cannot miss a wake-up and their cost is measured in the end-to-end benchmark. |
| D20 | When the command queue is full, the network thread stops reading sockets instead of rejecting orders. | Reject with a "busy" message; block the network thread. | Stopping to read lets TCP slow the senders down with no loss and no new message type. Blocking would allow the two threads to wait for each other. |
| D22 | Pipeline mode is the default. | Make inline the default. | The pipeline is the architecture this project is about. In the first end-to-end measurements inline mode answered 1.3 times faster with one client; since D24 the gap is about 15 % with one client and under 5 % in throughput with four. The network path, not matching, dominates a request, so the second thread has nothing to relieve there. Both modes remain one option away. |
| D24 | The network thread does not sleep while the engine thread owes it answers, and the engine thread never wakes it up. | The engine thread signals its events through an `eventfd` (the first implementation). | With the signal, every request made the network thread sleep and be woken a second time, which cost more than the matching it was waiting for. Without it there is one mechanism less, and no missed-wake-up case to reason about. Measured gain in [BENCHMARKS.md](BENCHMARKS.md). |
| D25 | The network thread sleeps when idle; never sleeping is an option. | Never sleep by default. | Not sleeping answers about five times faster on the reference machine, where waking a thread is slow, but uses a whole core for a server that is mostly idle. The option exists mainly to measure what the wake-up costs. |

## Command-line client

| # | Decision | Alternative considered | Reason |
|---|---|---|---|
| D17 | The command-line client shows prices in ticks and quantities in units, as whole numbers. | Decimal display with a configurable tick size. | A conversion would only exist in the client and add a place where rounding could go wrong; integers are what the protocol and the engine use. |
| D18 | The command-line client waits for the server's answers after each command, using the cancel of order 0 as a marker. | Print answers whenever they arrive; add a dedicated ping message to the protocol. | It makes scripted sessions produce the same transcript on every run. The marker must pass through the engine to be ordered after the engine's events, which an existing request already does. |

## Tooling and measurement

| # | Decision | Alternative considered | Reason |
|---|---|---|---|
| D6 | GoogleTest is the only third-party dependency. Benchmarks use a harness written in this repository. | Catch2 or doctest; Google Benchmark; HdrHistogram. | GoogleTest is the framework most widely used in C++ industry codebases. The benchmark harness needs full latency distributions, which is a small amount of code. |
| D11 | Latencies are timed with the processor's timestamp counter, calibrated against `steady_clock` and cross-checked after every pass. | `steady_clock` only. | Under WSL2 `steady_clock` advances in steps of 100 ns, which is too coarse for operations of about 500 ns. |
