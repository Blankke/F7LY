编译eastl，需要
```makefile
CXXFLAGS := $(CFLAGS) -std=c++17 \
            -DEA_PLATFORM_LINUX -DEA_PLATFORM_POSIX \
            -DEA_PROCESSOR_RISCV -DEA_ENDIAN_LITTLE=1 \
            -Wno-deprecated-declarations -Wno-strict-aliasing \
            -fno-exceptions -fno-rtti
```