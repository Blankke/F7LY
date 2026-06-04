#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../../../../.." && pwd)

COMBO="${1:-riscv-musl}"
PARSED_TSV="${2:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/parsed.tsv}"
RANK_TXT="${3:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/rank.txt}"

if [ ! -f "$PARSED_TSV" ]; then
    echo "Error: parsed file not found: $PARSED_TSV" >&2
    exit 1
fi

mkdir -p "$(dirname "$RANK_TXT")"

sorted_file=$(mktemp)
trap 'rm -f "$sorted_file"' EXIT

tail -n +2 "$PARSED_TSV" \
    | sort -t $'\t' -k3,3nr -k4,4nr -k10,10nr -k2,2 > "$sorted_file"

{
    echo "# combo=$COMBO"
    echo "# source=$PARSED_TSV"
    echo
    printf "%-16s | %-24s | %-6s | %-10s | %-12s | %-6s\n" \
        "Combo" "Case Name" "TPASS" "Summary" "Status" "End"
    echo "--------------------------------------------------------------------------------"

    while IFS=$'\t' read -r combo name tpass has_summary summary_pass summary_failed summary_broken summary_skipped summary_warnings summary_total status end_code; do
        summary_flag="no"
        if [ "$has_summary" = "1" ]; then
            summary_flag="yes"
        fi

        printf "%-16s | %-24s | %-6s | %-10s | %-12s | %-6s\n" \
            "$combo" "$name" "$tpass" "$summary_flag/$summary_total" "$status" "${end_code:-N/A}"
    done < "$sorted_file"
} > "$RANK_TXT"

echo "rank_txt=$RANK_TXT"
