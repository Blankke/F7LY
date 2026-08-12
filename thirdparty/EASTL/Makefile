CROSS_COMPILE ?= 
CXX = $(CROSS_COMPILE)g++
AR = $(CROSS_COMPILE)ar

# EASTL 的所有输出都必须位于顶层分配的画像构建目录。缺少该变量时直接
# 失败，避免 standalone clean 把空路径解释为 /thirdparty/EASTL。
ifeq ($(strip $(BUILDPATH)),)
$(error 缺少 BUILDPATH，请通过 F7LY 顶层 Makefile 构建 EASTL)
endif

# 顶层构建显式传入完整内核 C++ flags，确保 EASTL 与其余内核对象使用
# 完全相同的 ISA、soft-float ABI 和 freestanding 约束。
KERNEL_CXXFLAGS ?= -ffreestanding
CXXFLAGS := $(KERNEL_CXXFLAGS)

CXXFLAGS += -I include
CXXFLAGS += -I test/packages/EABase/include/Common 
CXXFLAGS += -I ../../kernel/
CXXFLAGS += -I ../../kernel/tm/
CXXFLAGS += -Wno-deprecated-declarations 

#cc_src = $(filter-out source/thread_support.cpp, $(wildcard source/*.cpp))
cc_src = $(wildcard source/*.cpp)
cc_obj = $(patsubst %.cpp,$(BUILDPATH)/thirdparty/EASTL/%.o,$(cc_src))
cc_dep = $(cc_obj:.o=.d)



.PHONY: all clean initdir test

all: initdir $(BUILDPATH)/thirdparty/EASTL/libeastl.a
	@echo "libeastl.a is created..."
	@echo "libeastl.a is created in $(BUILDPATH)/thirdparty/EASTL/"

initdir:
	mkdir -p $(BUILDPATH)/thirdparty/EASTL
	mkdir -p $(BUILDPATH)/thirdparty/EASTL/source

$(BUILDPATH)/thirdparty/EASTL/libeastl.a: $(cc_obj) source | initdir
	# 重建 archive，确保已删除的源码不会以旧 member 继续残留。
	rm -f $@
	$(AR) rcs $@ $(cc_obj)

$(BUILDPATH)/thirdparty/EASTL/source/%.o: source/%.cpp Makefile $(KERNEL_CONFIG_STAMP) | initdir
	$(CXX) -c $(CXXFLAGS) -MMD -MP \
		-MF $(BUILDPATH)/thirdparty/EASTL/source/$*.d -o $@ $<

# 让头文件变化能够准确触发对应 EASTL 对象重编，顶层无需猜测依赖关系。
-include $(cc_dep)
clean:
	rm -rf $(BUILDPATH)/thirdparty/EASTL

test:	
	@echo $(cc_src)
	@echo $(cc_obj)
