#!/usr/bin/env bash
# Compatibility entry point; ECU owns the implementation.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec bash "$ROOT/ECU/scripts/setup/build-vsomeip.sh" "$@"
