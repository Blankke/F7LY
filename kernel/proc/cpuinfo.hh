#include <EASTL/string.h>

struct cpuinfo
{
    eastl::string processor       = "0";
    eastl::string vendor_id       = "GenuineIntel";
    eastl::string cpu_family      = "6";
    eastl::string model           = "186";
    eastl::string model_name      = "13th Gen Intel(R) Core(TM) i9-13900H";
    eastl::string stepping        = "2";
    eastl::string microcode       = "0xffffffff";
    eastl::string cpu_MHz         = "2995.197";
    eastl::string cache_size      = "24576 KB";
    eastl::string physical_id     = "0";
    eastl::string siblings        = "20";
    eastl::string core_id         = "0";
    eastl::string cpu_cores       = "10";
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

eastl::string get_cpuinfo()
{
    cpuinfo info;
    eastl::string result;

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

    return result;
}
