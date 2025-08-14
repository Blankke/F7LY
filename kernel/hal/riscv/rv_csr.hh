#pragma once
#include "../types.hh"

namespace riscv{
/**
 * @brief 定义 RISC-V 控制状态寄存器 (CSR) 读写操作的宏
 * 
 * 此宏为每个 CSR 寄存器创建两个内联函数:
 * 1. r_name() - 读取 CSR 寄存器的值
 * 2. w_name() - 写入值到 CSR 寄存器
 * 
 * 使用 GCC 内联汇编实现 CSR 读写操作:
 * - csrr  指令用于读取 CSR 寄存器
 * - csrw  指令用于写入 CSR 寄存器
 * 
 * @param name CSR 寄存器的名称 (如 mstatus, sstatus, satp 等)
 * 
 * 调用示例:
 * - DECLARE_CSR_RW(satp) 会创建 r_satp() 和 w_satp() 两个函数
 * - 使用 r_satp() 读取 satp 寄存器的值
 * - 使用 w_satp(x) 将值 x 写入 satp 寄存器
 */
#define DECLARE_CSR_RW(name) \
    static inline uint64 r_##name() { \
        uint64 x; \
        asm volatile("csrr %0, " #name : "=r" (x)); \
        return x; \
    } \
    static inline void w_##name(uint64 x) { \
        asm volatile("csrw " #name ", %0" : : "r" (x)); \
    }

/**
 * @brief RISC-V 内核常量参数
 */
constexpr uint entry_stack_size = 4096; // 每个硬件线程的初始栈大小

/**
 * @brief 初始化 RISC-V 相关功能
 */
void riscv_init();

/**
 * @brief 页表相关常量和标志位
 */
enum PgEnum
{
    pg_flags_mask = 0x3FF,       // 页表项标志位掩码 (低10位)
};

/**
 * @brief SV39 分页模式下物理地址到页表项映射的位移和掩码定义
 */
constexpr uint64 pa_mask = 0x0000FFFFFFFFFFFFULL;  // 44位物理地址掩码
constexpr uint64 pa_shift = 12;                    // 物理页号偏移 (4KB对齐)

/**
 * @brief 虚拟地址到物理地址的转换
 * @param virt 虚拟地址
 * @return 对应的物理地址
 */
constexpr uint64 virt_to_phy_address(uint64 virt) { 
    return virt & pa_mask; // RISC-V 中简化处理，如有需要可以根据MMU设置修改
}

/**
 * @brief 页表项字段定义宏
 */
#define _build_pte_bit_(name,mask,shift) \
	pte_##name##_s = shift, \
	pte_##name##_m = mask << pte_##name##_s,

/**
 * @brief 页表项各个位域的枚举
 */
enum PteEnum
{
    _build_pte_bit_(valid, 0x1, 0)  // valid
    _build_pte_bit_(readable, 0x1, 1)  // readable
    _build_pte_bit_(writable, 0x1, 2)  // writable
    _build_pte_bit_(executable, 0x1, 3)  // executable
    _build_pte_bit_(user, 0x1, 4)  // user can access
    _build_pte_bit_(global, 0x1, 5)  // global mapping (G)
    _build_pte_bit_(accessed, 0x1, 6)  // accessed (A)
    _build_pte_bit_(dirty, 0x1, 7)  // dirty (D)

    // _build_pte_bit_(valid, 0x1, 0)      // V: 有效位 (1 = 该PTE有效)
    // _build_pte_bit_(dirty, 0x1, 1)      // D: 脏位 (1 = 该页被修改过)
    // _build_pte_bit_(plv, 0x3, 2)      // D: 脏位 (1 = 该页被修改过)
    // _build_pte_bit_(user,3L,2)
    // _build_pte_bit_(mat,1L,4)   //Memory Access Type
    // _build_pte_bit_(phy,1L,7) //Physical page exists
    // _build_pte_bit_(write,1,8)  //W: 可写位 (1 = 该页可写)
    // _build_pte_bit_(NX,1UL,62) // NX: 不可执行位 (1 = 该页不可执行)
    // _build_pte_bit_(NR,1L,62) // NR: 不可读位 (1 = 该页不可读)
    // _build_pte_bit_(RPLV,1L,63) //restricted privilege level enable
    // _build_pte_bit_(read,0,0) //R
    // _build_pte_bit_(X,0,0) //X
    

    // // 物理页号和标志掩码
    // pte_ppn_s = 10,                     // PPN: 物理页号起始位
    // pte_ppn_m = 0xFFFFFFFFFFFULL << pte_ppn_s, // 44位物理页号掩码
    
    // // 标志位整体掩码
    // pte_flags_m = pte_valid_m | pte_readable_m | pte_writable_m | pte_executable_m |
    //              pte_user_m | pte_global_m | pte_accessed_m | pte_dirty_m | pte_rsw_m,
};
#undef _build_pte_bit_

/**
 * @brief 内存访问类型枚举
 */
enum MatEnum
{
    mat_normal = 0x0,         // 普通可缓存内存
    mat_io = 0x1,             // 设备I/O内存
    mat_uncached = 0x2,       // 非缓存内存
    mat_reserved = 0x3        // 保留
};

/**
 * @brief 特权级别枚举
 */
enum PlvEnum
{
    plv_machine = 3,           // M模式 (Machine Mode)
    plv_super = 1,             // S模式 (Supervisor Mode)
    plv_user = 0               // U模式 (User Mode)
};

};

#pragma once


namespace riscv {

    // 声明 mstatus 寄存器读写函数
    #ifndef DECLARE_CSR_MSTATUS
    #define DECLARE_CSR_MSTATUS
    DECLARE_CSR_RW(mstatus)
    #endif

    // 声明 mepc (M 模式异常程序计数器) 寄存器读写函数
    #ifndef DECLARE_CSR_MEPC
    #define DECLARE_CSR_MEPC
    DECLARE_CSR_RW(mepc)
    #endif

    // 声明 sstatus 寄存器读写函数
    #ifndef DECLARE_CSR_SSTATUS
    #define DECLARE_CSR_SSTATUS
    DECLARE_CSR_RW(sstatus)
    #endif

    // 声明 sip (S 模式中断等待) 寄存器读写函数
    #ifndef DECLARE_CSR_SIP
    #define DECLARE_CSR_SIP
    DECLARE_CSR_RW(sip)
    #endif

    // 声明 sie 寄存器读写函数
    #ifndef DECLARE_CSR_SIE
    #define DECLARE_CSR_SIE
    DECLARE_CSR_RW(sie)
    #endif

    // 声明各种 CSR 寄存器的读写函数
    #ifndef DECLARE_CSR_MIE
    #define DECLARE_CSR_MIE
    DECLARE_CSR_RW(mie)         // M 模式中断使能寄存器
    #endif
    
    #ifndef DECLARE_CSR_SEPC
    #define DECLARE_CSR_SEPC
    DECLARE_CSR_RW(sepc)        // S 模式异常程序计数器
    #endif
    
    #ifndef DECLARE_CSR_MEDELEG
    #define DECLARE_CSR_MEDELEG
    DECLARE_CSR_RW(medeleg)     // M 模式异常委托寄存器
    #endif
    
    #ifndef DECLARE_CSR_MIDELEG
    #define DECLARE_CSR_MIDELEG
    DECLARE_CSR_RW(mideleg)     // M 模式中断委托寄存器
    #endif
    
    #ifndef DECLARE_CSR_MHARTID
    #define DECLARE_CSR_MHARTID
    DECLARE_CSR_RW(mhartid)     // M 模式硬件线程ID寄存器
    #endif
    
    // mstatus已在前面定义
    
    #ifndef DECLARE_CSR_MCAUSE
    #define DECLARE_CSR_MCAUSE
    DECLARE_CSR_RW(mcause)      // M 模式异常原因寄存器 
    #endif
    
    #ifndef DECLARE_CSR_MIP
    #define DECLARE_CSR_MIP
    DECLARE_CSR_RW(mip)       // M 模式异常入口地址寄存器
    #endif
    
    #ifndef DECLARE_CSR_STVEC
    #define DECLARE_CSR_STVEC
    DECLARE_CSR_RW(stvec)       // S 模式陷入向量基址寄存器
    #endif
    
    #ifndef DECLARE_CSR_MTVEC
    #define DECLARE_CSR_MTVEC
    DECLARE_CSR_RW(mtvec)       // M 模式陷入向量基址寄存器
    #endif
    
    #ifndef DECLARE_CSR_PMPCFG0
    #define DECLARE_CSR_PMPCFG0
    DECLARE_CSR_RW(pmpcfg0)     // 物理内存保护配置寄存器0
    #endif
    
    #ifndef DECLARE_CSR_PMPADDR0
    #define DECLARE_CSR_PMPADDR0
    DECLARE_CSR_RW(pmpaddr0)    // 物理内存保护地址寄存器0
    #endif
    
    #ifndef DECLARE_CSR_SATP
    #define DECLARE_CSR_SATP
    DECLARE_CSR_RW(satp)        // S 模式地址转换和保护寄存器
    #endif
    
    #ifndef DECLARE_CSR_SSCRATCH
    #define DECLARE_CSR_SSCRATCH
    DECLARE_CSR_RW(sscratch)    // S 模式暂存寄存器
    #endif
    
    #ifndef DECLARE_CSR_MSCRATCH
    #define DECLARE_CSR_MSCRATCH
    DECLARE_CSR_RW(mscratch)    // M 模式暂存寄存器
    #endif
    
    #ifndef DECLARE_CSR_SCAUSE
    #define DECLARE_CSR_SCAUSE
    DECLARE_CSR_RW(scause)      // S 模式陷入原因寄存器
    #endif
    
    #ifndef DECLARE_CSR_STVAL
    #define DECLARE_CSR_STVAL
    DECLARE_CSR_RW(stval)       // S 模式陷入值寄存器
    #endif
    
    #ifndef DECLARE_CSR_MCOUNTEREN
    #define DECLARE_CSR_MCOUNTEREN
    DECLARE_CSR_RW(mcounteren)  // M 模式计数器使能寄存器
    #endif
    
    #ifndef DECLARE_CSR_TIME
    #define DECLARE_CSR_TIME
    DECLARE_CSR_RW(time)        // 时间计数器
    #endif
    
    #ifndef DECLARE_CSR_SP
    #define DECLARE_CSR_SP
    DECLARE_CSR_RW(sp)          // 栈指针
    #endif
    
    #ifndef DECLARE_CSR_TP
    #define DECLARE_CSR_TP
    static inline uint64
    r_tp()
    {
      uint64 x;
      asm volatile("mv %0, tp" : "=r" (x) );
      return x;
    }
    
    static inline void 
    w_tp(uint64 x)
    {
      asm volatile("mv tp, %0" : : "r" (x));
    }
    #endif
    
    #ifndef DECLARE_CSR_RA
    #define DECLARE_CSR_RA
    static inline uint64
    r_ra()
    {
      uint64 x;
      asm volatile("mv %0, ra" : "=r" (x) );
      return x;
    }
    #endif

/**
 * @brief CSR 寄存器地址和操作
 */
namespace csr
{
    /**
     * @brief CSR 寄存器地址枚举
     */
    enum CsrAddr
    {
        // M模式 CSR 寄存器
        mhartid     = 0xF14,   // Hart (Hardware Thread) ID 寄存器
        mstatus     = 0x300,   // 机器模式状态寄存器
        mtvec       = 0x305,   // 机器模式异常入口地址
        mepc        = 0x341,   // 机器模式异常返回地址
        mcause      = 0x342,   // 机器模式异常原因
        mtval       = 0x343,   // 机器模式异常值
        mie         = 0x304,   // 机器模式中断启用
        mip         = 0x344,   // 机器模式中断等待
        medeleg     = 0x302,   // 机器模式异常委托
        mideleg     = 0x303,   // 机器模式中断委托
        mcounteren  = 0x306,   // 机器模式计数器使能
        mscratch    = 0x340,   // 机器模式暂存寄存器
        
        // S模式 CSR 寄存器
        sstatus     = 0x100,   // 监管者模式状态寄存器
        stvec       = 0x105,   // 监管者模式异常入口地址
        sepc        = 0x141,   // 监管者模式异常返回地址
        scause      = 0x142,   // 监管者模式异常原因
        stval       = 0x143,   // 监管者模式异常值
        sie         = 0x104,   // 监管者模式中断启用
        sip         = 0x144,   // 监管者模式中断等待
        satp        = 0x180,   // 监管者模式地址转换和保护
        sscratch    = 0x140,   // 监管者模式暂存寄存器
        scounteren  = 0x106,   // 监管者模式计数器使能
        
        // 时间和性能计数器
        time        = 0xC01,   // 时间计数器
        cycle       = 0xC00,   // 周期计数器
        instret     = 0xC02    // 指令计数器
    };

    /**
     * @brief M模式状态寄存器(mstatus)位域定义
     */
    enum Mstatus
    {
        // S模式中断使能
        mstatus_sie_s = 1,  
        mstatus_sie_m = 0x1ULL << mstatus_sie_s,
        
        // M模式中断使能
        mstatus_mie_s = 3,  
        mstatus_mie_m = 0x1ULL << mstatus_mie_s,
        
        // S模式上一次中断使能
        mstatus_spie_s = 5, 
        mstatus_spie_m = 0x1ULL << mstatus_spie_s,
        
        // M模式上一次中断使能
        mstatus_mpie_s = 7, 
        mstatus_mpie_m = 0x1ULL << mstatus_mpie_s,
        
        // S模式上一次特权级
        mstatus_spp_s = 8,  
        mstatus_spp_m = 0x1ULL << mstatus_spp_s,
        
        // M模式上一次特权级
        mstatus_mpp_s = 11, 
        mstatus_mpp_mmode = 0x3ULL << mstatus_mpp_s,
        mstatus_mpp_smode = 0x1ULL << mstatus_mpp_s,
        mstatus_mpp_umode = 0x0ULL << mstatus_mpp_s,
        mstatus_mpp_mask = 0x3ULL << mstatus_mpp_s,

        // 浮点单元状态
        mstatus_fs_s = 13,  
        mstatus_fs_m = 0x3ULL << mstatus_fs_s,
        
        // 扩展单元状态
        mstatus_xs_s = 15,  
        mstatus_xs_m = 0x3ULL << mstatus_xs_s,
        
        // 内存特权访问模式
        mstatus_mprv_s = 17, 
        mstatus_mprv_m = 0x1ULL << mstatus_mprv_s,
        
        // S模式用户内存访问
        mstatus_sum_s = 18, 
        mstatus_sum_m = 0x1ULL << mstatus_sum_s,
        
        // 使可执行页可读
        mstatus_mxr_s = 19, 
        mstatus_mxr_m = 0x1ULL << mstatus_mxr_s,
        
        // 陷入虚拟内存
        mstatus_tvm_s = 20, 
        mstatus_tvm_m = 0x1ULL << mstatus_tvm_s,
        
        // 超时等待陷入
        mstatus_tw_s = 21,  
        mstatus_tw_m = 0x1ULL << mstatus_tw_s,
        
        // 陷入SRET指令
        mstatus_tsr_s = 22, 
        mstatus_tsr_m = 0x1ULL << mstatus_tsr_s,
        
        // 状态脏位
        mstatus_sd_s = 63,  
        mstatus_sd_m = 0x1ULL << mstatus_sd_s
    };

    /**
     * @brief S模式状态寄存器(sstatus)位域定义
     */
    enum Sstatus
    {
        // S模式中断使能
        sstatus_sie_s = 1,  
        sstatus_sie_m = 0x1ULL << sstatus_sie_s,
        
        // S模式上一次中断使能
        sstatus_spie_s = 5, 
        sstatus_spie_m = 0x1ULL << sstatus_spie_s,
        
        // S模式上一次特权级
        sstatus_spp_s = 8,  
        sstatus_spp_m = 0x1ULL << sstatus_spp_s,
        
        // S模式用户内存访问
        sstatus_sum_s = 18, 
        sstatus_sum_m = 0x1ULL << sstatus_sum_s,
        
        // 使可执行页可读
        sstatus_mxr_s = 19, 
        sstatus_mxr_m = 0x1ULL << sstatus_mxr_s,
        
        // 状态脏位
        sstatus_sd_s = 63,  
        sstatus_sd_m = 0x1ULL << sstatus_sd_s
    };

    /**
     * @brief 中断控制寄存器(sie/mie)位域定义
     */
    enum InterruptEnable
    {
        // 用户软件中断使能
        ie_usie_s = 0,  
        ie_usie_m = 0x1ULL << ie_usie_s,
        
        // 监管者软件中断使能
        ie_ssie_s = 1,  
        ie_ssie_m = 0x1ULL << ie_ssie_s,
        
        // 机器软件中断使能
        ie_msie_s = 3,  
        ie_msie_m = 0x1ULL << ie_msie_s,
        
        // 用户定时器中断使能
        ie_utie_s = 4,  
        ie_utie_m = 0x1ULL << ie_utie_s,
        
        // 监管者定时器中断使能
        ie_stie_s = 5,  
        ie_stie_m = 0x1ULL << ie_stie_s,
        
        // 机器定时器中断使能
        ie_mtie_s = 7,  
        ie_mtie_m = 0x1ULL << ie_mtie_s,
        
        // 用户外部中断使能
        ie_ueie_s = 8,  
        ie_ueie_m = 0x1ULL << ie_ueie_s,
        
        // 监管者外部中断使能
        ie_seie_s = 9,  
        ie_seie_m = 0x1ULL << ie_seie_s,
        
        // 机器外部中断使能
        ie_meie_s = 11, 
        ie_meie_m = 0x1ULL << ie_meie_s
    };

    /**
     * @brief S模式中断等待寄存器(sip)位域定义
     */
    enum Sip
    {
        // 用户软件中断等待
        sip_usip_s = 0,  
        sip_usip_m = 0x1ULL << sip_usip_s,
        
        // 监管者软件中断等待
        sip_ssip_s = 1,  
        sip_ssip_m = 0x1ULL << sip_ssip_s,
        
        // 用户定时器中断等待
        sip_utip_s = 4,  
        sip_utip_m = 0x1ULL << sip_utip_s,
        
        // 监管者定时器中断等待
        sip_stip_s = 5,  
        sip_stip_m = 0x1ULL << sip_stip_s,
        
        // 用户外部中断等待
        sip_ueip_s = 8,  
        sip_ueip_m = 0x1ULL << sip_ueip_s,
        
        // 监管者外部中断等待
        sip_seip_s = 9,  
        sip_seip_m = 0x1ULL << sip_seip_s
    };

    /**
     * @brief M模式中断等待寄存器(mip)位域定义
     */
    enum Mip
    {
        // 用户软件中断等待
        mip_usip_s = 0,  
        mip_usip_m = 0x1ULL << mip_usip_s,
        
        // 监管者软件中断等待
        mip_ssip_s = 1,  
        mip_ssip_m = 0x1ULL << mip_ssip_s,
        
        // 机器软件中断等待
        mip_msip_s = 3,  
        mip_msip_m = 0x1ULL << mip_msip_s,
        
        // 用户定时器中断等待
        mip_utip_s = 4,  
        mip_utip_m = 0x1ULL << mip_utip_s,
        
        // 监管者定时器中断等待
        mip_stip_s = 5,  
        mip_stip_m = 0x1ULL << mip_stip_s,
        
        // 机器定时器中断等待
        mip_mtip_s = 7,  
        mip_mtip_m = 0x1ULL << mip_mtip_s,
        
        // 用户外部中断等待
        mip_ueip_s = 8,  
        mip_ueip_m = 0x1ULL << mip_ueip_s,
        
        // 监管者外部中断等待
        mip_seip_s = 9,  
        mip_seip_m = 0x1ULL << mip_seip_s,
        
        // 机器外部中断等待
        mip_meip_s = 11, 
        mip_meip_m = 0x1ULL << mip_meip_s
    };

    enum Sie
    {
        sie_seie = 1<<9,   // S模式外部中断使能
        sie_stie = 1<<5,   // S模式定时器中断使能
        sie_ssie = 1<<1    // S模式软件中断使能
    };

    /**
     * @brief 异常代码枚举
     */
    enum Ecode
    {
        // 中断 (高位为1)
        // 用户软件中断
        // 监管者软件中断
        ecode_s_software_intr = 0x1 | (1ULL << 63),
        
        // 机器软件中断
        ecode_m_software_intr = 0x3 | (1ULL << 63),
         
        // 监管者时钟中断
        ecode_s_timer_intr = 0x5 | (1ULL << 63),
        
        // 机器时钟中断
        ecode_m_timer_intr = 0x7 | (1ULL << 63),
                
        // 监管者外部中断
        ecode_s_external_intr = 0x9 | (1ULL << 63),
        
        // 机器外部中断
        ecode_m_external_intr = 0xB | (1ULL << 63),
        
        // 异常 (高位为0)
        // 指令地址未对齐
        ecode_instr_addr_misaligned = 0x0,
        
        // 指令访问错误
        ecode_instr_access_fault = 0x1,
        
        // 非法指令
        ecode_illegal_instr = 0x2,
        
        // 断点
        ecode_breakpoint = 0x3,
        
        // 加载地址未对齐
        ecode_load_addr_misaligned = 0x4,
        
        // 加载访问错误
        ecode_load_access_fault = 0x5,
        
        // 存储地址未对齐
        ecode_store_addr_misaligned = 0x6,
        
        // 存储访问错误
        ecode_store_access_fault = 0x7,
        
        // 用户态系统调用
        ecode_ecall_from_u = 0x8,
        
        // 监管者态系统调用
        ecode_ecall_from_s = 0x9,
        
        // 机器态系统调用
        ecode_ecall_from_m = 0xB,
        
        // 指令页错误
        ecode_instr_page_fault = 0xC,
        
        // 加载页错误
        ecode_load_page_fault = 0xD,
        
        // 存储页错误
        ecode_store_page_fault = 0xF
    };

    /**
     * @brief SATP寄存器位域定义
     */
    enum Satp
    {
        satp_mode_s = 60,                     // 分页模式位字段
        satp_mode_m = 0xFUL << satp_mode_s,   // 分页模式掩码
        satp_asid_s = 44,                     // ASID字段
        satp_asid_m = 0xFFFFUL << satp_asid_s, // ASID掩码
        satp_ppn_s = 0,                       // 物理页号字段
        satp_ppn_m = 0xFFFFFFFFFFFUL << satp_ppn_s, // 物理页号掩码
        
        // 分页模式值
        satp_mode_bare = 0UL << satp_mode_s,  // 无分页 (裸机)
        satp_mode_sv39 = 8UL << satp_mode_s,  // Sv39 分页 (39位虚拟地址)
        satp_mode_sv48 = 9UL << satp_mode_s,  // Sv48 分页 (48位虚拟地址)
        satp_mode_sv57 = 10UL << satp_mode_s  // Sv57 分页 (57位虚拟地址)
    };

    /**
     * @brief 读取CSR寄存器
     * @param _csr CSR寄存器地址
     * @return 寄存器值
     */
    static inline uint64 _read_csr_(CsrAddr _csr)
    {
        uint64 value;
        switch (_csr) {
            case mhartid:     value = r_mhartid(); break;
            case mstatus:     value = r_mstatus(); break;
            case mtvec:       value = r_mtvec(); break;
            case mepc:        value = r_mepc(); break;
            case mcause:      value = r_mcause(); break;
            case mie:         value = r_mie(); break;
            case mip:         value = r_mip(); break;
            case medeleg:     value = r_medeleg(); break;
            case mideleg:     value = r_mideleg(); break;
            case mcounteren:  value = r_mcounteren(); break;
            case mscratch:    value = r_mscratch(); break;
            case sstatus:     value = r_sstatus(); break;
            case stvec:       value = r_stvec(); break;
            case sepc:        value = r_sepc(); break;
            case scause:      value = r_scause(); break;
            case stval:       value = r_stval(); break;
            case sie:         value = r_sie(); break;
            case sip:         value = r_sip(); break;
            case satp:        value = r_satp(); break;
            case sscratch:    value = r_sscratch(); break;
            default:          value = 0;  // 未知CSR
        }
        return value;
    }

    /**
     * @brief 写入CSR寄存器
     * @param _csr CSR寄存器地址
     * @param _data 要写入的值
     */
    static inline void _write_csr_(CsrAddr _csr, uint64 _data)
    {
        switch (_csr) {
            case mstatus:     w_mstatus(_data); break;
            case mtvec:       w_mtvec(_data); break;
            case mepc:        w_mepc(_data); break;
            case mie:         w_mie(_data); break;
            case medeleg:     w_medeleg(_data); break;
            case mideleg:     w_mideleg(_data); break;
            case mcounteren:  w_mcounteren(_data); break;
            case mscratch:    w_mscratch(_data); break;
            case sstatus:     w_sstatus(_data); break;
            case stvec:       w_stvec(_data); break;
            case sepc:        w_sepc(_data); break;
            case sie:         w_sie(_data); break;
            case satp:        w_satp(_data); break;
            case sscratch:    w_sscratch(_data); break;
            default:          break;  // 未知CSR或不可写CSR
        }
    }

} // namespace csr

} // namespace riscv