#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 <riscv|loongarch> <cross-prefix> <kernel-elf> [kernel-object-or-archive ...]" >&2
    exit 2
}

[[ $# -ge 3 ]] || usage

arch=$1
cross_prefix=$2
kernel_elf=$3
shift 3

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd -- "${script_dir}/.." && pwd)
readelf_bin="${cross_prefix}readelf"
objdump_bin="${cross_prefix}objdump"
nm_bin="${cross_prefix}nm"

fail() {
    echo "kernel-no-fp: $*" >&2
    exit 1
}

for tool in "${readelf_bin}" "${objdump_bin}" "${nm_bin}"; do
    command -v "${tool}" >/dev/null 2>&1 || fail "missing tool: ${tool}"
done
[[ -r "${kernel_elf}" ]] || fail "kernel ELF not readable: ${kernel_elf}"

case "${arch}" in
    riscv)
        expected_machine='RISC-V'
        context_begin='<_trampoline>:'
        context_end='<_sig_trampoline>:'
        ;;
    loongarch)
        expected_machine='LoongArch'
        context_begin='<_ueentry>:'
        context_end='<_tlbrentry>:'
        ;;
    *)
        usage
        ;;
esac

check_soft_abi() {
    local input=$1
    local headers flags

    [[ -r "${input}" ]] || fail "ABI input not readable: ${input}"
    headers=$("${readelf_bin}" -h "${input}") || fail "readelf failed: ${input}"
    grep -Fq "Machine:" <<<"${headers}" || fail "no ELF header found: ${input}"
    if grep -F "Machine:" <<<"${headers}" | grep -Fvq "${expected_machine}"; then
        fail "unexpected machine type: ${input}"
    fi

    flags=$(grep -F "Flags:" <<<"${headers}" || true)
    [[ -n "${flags}" ]] || fail "no ELF ABI flags found: ${input}"
    if grep -Eiq '(double-float|single-float|DOUBLE-FLOAT|SINGLE-FLOAT)' <<<"${flags}"; then
        fail "hard-float ABI object detected: ${input}"
    fi
    if grep -Eiv 'soft-float|SOFT-FLOAT' <<<"${flags}" | grep -q .; then
        fail "object is not marked soft-float ABI: ${input}"
    fi
}

check_soft_abi "${kernel_elf}"
for input in "$@"; do
    check_soft_abi "${input}"
done

# uservec/trampoline 是唯一允许显式访问用户扩展现场的内核代码。链接脚本把
# 它们合并进 .text，因此按稳定的边界符号跳过，而不是粗暴排除整个 .text。
objdump_args=(-d --no-show-raw-insn)
if [[ "${arch}" == riscv ]]; then
    # 禁用伪指令别名后同时检查 c.f* 压缩浮点编码，避免别名掩盖真实指令类别。
    objdump_args+=(-M no-aliases)
fi
bad_instructions=$(
    "${objdump_bin}" "${objdump_args[@]}" "${kernel_elf}" |
        awk -v arch="${arch}" -v begin="${context_begin}" -v end="${context_end}" '
            index($0, begin) { in_context = 1; saw_begin = 1; next }
            index($0, end)   { in_context = 0; saw_end = 1 }
            index($0, "<initcode_start>:") { in_initcode = 1; saw_init_begin = 1; next }
            index($0, "<initcode_end>:")   { in_initcode = 0; saw_init_end = 1; next }
            in_context || in_initcode || $0 !~ /^[[:space:]]*[[:xdigit:]]+:/ { next }
            {
                mnemonic = $2
                bad = 0
                if (arch == "riscv") {
                    bad = (mnemonic ~ /^c[.]f/) ||
                          (mnemonic ~ /^f/ && mnemonic !~ /^fence([.]i)?$/)
                } else {
                    bad = (mnemonic ~ /^f/) || (mnemonic ~ /^(x?v)[[:alnum:]_.]*/)
                }
                if (bad)
                    print
            }
            END {
                if (!saw_begin || !saw_end || !saw_init_begin || !saw_init_end)
                    exit 3
            }
        '
) || fail "dedicated context-assembly boundary symbols are missing"

if [[ -n "${bad_instructions}" ]]; then
    printf '%s\n' "${bad_instructions}" >&2
    fail "floating-point/vector instruction found outside dedicated context assembly"
fi

# 即使 soft ABI 正确，C/C++ 浮点表达式仍可能退化为 libgcc helper；这些调用
# 同样会在内核路径中破坏“无运行期浮点”的不变量。
soft_helpers=$(
    "${nm_bin}" -A "${kernel_elf}" |
        grep -Ei '(^|[[:space:]])__(add|sub|mul|div|neg|abs|sqrt|cmp|eq|ne|lt|le|gt|ge|unord|min|max)(hf|sf|df|xf|tf|sc|dc|xc|tc)[0-9]*($|[[:space:]])|(^|[[:space:]])__(extend|trunc|fix|fixuns|float|floatun)[[:alnum:]_]*(hf|sf|df|xf|tf)[[:alnum:]_]*($|[[:space:]])' || true
)
if [[ -n "${soft_helpers}" ]]; then
    printf '%s\n' "${soft_helpers}" >&2
    fail "compiler soft-float helper linked into kernel"
fi

if [[ "${arch}" == loongarch ]]; then
    lsx_whitelist="${project_root}/kernel/trap/loongarch/uservec.S"
    [[ -r "${lsx_whitelist}" ]] || fail "missing LSX whitelist source: ${lsx_whitelist}"
    grep -Eq '\$(vr|xr)[0-9]+' "${lsx_whitelist}" || fail "LSX whitelist source no longer contains an LSX register"

    lsx_violations=$(
        while IFS= read -r -d '' source; do
            [[ "${source}" == "${lsx_whitelist}" ]] && continue
            grep -EnH '\$(vr|xr)[0-9]+' "${source}" || true
        done < <(find "${project_root}/kernel" -type f \
            \( -name '*.S' -o -name '*.s' -o -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.hh' \) \
            -print0)
    )
    if [[ -n "${lsx_violations}" ]]; then
        printf '%s\n' "${lsx_violations}" >&2
        fail "LoongArch LSX/LASX source exists outside uservec.S whitelist"
    fi
fi

echo "kernel-no-fp: ${arch} soft ABI and instruction checks passed"
