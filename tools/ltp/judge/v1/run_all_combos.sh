#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/combos.sh"

for combo in "${COMBOS[@]}"; do
    echo "=== running combo: $combo ==="
    bash "$SCRIPT_DIR/run_combo_pipeline.sh" "$combo"
    echo
done

echo "all combos completed"
