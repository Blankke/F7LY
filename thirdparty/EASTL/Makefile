CROSS_COMPILE ?= 
CXX = $(CROSS_COMPILE)g++
AR = $(CROSS_COMPILE)ar

# 顶层构建显式传入完整内核 C++ flags，确保 EASTL 与其余内核对象使用
# 完全相同的 ISA、soft-float ABI 和 freestanding 约束。
KERNEL_CXXFLAGS ?= -nostdlib
CXXFLAGS := $(KERNEL_CXXFLAGS)

CXXFLAGS += -I include -nostdlib
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

$(BUILDPATH)/thirdparty/EASTL/libeastl.a: $(cc_obj) | initdir
	$(AR) rcs $@ $^

$(BUILDPATH)/thirdparty/EASTL/source/%.o: source/%.cpp $(KERNEL_CONFIG_STAMP) | initdir
	$(CXX) -c $(CXXFLAGS) -MMD -MF $(BUILDPATH)/thirdparty/EASTL/source/$*.d -o $@ $<
clean:
	rm -rf $(BUILDPATH)/thirdparty/EASTL

test:	
	@echo $(cc_src)
	@echo $(cc_obj)

-include $(cc_dep)
