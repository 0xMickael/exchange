# exchange
A C++20 limit order matching engine with TCP networking, lock-free queues, and reproducible latency benchmarks.

> **Status: in development.** The matching engine, the binary protocol, the TCP server in both its one-thread and two-thread modes, the command-line client and the benchmarks work and are tested. Profile-guided optimization and a demo script remain to be done. The behavior is specified in [docs/SPEC.md](docs/SPEC.md).

## Repository layout

| Path | Content |
|---|---|
| `src/exchange/core/` | Order book and matching engine: a library with no I/O |
| `src/exchange/queue/` | Lock-free single-producer single-consumer queue |
| `src/exchange/protocol/` | Binary wire protocol: message definitions, encoder and decoder |
| `src/exchange/net/` | Small wrappers over Linux sockets |
| `src/exchange/server/` | TCP server: event loop, sessions, routing of events to clients, engine thread |
| `src/exchange/client/` | Command-line client logic: commands, display, local copy of the book |
| `apps/server/` | `exchange-server` |
| `apps/cli/` | `exchange-cli` |
| `apps/replay/` | `exchange-replay`, which runs a command script through the engine |
| `bench/` | `exchange-bench`, the benchmark tool |
| `tests/` | Unit tests, replay scenarios, and a randomized comparison against a reference engine |
| `docs/SPEC.md` | Technical specification |
| `docs/BENCHMARKS.md` | Measured results and how to reproduce them |
| `scripts/check.sh` | Runs every check that CI runs |

## Building and testing

Requirements: Linux, a C++20 compiler (GCC or Clang), CMake 3.25 or newer, and Ninja.
GoogleTest is downloaded automatically at configure time.

```sh
cmake --workflow --preset debug
```

This configures, builds and runs the tests in `build/debug`. The available presets are:

| Preset | Purpose |
|---|---|
| `debug` | Unoptimized build with assertions |
| `release` | Optimized build |
| `asan` | AddressSanitizer and UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |

To use a specific compiler, set `CXX` before the first configure of a preset, for example `CXX=clang++ cmake --workflow --preset debug`.

## Trying it

Start the server in one terminal:

```sh
./build/debug/apps/exchange-server --port 9000
```

By default the matching engine runs on its own thread, connected to the network thread by two lock-free queues. `--mode inline` runs everything on one thread instead; `exchange-server --help` lists the options.

Then start a client in two other terminals:

```sh
./build/debug/apps/exchange-cli --port 9000
```

In the first client, place two sell orders. In the second, look at the book and buy:

```text
> sell 101 5                          > book
order 1 accepted: sell 5 @ 101                        BIDS |       ASKS
> sell 102 10                           quantity     price |      price  quantity
order 2 accepted: sell 10 @ 102                            |        101         5
                                                           |        102        10
                                      > buy 102 8
                                      order 3 accepted: buy 8 @ 102
                                      order 3 filled: 5 @ 101, 3 remaining
                                      market trade: 5 @ 101, buy side aggressor
                                      order 3 filled: 3 @ 102, complete
                                      market trade: 3 @ 102, buy side aggressor
```

The first client is told that its orders were filled. `help` lists the commands. Prices are in ticks and quantities in units, both whole numbers. Leaving a client cancels its orders.

The client also reads commands from a pipe or a file, and then prints the same transcript on every run:

```sh
printf 'buy 100 5\nbook\n' | ./build/debug/apps/exchange-cli --port 9000
```

## Replaying a command script

`exchange-replay` runs a text script of commands through the matching engine and prints every event. The engine is deterministic, so a script always produces the same output.

```sh
./build/debug/apps/exchange-replay tests/data/replay/spec_example_a.commands
```

The script and output formats are described in [docs/SPEC.md](docs/SPEC.md), section 12.

## Benchmark

`exchange-bench` has three benchmarks. `engine` measures the matching engine alone. `queue` compares the lock-free queue with a mutex-protected one. `e2e` measures requests sent over TCP to a server, in each server mode. Build them with the `release` preset:

```sh
cmake --workflow --preset release
./build/release/bench/exchange-bench engine
./build/release/bench/exchange-bench queue
./build/release/bench/exchange-bench e2e
```

On the reference machine (a laptop under WSL2): about 0.4 µs per command in the engine alone, and about 65 µs per request end to end over loopback TCP.

Measured results, with the machine and method they come from, are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Formatting and static analysis

Both checks run in CI with clang-format 21 and clang-tidy 21. To run them locally, after configuring the `debug` preset:

```sh
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
git ls-files '*.cpp' | xargs clang-tidy -p build/debug --quiet
```

The first command reformats the sources in place. The second reports problems and exits with an error if it finds any.

`scripts/check.sh` runs the formatting check, all four presets with both GCC and Clang, and the static analysis.
