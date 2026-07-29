#!/bin/sh
#
# LA 性能探针跳过 CAgent 的 guest 辅助入口。
#
# 使用示例：
#   由 scripts/run/buildstorm_perf_probe.sh 自动写入临时镜像。

echo "#### OS COMP TEST GROUP START cagent-perf-skip ####"
echo "CAGENT_PERF_SKIPPED"
echo "#### OS COMP TEST GROUP END cagent-perf-skip ####"

