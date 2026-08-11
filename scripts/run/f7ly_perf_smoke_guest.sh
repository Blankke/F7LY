#!/bin/sh
# 诊断内核的短验收入口；由 buildstorm_perf_probe.sh 写入临时 rootfs 后执行。
set -u

PERF=/usr/bin/f7ly-perf
ROOT=/proc/f7ly/perf
failed=0

fail()
{
    echo "PERF_SMOKE_ERROR $*" >&2
    failed=1
}

meta_value()
{
    awk -F '\t' -v key="$1" '$1 == key { print $2; exit }' "${ROOT}/meta"
}

metric_sum()
{
    awk -F '\t' -v name="$1" '$6 == name { sum += $9 } END { print sum + 0 }' "${ROOT}/metrics"
}

json_samples()
{
    sed -n 's/.*"samples":\([0-9][0-9]*\).*/\1/p'
}

echo "PERF_SMOKE_BEGIN"
test -x "${PERF}" || fail "missing_cli"
test -r "${ROOT}/meta" || fail "missing_proc_abi"

"${PERF}" status --json || fail "status"
"${PERF}" stat --interval-ms 1000 --count 1 --json || fail "stat"
timer_flat=$(${PERF} top --backend timer --event cycles --frequency 100 --duration 2 --limit 20 --json) ||
    fail "timer_flat"
echo "${timer_flat}"
timer_flat_samples=$(printf '%s\n' "${timer_flat}" | json_samples)
test "${timer_flat_samples:-0}" -gt 0 || fail "timer_flat_no_samples"

timer_callgraph=$(${PERF} top --backend timer --event cycles --frequency 100 --callgraph --duration 2 --limit 20 --json) ||
    fail "timer_callgraph"
echo "${timer_callgraph}"
timer_callgraph_samples=$(printf '%s\n' "${timer_callgraph}" | json_samples)
test "${timer_callgraph_samples:-0}" -gt 0 || fail "timer_callgraph_no_samples"

pmu_cycles=$(meta_value pmu_cycles)
auto_output=$(${PERF} top --backend auto --event cycles --frequency 100 --duration 1 --limit 5 --json) ||
    fail "auto_backend"
echo "${auto_output}"
auto_samples=$(printf '%s\n' "${auto_output}" | json_samples)
test "${auto_samples:-0}" -gt 0 || fail "auto_backend_no_samples"
active_backend=$(meta_value profile_active_backend)
if test "${pmu_cycles}" = unavailable && test "${active_backend}" != timer; then
    fail "auto_did_not_fallback active=${active_backend}"
fi

if test "${pmu_cycles}" = unavailable; then
    if "${PERF}" top --backend pmu --event cycles --period 1000000 --duration 1 --limit 5 --json; then
        fail "explicit_pmu_should_fail"
    else
        echo "PERF_SMOKE_PMU unsupported=true"
    fi
fi

metrics_before=$(meta_value metrics_epoch)
profile_before=$(meta_value profile_epoch)
syscalls_before=$(metric_sum syscall.total)
"${PERF}" reset all || fail "reset_all"
metrics_after=$(meta_value metrics_epoch)
profile_after=$(meta_value profile_epoch)
if test "${metrics_after}" -le "${metrics_before}" || test "${profile_after}" -le "${profile_before}"; then
    fail "epoch_not_advanced metrics=${metrics_before}:${metrics_after} profile=${profile_before}:${profile_after}"
fi

syscalls_after=$(metric_sum syscall.total)
if test "${syscalls_after}" -ge "${syscalls_before}"; then
    fail "metrics_not_reset syscall_total=${syscalls_before}:${syscalls_after}"
fi
if ! awk -F '\t' 'NR > 2 && $4 == "stats" && $6 != 0 { bad=1 } END { exit bad }' "${ROOT}/profile"; then
    fail "profile_not_zero_after_reset"
fi

if printf '%s' 'profile start backend=timer event=cycles frequency=3 period=1 callchain=0' >"${ROOT}/control" 2>/dev/null; then
    fail "invalid_frequency_accepted"
fi
if printf '%s' 'unknown command' >"${ROOT}/control" 2>/dev/null; then
    fail "unknown_command_accepted"
fi

if test "${failed}" -eq 0; then
    echo "PERF_SMOKE_RESULT ok=true metrics_epoch=${metrics_after} profile_epoch=${profile_after} backend=${active_backend}"
    exit 0
fi
echo "PERF_SMOKE_RESULT ok=false metrics_epoch=${metrics_after:-unknown} profile_epoch=${profile_after:-unknown}"
exit 1
