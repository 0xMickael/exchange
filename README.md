# Exchange

[![CI](https://github.com/0xMickael/exchange/actions/workflows/ci.yml/badge.svg)](https://github.com/0xMickael/exchange/actions/workflows/ci.yml)

A limit order matching engine in C++20, with a TCP server, a lock-free queue between its two threads, and benchmarks in which every number was measured.

An exchange receives orders to buy and orders to sell, and its matching engine decides who trades with whom, and at what price. This project is such an engine, with the server around it.

I built it to understand how an electronic exchange could work underneath: how orders are matched, how two threads hand work to each other without a lock, and where the time of a request really goes. It is small on purpose and is not meant for real trading. What it tries to do well is to state its behavior precisely, test it, and measure instead of assuming.

## What it does

- **Matching.** One instrument and limit orders: an order states a price it will not go beyond. The best price trades first, and at the same price the oldest order trades first. An order can be filled in several parts, cancelled, and is cancelled automatically when its client disconnects.
- **Network.** Clients talk to the server over TCP with a small binary protocol. Each client is told what happens to its own orders and can follow the market: the state of the order book, then every change.
- **Reproducibility.** The same orders always produce the same result, so a scenario can be replayed from a file.
- **Four programs.** `exchange-server`, `exchange-cli` (an interactive client), `exchange-bench` (the benchmarks) and `exchange-replay` (runs a scenario file through the engine).

Left out on purpose: several instruments, market and stop orders, order modification, accounts, persistence. [docs/SPEC.md](docs/SPEC.md) specifies everything that is in.

## Architecture

```text
   clients (exchange-cli, exchange-bench)
                    │  TCP, binary protocol
                    ▼
        ┌──────────────────────┐   command queue    ┌──────────────────────┐
        │    network thread    │ ─────────────────▶ │    engine thread     │
        │  sockets, sessions,  │                    │  order book,         │
        │  encoding, decoding  │ ◀───────────────── │  matching            │
        └──────────────────────┘    event queue     └──────────────────────┘
```

One thread talks to the clients and another one matches orders. They share nothing except two queues of fixed size, each with exactly one writer and one reader, which is what allows the queues to work without a lock. When the command queue is full the network thread stops reading from the clients, and TCP itself slows them down: no order is dropped or rejected because of load.

`exchange-server --mode inline` runs everything on one thread instead. It exists to compare the two designs with the same benchmark.

## Try it

Requirements: Linux, a C++20 compiler (GCC or Clang), CMake 3.25 or newer, and Ninja. GoogleTest is downloaded at configure time.

```sh
cmake --workflow --preset release    # configures, builds and runs the tests
scripts/demo.sh
```

The demo starts a server, lets two clients trade with each other and prints what each of them sees. The part where Bob buys from Alice looks like this:

```text
> book
                BIDS |       ASKS
  quantity     price |      price  quantity
                     |        101         5
                     |        102        10
> buy 102 8
order 3 accepted: buy 8 @ 102
order 3 filled: 5 @ 101, 3 remaining
market trade: 5 @ 101, buy side aggressor
order 3 filled: 3 @ 102, complete
market trade: 3 @ 102, buy side aggressor
```

To explore by hand, start `./build/release/apps/exchange-server` in one terminal and `./build/release/apps/exchange-cli` in two others, then type `help`. Prices are in ticks and quantities in units, both whole numbers. `exchange-server --help` lists the server's options.

A scenario can also be replayed from a file, without a server:

```sh
./build/release/apps/exchange-replay tests/data/replay/spec_example_a.commands
```

## Tests

More than 300 tests, in layers: the matching rules one by one, the engine against a deliberately naive second implementation on random orders, the queue between two threads, the protocol byte by byte, and a real server with real TCP connections, including clients that stop reading and queues that are full.

Every test runs in four builds: unoptimized, optimized, and two with detectors, one for memory errors and one for unsafe access from two threads. Each build is done with GCC and with Clang, on every push.

```sh
cmake --workflow --preset debug    # one build: debug, release, asan or tsan
scripts/check.sh                   # everything CI runs, including clang-format and clang-tidy
```

## Benchmarks

```sh
./build/release/bench/exchange-bench engine    # the matching engine alone
./build/release/bench/exchange-bench queue     # lock-free queue against a mutex
./build/release/bench/exchange-bench e2e       # requests over TCP to a server
```

Measured on a laptop under WSL2, a virtual machine, so the figures are indicative:

| Measurement | Result |
|---|---|
| Engine, median time of one command | 159 ns (6.7 million commands per second) |
| Lock-free queue against the same queue with a mutex | about 4 times the throughput |
| Request over TCP, one client, median | 60 µs |
| Same, with a network thread that never sleeps | 12 µs |
| Four clients at once | about 80,000 requests per second |

Three results I did not expect:

- Matching an order takes about 0.2 µs. Answering it over the network takes about 60 µs, and 80 % of that is the operating system waking up a sleeping thread.
- The engine became twice as fast by replacing one data structure that a profiler pointed at. My own guess about what was slow was wrong.
- A memory pool, a classic optimization for this kind of code, made the engine 28 % slower.

[docs/BENCHMARKS.md](docs/BENCHMARKS.md) gives the machine, the method, and what each optimization gained or failed to gain.

## What I learned

- **Specify before coding.** Writing the rules first turned vague questions into decisions, and the tests follow them line by line.
- **Keep a naive version to test against.** Comparing the engine with a slow, obviously correct one is what made it safe to rewrite it for speed.
- **Measure the measuring tool too.** My own benchmark was inflating its numbers by 15 %, and a profiler found that before anything in the server.

## Documentation

| Document | Content |
|---|---|
| [docs/SPEC.md](docs/SPEC.md) | Behavior and contracts: matching rules, protocol, threads, backpressure, benchmark method, testing strategy |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Each design decision, the alternative considered and the reason |
| [docs/BENCHMARKS.md](docs/BENCHMARKS.md) | Results, and the story of each optimization, kept or not |

## Repository layout

| Path | Content |
|---|---|
| `src/exchange/core/` | Order book and matching engine: a library with no I/O |
| `src/exchange/queue/` | Lock-free single-producer single-consumer queue |
| `src/exchange/protocol/` | Wire protocol: messages, encoder, decoder |
| `src/exchange/net/`, `src/exchange/server/` | Sockets, event loop, sessions, engine thread |
| `src/exchange/client/` | Logic of the command-line client |
| `apps/`, `bench/` | The four programs |
| `tests/` | Tests, replay scenarios and the reference engine |
| `scripts/` | `demo.sh`, `check.sh`, `ab_compare.py` |

## Known limitations

- Linux only: the server uses `epoll`.
- Nothing is saved: orders are lost when the server stops.
- The number of orders in the book is not capped, and there are no accounts, so nothing prevents one client from filling the book.
- All measurements were taken on one laptop under WSL2, with clients and server on the same machine.

The full list is in [docs/SPEC.md](docs/SPEC.md), section 16.

## Possible next steps

Ideas for later versions. None is promised.

- Let the network thread wait a short moment before it sleeps, and measure what that gains and costs.
- Repeat the measurements on native Linux, with threads pinned to cores, to see what the virtual machine hides.
- Order modification, which today means cancelling and sending a new order.

## Feedback

This is a learning project, so corrections are as welcome as questions. If you spot a mistake, a flaw in a measurement or a better way to do something, please open an [issue](https://github.com/0xMickael/exchange/issues).

## License

MIT, see [LICENSE](LICENSE).
