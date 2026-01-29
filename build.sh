#!/usr/bin/env bash
set -euo pipefail

CONFIG="${1:-Release}"
BUILD_DIR="${2:-build_vs}"

cmake -S . -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" --config "${CONFIG}"
