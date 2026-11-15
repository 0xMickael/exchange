# exchange
A C++20 limit order matching engine with TCP networking, lock-free queues, and reproducible latency benchmarks.

> **Status: in development.** The matching engine is implemented and tested as a library, with a replay tool. The TCP server, the command-line client and the benchmarks are not written yet. The behavior is specified in [docs/SPEC.md](docs/SPEC.md).

## Repository layout

| Path | Content |
|---|---|
| `src/exchange/core/` | Order book and matching engine: a library with no I/O |
| `apps/replay/` | `exchange-replay`, which runs a command script through the engine |
| `tests/` | Unit tests, replay scenarios, and a randomized comparison against a reference engine |
| `docs/SPEC.md` | Technical specification |
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

## Replaying a command script

`exchange-replay` runs a text script of commands through the matching engine and prints every event. The engine is deterministic, so a script always produces the same output.

```sh
./build/debug/apps/exchange-replay tests/data/replay/spec_example_a.commands
```

The script and output formats are described in [docs/SPEC.md](docs/SPEC.md), section 12.

## Formatting and static analysis

Both checks run in CI with clang-format 21 and clang-tidy 21. To run them locally, after configuring the `debug` preset:

```sh
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
git ls-files '*.cpp' | xargs clang-tidy -p build/debug --quiet
```

The first command reformats the sources in place. The second reports problems and exits with an error if it finds any.

`scripts/check.sh` runs the formatting check, all four presets with both GCC and Clang, and the static analysis.
