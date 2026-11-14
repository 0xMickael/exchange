# exchange
A C++20 limit order matching engine with TCP networking, lock-free queues, and reproducible latency benchmarks.

> **Status: early development.** The build, test and CI setup is in place. The matching engine is specified in [docs/SPEC.md](docs/SPEC.md) and not yet implemented.

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

## Formatting and static analysis

Both checks run in CI with clang-format 21 and clang-tidy 21. To run them locally, after configuring the `debug` preset:

```sh
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
git ls-files '*.cpp' | xargs clang-tidy -p build/debug --quiet
```

The first command reformats the sources in place. The second reports problems and exits with an error if it finds any.
