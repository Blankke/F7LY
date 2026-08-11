# 可选性能诊断内核与宿主工具。
#
# PERF_DIAG=1 只改变当前画像的代码生成和产物后缀；普通构建不承担任何
# 采样开销。工具按架构生成，与开发板画像无关。

PERF_TOOL_SRC := tools/perf/f7ly_perf.cc
PERF_TOOL_BIN := $(BUILD_ROOT)/perf-tools/f7ly-perf-$(PROFILE_ARCH)

.PHONY: perf-tool perf-tools perf-native-tests

perf-tool: $(PERF_TOOL_BIN)

perf-tools:
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=riscv-qemu perf-tool
	@$(MAKE) $(BUILD_SUBMAKE_JOBS) PROFILE=loongarch-qemu perf-tool

perf-native-tests: tools/perf/f7ly_perf.cc tools/perf/f7ly_perf_native_test.cc \
                   kernel/libs/perf_diag_algorithms.hh
	@mkdir -p $(BUILD_ROOT)/perf-tools
	g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-function -Ikernel \
		tools/perf/f7ly_perf_native_test.cc \
		-o $(BUILD_ROOT)/perf-tools/f7ly-perf-native-test
	@$(BUILD_ROOT)/perf-tools/f7ly-perf-native-test

$(PERF_TOOL_BIN): $(PERF_TOOL_SRC)
	@mkdir -p $(dir $@)
	$(CROSS_COMPILE)g++ -std=c++17 -O2 -Wall -Wextra -Werror -static $< -o $@
