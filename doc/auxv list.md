| `a_type` 值 | 宏定义                | 说明                                    |
| ---------- | ------------------ | ------------------------------------- |
| `0`        | `AT_NULL`          | 结束标志，表明 auxv 的终止                      |
| `1`        | `AT_IGNORE`        | 忽略（无意义），可跳过                           |
| `2`        | `AT_EXECFD`        | 可执行文件的文件描述符（用于 `execveat()`）          |
| `3`        | `AT_PHDR`          | 程序头表（Program Header Table）的地址（虚拟地址）   |
| `4`        | `AT_PHENT`         | 每个 Program Header 的大小（字节）             |
| `5`        | `AT_PHNUM`         | Program Header 的数量                    |
| `6`        | `AT_PAGESZ`        | 内存页大小（通常是 4096）                       |
| `7`        | `AT_BASE`          | 动态链接器（ld.so）的加载地址（动态链接程序的基址）          |
| `8`        | `AT_FLAGS`         | 特殊标志位，通常为 0                           |
| `9`        | `AT_ENTRY`         | 程序的入口点虚拟地址（ELF Header 的 e\_entry 字段）  |
| `10`       | `AT_NOTELF`        | 若非 ELF 文件则为非 0（已废弃）                   |
| `11`       | `AT_UID`           | 实 UID                                 |
| `12`       | `AT_EUID`          | 有效 UID                                |
| `13`       | `AT_GID`           | 实 GID                                 |
| `14`       | `AT_EGID`          | 有效 GID                                |
| `15`       | `AT_PLATFORM`      | 描述平台的字符串地址（如 `"x86_64"` 的地址）          |
| `16`       | `AT_HWCAP`         | 硬件能力位（CPU 支持的特性），与 `/proc/cpuinfo` 类似 |
| `17`       | `AT_CLKTCK`        | `times()` 系统调用的时钟滴答率（常为 100）          |
| `18`       | `AT_FPUCW`         | FPU 控制字（已废弃）                          |
| `19`       | `AT_DCACHEBSIZE`   | 数据缓存块大小（已废弃）                          |
| `20`       | `AT_ICACHEBSIZE`   | 指令缓存块大小（已废弃）                          |
| `21`       | `AT_UCACHEBSIZE`   | 通用缓存块大小（已废弃）                          |
| `22`       | `AT_IGNOREPPC`     | 忽略（PPC 架构特有）                          |
| `23`       | `AT_SECURE`        | 如果进程处于 secure 模式（setuid/setgid），则为 1  |
| `24`       | `AT_BASE_PLATFORM` | 备用平台字符串地址                             |
| `25`       | `AT_RANDOM`        | 指向 16 字节随机值的地址（供堆栈保护使用）               |
| `26`       | `AT_HWCAP2`        | 扩展硬件能力标志（如 x86 的 AVX512）              |
| `31`       | `AT_EXECFN`        | 执行文件路径的字符串地址（例如 `/bin/ls`）            |
| `32`       | `AT_SYSINFO`       | 用于 vDSO 的 syscall helper（x86 已废弃）     |
| `33`       | `AT_SYSINFO_EHDR`  | vDSO 的 ELF Header 地址（用于高效系统调用）        |
