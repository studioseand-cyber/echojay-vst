#!/bin/bash
# Builds the standalone harness tree at -j 2 (never more: see never-bare-j-build) and runs its tests.
set -e
cd "$(dirname "$0")/../.."
cmake -S tools/limiter_ab_guard -B build-limiter-ab -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-limiter-ab -j 2
ctest --test-dir build-limiter-ab --output-on-failure
