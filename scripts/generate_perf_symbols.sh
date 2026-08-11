#!/bin/sh
# 从首次链接 ELF 生成诊断内核使用的紧凑 text 符号表，或校验二次链接地址稳定。
set -eu

mode=${1:-}
cross=${2:-}
first_elf=${3:-}
output=${4:-}

if [ -z "$mode" ] || [ -z "$cross" ] || [ -z "$first_elf" ] || [ -z "$output" ]; then
    echo "usage: $0 {generate|verify} CROSS_COMPILE FIRST_ELF OUTPUT" >&2
    exit 2
fi

nm_bin="${cross}nm"
cxxfilt_bin="${cross}c++filt"
command -v "$nm_bin" >/dev/null 2>&1 || { echo "missing $nm_bin" >&2; exit 1; }
command -v "$cxxfilt_bin" >/dev/null 2>&1 || { echo "missing $cxxfilt_bin" >&2; exit 1; }

tmp_dir=$(mktemp -d /tmp/f7ly-perf-symbols.XXXXXX)
cleanup() {
    unlink "$tmp_dir/raw" 2>/dev/null || true
    unlink "$tmp_dir/names" 2>/dev/null || true
    unlink "$tmp_dir/joined" 2>/dev/null || true
    unlink "$tmp_dir/first" 2>/dev/null || true
    unlink "$tmp_dir/second" 2>/dev/null || true
    rmdir "$tmp_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

extract_text() {
    "$nm_bin" -n --defined-only "$1" |
        awk 'NF >= 3 && ($2 == "t" || $2 == "T") && $3 !~ /^[$.]/ { print $1 " " $3 }'
}

case "$mode" in
generate)
    extract_text "$first_elf" >"$tmp_dir/raw"
    cut -d' ' -f2- "$tmp_dir/raw" | "$cxxfilt_bin" >"$tmp_dir/names"
    paste -d '	' "$tmp_dir/raw" "$tmp_dir/names" >"$tmp_dir/joined"
    awk '
        BEGIN {
            print ".section .rodata.f7ly_perf_symbols,\"a\",@progbits"
            print ".balign 8"
            print ".global __f7ly_perf_symbols_start"
            print "__f7ly_perf_symbols_start:"
        }
        {
            split($1, raw, " ")
            address[NR] = raw[1]
            name[NR] = substr($0, index($0, "\t") + 1)
            print "  .quad 0x" address[NR]
            print "  .long .Lf7ly_perf_name_" NR " - __f7ly_perf_symbol_names_start"
            print "  .long 0"
        }
        END {
            print ".global __f7ly_perf_symbols_end"
            print "__f7ly_perf_symbols_end:"
            print ".global __f7ly_perf_symbol_names_start"
            print "__f7ly_perf_symbol_names_start:"
            for (i = 1; i <= NR; ++i) {
                escaped = name[i]
                gsub(/\\/, "\\\\", escaped)
                gsub(/"/, "\\\"", escaped)
                print ".Lf7ly_perf_name_" i ":"
                print "  .asciz \"" escaped "\""
            }
            print ".section .note.GNU-stack,\"\",@progbits"
        }
    ' "$tmp_dir/joined" >"$output"
    ;;
verify)
    second_elf=$output
    extract_text "$first_elf" >"$tmp_dir/first"
    extract_text "$second_elf" >"$tmp_dir/second"
    if ! cmp -s "$tmp_dir/first" "$tmp_dir/second"; then
        echo "perf symbol relink changed text addresses" >&2
        diff -u "$tmp_dir/first" "$tmp_dir/second" | sed -n '1,80p' >&2 || true
        exit 1
    fi
    ;;
*)
    echo "unknown mode: $mode" >&2
    exit 2
    ;;
esac
