#!/bin/sh
# The one command to run after any change: formatting check, the full lint
# policy, then BOTH builds (debug for the tests, release for daily use).
# Stops at the first step that fails.
set -e
cd "$(dirname "$0")"

cargo fmt --check
cargo clippy --all-targets
cargo build
cargo build --release
