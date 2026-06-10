#!/bin/bash
set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"

cmake -B "$BUILD_DIR" -S .
cmake --build "$BUILD_DIR" "$@"
