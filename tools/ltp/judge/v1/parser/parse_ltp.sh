#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/../../../../.." && pwd)

COMBO="${1:-riscv-musl}"
RAW_LOG="${2:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/raw.log}"
PARSED_TSV="${3:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/parsed.tsv}"
ANALYSIS_TXT="${4:-$ROOT_DIR/tools/ltp/judge/v1/outputs/$COMBO/analysis.txt}"

if [ ! -f "$RAW_LOG" ]; then
    echo "Error: raw log not found: $RAW_LOG" >&2
    exit 1
fi

mkdir -p "$(dirname "$PARSED_TSV")" "$(dirname "$ANALYSIS_TXT")"

awk -v combo="$COMBO" -v parsed_tsv="$PARSED_TSV" -v analysis_txt="$ANALYSIS_TXT" '
function reset_case() {
    current_case = ""
    has_summary = 0
    in_summary = 0
    tpass_count = 0
    summary_pass = 0
    summary_failed = 0
    summary_broken = 0
    summary_skipped = 0
    summary_warnings = 0
    summary_total = 0
    end_code = ""
    status_hint = ""
}

function flush_case(   status) {
    if (current_case == "") {
        return
    }

    if (end_code == "") {
        status = "INCOMPLETE"
    } else if (status_hint != "") {
        status = status_hint
    } else if (!has_summary) {
        status = (end_code == 0 ? "NO_SUMMARY" : "FAIL")
    } else {
        status = (end_code == 0 ? "PASS" : "FAIL")
    }

    summary_total = summary_pass + summary_failed + summary_broken + summary_skipped + summary_warnings

    printf "%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\t%s\n",
        combo, current_case, tpass_count, has_summary, summary_pass, summary_failed,
        summary_broken, summary_skipped, summary_warnings, summary_total, status,
        (end_code == "" ? "" : end_code) >> parsed_tsv

    printf "%-24s | TPASS=%-4d | Summary=%-3s | SummaryTotal=%-4d | Status=%-12s | End=%s\n",
        current_case, tpass_count, (has_summary ? "yes" : "no"), summary_total,
        status, (end_code == "" ? "N/A" : end_code) >> analysis_txt

    flushed_cases++
}

BEGIN {
    print "# combo\tname\ttpass_count\thas_summary\tsummary_pass\tsummary_failed\tsummary_broken\tsummary_skipped\tsummary_warnings\tsummary_total\tstatus\tend_code" > parsed_tsv
    printf "combo=%s\nsource=%s\n\n", combo, ARGV[1] > analysis_txt
    reset_case()
    total_lines = 0
    summary_case_count = 0
}

{
    line = $0
    gsub(/\r/, "", line)
    total_lines++

    if (line ~ /^RUN LTP CASE /) {
        flush_case()
        reset_case()

        split(line, parts, " ")
        current_case = parts[4]
        next
    }

    if (current_case == "") {
        next
    }

    if (line ~ /^MISSING CASE BINARY:/) {
        status_hint = "MISSING"
        next
    }

    if (line ~ /^Test case timed out after /) {
        status_hint = "TIMEOUT"
        next
    }

    if (line ~ /^Summary:/) {
        has_summary = 1
        in_summary = 1
        summary_case_count++
        next
    }

    if (line ~ /TPASS:/) {
        tpass_count++
    }

    if (line ~ /^(FAIL|END) LTP CASE /) {
        end_count = split(line, end_parts, " ")
        end_code = end_parts[end_count]
        flush_case()
        reset_case()
        next
    }

    if (in_summary) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
        if (line == "") {
            in_summary = 0
            next
        }

        split(line, summary_parts, /[[:space:]]+/)
        key = summary_parts[1]
        value = summary_parts[2] + 0

        if (key == "passed") {
            summary_pass += value
        } else if (key == "failed") {
            summary_failed += value
        } else if (key == "broken") {
            summary_broken += value
        } else if (key == "skipped") {
            summary_skipped += value
        } else if (key == "warnings") {
            summary_warnings += value
        }
    }
}

END {
    flush_case()
    printf "\n# total_cases=%d\n# summary_cases=%d\n# parsed_cases=%d\n", flushed_cases, summary_case_count, flushed_cases >> analysis_txt
}
' "$RAW_LOG"

echo "parsed_tsv=$PARSED_TSV"
echo "analysis_txt=$ANALYSIS_TXT"
