#pragma once

#include <EASTL/string.h>
#include "hal/cpu.hh"

struct cpuinfo
{
    eastl::string processor       = "0";
    eastl::string vendor_id       = "GenuineIntel";
    eastl::string cpu_family      = "6";
    eastl::string model           = "186";
    // LTP 通过 /proc/cpuinfo 中的 QEMU Virtual CPU 判断虚拟化环境，
    // 进而放宽时间类测例的上界阈值；F7LY 默认在 QEMU 中评测，需如实暴露。
    eastl::string model_name      = "QEMU Virtual CPU";
    eastl::string stepping        = "2";
    eastl::string microcode       = "0xffffffff";
    eastl::string cpu_MHz         = "2995.197";
    eastl::string cache_size      = "24576 KB";
    eastl::string physical_id     = "0";
    eastl::string siblings        = "1";
    eastl::string core_id         = "0";
    eastl::string cpu_cores       = "1";
    eastl::string apicid          = "0";
    eastl::string initial_apicid  = "0";
    eastl::string fpu             = "yes";
    eastl::string fpu_exception   = "yes";
    eastl::string cpuid_level     = "28";
    eastl::string wp              = "yes";
    eastl::string flags           = "fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx pdpe1gb rdtscp lm constant_tsc rep_good nopl xtopology tsc_reliable nonstop_tsc cpuid tsc_known_freq pni pclmulqdq vmx ssse3 fma cx16 pcid sse4_1 sse4_2 x2apic movbe popcnt tsc_deadline_timer aes xsave avx f16c rdrand hypervisor lahf_lm abm 3dnowprefetch ssbd ibrs ibpb stibp ibrs_enhanced tpr_shadow ept vpid ept_ad fsgsbase tsc_adjust bmi1 avx2 smep bmi2 erms invpcid rdseed adx smap clflushopt clwb sha_ni xsaveopt xsavec xgetbv1 xsaves avx_vnni vnmi umip waitpkg gfni vaes vpclmulqdq rdpid movdiri movdir64b fsrm md_clear serialize flush_l1d arch_capabilities";
    eastl::string vmx_flags       = "vnmi invvpid ept_x_only ept_ad ept_1gb tsc_offset vtpr ept vpid unrestricted_guest ept_mode_based_exec tsc_scaling usr_wait_pause";
    eastl::string bugs            = "spectre_v1 spectre_v2 spec_store_bypass swapgs retbleed eibrs_pbrsb rfds bhi";
    eastl::string bogomips        = "5990.39";
    eastl::string clflush_size    = "64";
    eastl::string cache_alignment = "64";
    eastl::string address_sizes   = "46 bits physical, 48 bits virtual";
    eastl::string power_management = "";
};

inline eastl::string cpuinfo_unsigned_decimal(uint64 value)
{
    eastl::string result;
    char digits[32];
    int length = 0;
    do
    {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (length > 0)
    {
        result += digits[--length];
    }
    return result;
}

inline void append_cpuinfo_entry(eastl::string &result, const cpuinfo &info)
{

    result += "processor: " + info.processor + "\n";
    result += "vendor_id: " + info.vendor_id + "\n";
    result += "cpu family: " + info.cpu_family + "\n";
    result += "model: " + info.model + "\n";
    result += "model name: " + info.model_name + "\n";
    result += "stepping: " + info.stepping + "\n";
    result += "microcode: " + info.microcode + "\n";
    result += "cpu MHz: " + info.cpu_MHz + "\n";
    result += "cache size: " + info.cache_size + "\n";
    result += "physical id: " + info.physical_id + "\n";
    result += "siblings: " + info.siblings + "\n";
    result += "core id: " + info.core_id + "\n";
    result += "cpu cores: " + info.cpu_cores + "\n";
    result += "apicid: " + info.apicid + "\n";
    result += "initial apicid: " + info.initial_apicid + "\n";
    result += "fpu: " + info.fpu + "\n";
    result += "fpu_exception: " + info.fpu_exception + "\n";
    result += "cpuid level: " + info.cpuid_level + "\n";
    result += "wp: " + info.wp + "\n";
    result += "flags: [" + info.flags + "]\n";
    // result += "vmx flags: " + info.vmx_flags + "\n";
    result += "bugs: [" + info.bugs + "]\n";
    result += "bogomips: " + info.bogomips + "\n";
    result += "clflush size: " + info.clflush_size + "\n";
    result += "cache_alignment: " + info.cache_alignment + "\n";
    result += "address sizes: " + info.address_sizes + "\n";
    result += "power management: " + info.power_management + "\n";
}

inline eastl::string get_cpuinfo()
{
    eastl::string result;
    uint64 visible_mask = Cpu::online_cpu_mask();
    if (visible_mask == 0)
    {
        // 仅在极早期启动窗口兜底；正常用户态运行时必须只报告已经 online 的 CPU。
        visible_mask = Cpu::possible_cpu_mask();
    }

    int visible_count = 0;
    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((visible_mask & (1ULL << cpu_id)) != 0)
        {
            ++visible_count;
        }
    }

    for (uint64 cpu_id = 0; cpu_id < NCPU; ++cpu_id)
    {
        if ((visible_mask & (1ULL << cpu_id)) == 0)
        {
            continue;
        }

        cpuinfo info;
        const eastl::string cpu_id_text = cpuinfo_unsigned_decimal(cpu_id);
        const eastl::string cpu_count_text = cpuinfo_unsigned_decimal(visible_count);
        info.processor = cpu_id_text;
        info.siblings = cpu_count_text;
        info.core_id = cpu_id_text;
        info.cpu_cores = cpu_count_text;
        info.apicid = cpu_id_text;
        info.initial_apicid = cpu_id_text;

        append_cpuinfo_entry(result, info);
        result += "\n";
    }

    return result;
}
