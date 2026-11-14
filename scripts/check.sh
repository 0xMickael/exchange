#!/usr/bin/env bash
# Runs every check that CI runs: formatting, all presets with GCC and Clang,
# and static analysis. Usage: scripts/check.sh
set -euo pipefail
cd "$(dirname "$0")/.."

sources() { git ls-files --cached --others --exclude-standard "$@"; }

echo "== format"
sources '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror

for preset in debug release asan tsan; do
    echo "== g++ / $preset"
    CXX=g++ cmake --preset "$preset" > /dev/null
    cmake --build --preset "$preset" > /dev/null
    ctest --preset "$preset" --output-on-failure | tail -n 3

    echo "== clang++ / $preset"
    CXX=clang++ cmake --preset "$preset" -B "build/clang-$preset" > /dev/null
    cmake --build "build/clang-$preset" > /dev/null
    ctest --test-dir "build/clang-$preset" --output-on-failure | tail -n 3
done

echo "== clang-tidy"
sources '*.cpp' | xargs -P "$(nproc)" -n 1 clang-tidy -p build/debug --quiet

echo "All checks passed."
