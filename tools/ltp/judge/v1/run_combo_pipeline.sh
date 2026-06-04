#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../../../.." && pwd)

COMBO="${1:-riscv-musl}"
OUT_DIR="$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO"
RAW_LOG="$OUT_DIR/raw.log"
PARSED_TSV="$OUT_DIR/parsed.tsv"
ANALYSIS_TXT="$OUT_DIR/analysis.txt"
RANK_TXT="$OUT_DIR/rank.txt"

mkdir -p "$OUT_DIR"

bash "$SCRIPT_DIR/runner/run_combo.sh" "$COMBO" "$RAW_LOG"
bash "$SCRIPT_DIR/parser/parse_ltp.sh" "$COMBO" "$RAW_LOG" "$PARSED_TSV" "$ANALYSIS_TXT"
bash "$SCRIPT_DIR/ranker/rank_ltp.sh" "$COMBO" "$PARSED_TSV" "$RANK_TXT"

echo "combo=$COMBO"
echo "raw_log=$RAW_LOG"
echo "parsed_tsv=$PARSED_TSV"
echo "analysis_txt=$ANALYSIS_TXT"
echo "rank_txt=$RANK_TXT"
