CXXFLAGS += -I include 
CXXFLAGS += -I test/packages/EABase/include/Common
CXXFLAGS += -I ../kernel/
CXXFLAGS += -I ../kernel/libs/
CXXFLAGS += -Wno-deprecated-declarations

# 修改这里：去掉 source/ 前缀
cc_src = $(notdir $(wildcard source/*.cpp))
cc_obj = $(patsubst %.cpp,$(BUILDPATH)/EASTL/%.o,$(cc_src))

.PHONY: all clean initdir

all: $(BUILDPATH)/EASTL/libeastl.a
	@echo "libeastl.a is created..."

initdir:
	@mkdir -p $(BUILDPATH)/EASTL

$(BUILDPATH)/EASTL/libeastl.a: $(cc_obj)
	$(AR) rcs $@ $^

$(BUILDPATH)/EASTL/%.o: source/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -MM -MF $(patsubst %.o,%.d,$@) -o $@ $<

clean:
	rm -rf $(BUILDPATH)/EASTL