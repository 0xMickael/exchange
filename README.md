# exchange
A C++20 limit order matching engine with TCP networking, lock-free queues, and reproducible latency benchmarks.

> **Status: in development.** The matching engine, the binary protocol and a single-threaded TCP server work and are tested. The command-line client, the two-thread server and the end-to-end benchmark are not written yet. The behavior is specified in [docs/SPEC.md](docs/SPEC.md).

## Repository layout

| Path | Content |
|---|---|
| `src/exchange/core/` | Order book and matching engine: a library with no I/O |
| `src/exchange/queue/` | Lock-free single-producer single-consumer queue |
| `src/exchange/protocol/` | Binary wire protocol: message definitions, encoder and decoder |
| `src/exchange/net/` | Small wrappers over Linux sockets |
| `src/exchange/server/` | TCP server: event loop, sessions, routing of events to clients |
| `apps/server/` | `exchange-server` |
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

## Running the server

```sh
./build/debug/apps/exchange-server --port 9000
```

The server listens on `127.0.0.1` by default and runs until interrupted with Ctrl-C. It speaks the binary protocol of [docs/SPEC.md](docs/SPEC.md), section 7. The command-line client is not written yet; for now the server is exercised by the integration tests in `tests/server/`.

## Replaying a command script

`exchange-replay` runs a text script of commands through the matching engine and prints every event. The engine is deterministic, so a script always produces the same output.

```sh
./build/debug/apps/exchange-replay tests/data/replay/spec_example_a.commands
```

The script and output formats are described in [docs/SPEC.md](docs/SPEC.md), section 12.

## Benchmark

`exchange-bench` has two benchmarks. `engine` measures the matching engine alone and reports the latency distribution and the throughput. `queue` compares the lock-free queue with a mutex-protected one. Build them with the `release` preset:

```sh
cmake --workflow --preset release
./build/release/bench/exchange-bench engine
./build/release/bench/exchange-bench queue
```

Measured results, with the machine and method they come from, are in [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## Formatting and static analysis

Both checks run in CI with clang-format 21 and clang-tidy 21. To run them locally, after configuring the `debug` preset:

```sh
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
git ls-files '*.cpp' | xargs clang-tidy -p build/debug --quiet
```

The first command reformats the sources in place. The second reports problems and exits with an error if it finds any.

`scripts/check.sh` runs the formatting check, all four presets with both GCC and Clang, and the static analysis.
