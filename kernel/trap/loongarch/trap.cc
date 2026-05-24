#ifdef LOONGARCH
#include "types.hh"
#include "trap.hh"
#include "platform.hh"
#include "param.h"
// #include "plic.hh"
#include "mem.hh"
#include "mem/memlayout.hh"
#include "devs/console.hh"
#include "printer.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"
#include "proc/scheduler.hh"
#include "trap_func_wrapper.hh"
#include "extioi.hh"
#include "trap/loongarch/pci.h"
#include "apic.hh"
#include "syscall_handler.hh"
#include "cpu.hh"
#include "physical_memory_manager.hh"
#include "virtual_memory_manager.hh"
#include "heap_memory_manager.hh"
#include "vfs/file/normal_file.hh"
#include "devs/loongarch/disk_driver.hh"
#include "trap/interrupt_stats.hh"
#include "timer_interface.hh"
#include "proc/posix_timers.hh"
#include "asm.hh"
// in kernelvec.S, calls kerneltrap().
extern "C" void kernelvec();
extern "C" void uservec();
extern "C" void handle_tlbr();
extern "C" void handle_merr();
extern "C" void userret(uint64, uint64);
int mmap_handler(uint64 va, int cause);
// 创建一个静态对象
trap_manager trap_mgr;

namespace
{
  // LoongArch 异常信息拆分：一级编码在 ESTAT[21:16]，二级编码在 ESTAT[30:22]。
  // 之前把 ecode=8 直接当成缺页，会把 ADEM（访存地址错误）误送进 mmap 懒分配路径。
  inline uint32 loongarch_exception_code(uint32 estat)
  {
    return (estat & CSR_ESTAT_ECODE) >> 16;
  }

  inline uint32 loongarch_exception_subcode(uint32 estat)
  {
    return (estat >> 22) & 0x1ffU;
  }

  inline bool is_loongarch_page_fault_code(uint32 ecode)
  {
    return ecode >= 0x1 && ecode <= 0x7;
  }

  inline void loongarch_invalidate_user_tlb_page(uint64 va)
  {
    // LoongArch 一个普通 TLB 表项覆盖相邻两页，Linux 也会按 8KB 对齐做单页失效。
    // 这里对“PTE 已合法存在、只是 TLB 内部残着无效项”的场景做最小失效，
    // 避免把它误判成 mmap 懒分配失败后直接送 SIGSEGV。
    uint64 pair_base = va & ~((PGSIZE << 1) - 1);
    asm volatile("invtlb 0x6, $zero, %0" : : "r"(pair_base) : "memory");
  }

  inline void loongarch_ack_timer_interrupt()
  {
    // TICLR/TINTCLR 是“写 1 清中断”的专用寄存器，按 Linux 的做法直接写清除位即可。
    // 这里不要先读再 OR 回写：
    // 1. 该寄存器本身没有需要保留的状态位；
    // 2. 读值可能是未定义/瞬时值，反而容易把 timer pending 留到 userret 窗口里，
    //    让同一个时钟中断在刚返回用户态时又立刻打回来。
    w_csr_ticlr(CSR_TICLR_CLR);
  }

  inline bool loongarch_can_retry_present_user_fault(mem::Pte pte, uint32 ecode)
  {
    if (pte.is_null() || !pte.is_valid() || !pte.is_present() || pte.is_super_plv())
    {
      return false;
    }

    switch (ecode)
    {
    case 0x1: // load invalid
      return pte.is_readable();
    case 0x2: // store invalid
      return pte.is_writable();
    case 0x3: // fetch invalid
      return pte.is_executable();
    case 0x4: // modified fault
      return pte.is_writable();
    default:
      return false;
    }
  }

  inline bool is_entry_static_thread(proc::Pcb *p)
  {
    return p != nullptr && strncmp(p->_name, "entry-static.exe", sizeof("entry-static.exe") - 1) == 0;
  }

  struct LoongarchTlbProbe
  {
    uint32 idx = 0;
    uint64 tlbehi = 0;
    uint64 tlbelo0 = 0;
    uint64 tlbelo1 = 0;
    bool hit = false;
  };

  inline LoongarchTlbProbe probe_loongarch_tlb(uint64 va)
  {
    LoongarchTlbProbe probe{};
    uint64 old_tlbehi = 0;
    uint32 old_asid = 0;
    uint32 old_idx = 0;

    asm volatile("csrrd %0, %1" : "=r"(old_tlbehi) : "i"(LOONGARCH_CSR_TLBEHI));
    asm volatile("csrrd %0, %1" : "=r"(old_asid) : "i"(LOONGARCH_CSR_ASID));
    asm volatile("csrrd %0, %1" : "=r"(old_idx) : "i"(LOONGARCH_CSR_TLBIDX));

    // 仅用于调试：按当前 ASID + VA 做一次 tlbsrch/tlbrd，把硬件当前实际生效的
    // TLB 表项读出来。这样可以区分“页表里的 PTE 是对的”和“refill/TLB 里被装歪了”。
    asm volatile("csrwr %0, %1" : : "r"(va), "i"(LOONGARCH_CSR_TLBEHI));
    asm volatile("tlbsrch");
    asm volatile("csrrd %0, %1" : "=r"(probe.idx) : "i"(LOONGARCH_CSR_TLBIDX));

    if ((probe.idx & (1U << 31)) == 0)
    {
      probe.hit = true;
      asm volatile("tlbrd");
      asm volatile("csrrd %0, %1" : "=r"(probe.tlbehi) : "i"(LOONGARCH_CSR_TLBEHI));
      asm volatile("csrrd %0, %1" : "=r"(probe.tlbelo0) : "i"(LOONGARCH_CSR_TLBELO0));
      asm volatile("csrrd %0, %1" : "=r"(probe.tlbelo1) : "i"(LOONGARCH_CSR_TLBELO1));
    }

    asm volatile("csrwr %0, %1" : : "r"(old_tlbehi), "i"(LOONGARCH_CSR_TLBEHI));
    asm volatile("csrwr %0, %1" : : "r"(old_asid), "i"(LOONGARCH_CSR_ASID));
    asm volatile("csrwr %0, %1" : : "r"(old_idx), "i"(LOONGARCH_CSR_TLBIDX));
    return probe;
  }

  inline const char *find_mm_section_name(proc::ProcessMemoryManager *mm, uint64 va, int &section_index)
  {
    section_index = -1;
    if (mm == nullptr)
    {
      return nullptr;
    }

    for (int i = 0; i < mm->prog_section_count && i < proc::max_program_section_num; ++i)
    {
      uint64 start = (uint64)mm->prog_sections[i]._sec_start;
      uint64 end = start + mm->prog_sections[i]._sec_size;
      if (va >= start && va < end)
      {
        section_index = i;
        return mm->prog_sections[i]._debug_name;
      }
    }

    return nullptr;
  }

  inline int count_pa_leaf_mappings_in_sections(proc::ProcessMemoryManager *mm,
                                                mem::PageTable *pt,
                                                uint64 target_pa)
  {
    if (mm == nullptr || pt == nullptr || target_pa == 0)
    {
      return 0;
    }

    int matches = 0;
    for (int i = 0; i < mm->prog_section_count && i < proc::max_program_section_num; ++i)
    {
      uint64 start = PGROUNDDOWN((uint64)mm->prog_sections[i]._sec_start);
      uint64 end = PGROUNDUP((uint64)mm->prog_sections[i]._sec_start + mm->prog_sections[i]._sec_size);
      for (uint64 va = start; va < end; va += PGSIZE)
      {
        mem::Pte pte = pt->walk(va, false);
        if (!pte.is_null() && pte.is_valid() && (uint64)pte.pa() == target_pa)
        {
          ++matches;
        }
      }
    }
    return matches;
  }

  inline int count_pa_as_pagetable_page(mem::PageTable pt, uint64 target_pa)
  {
    if (pt.get_base() == 0 || target_pa == 0)
    {
      return 0;
    }

    int matches = (to_phy(pt.get_base()) == target_pa) ? 1 : 0;
    for (int i = 0; i < 512; ++i)
    {
      mem::Pte pte = pt.get_pte(i);
      if (!pte.is_null() && pte.is_valid() && pte.is_dir_page())
      {
        mem::PageTable child;
        child.set_base(to_vir((uint64)pte.pa()));
        matches += count_pa_as_pagetable_page(child, target_pa);
      }
    }
    return matches;
  }

  inline int count_pa_as_leaf_mapping(mem::PageTable pt, uint64 target_pa)
  {
    if (pt.get_base() == 0 || target_pa == 0)
    {
      return 0;
    }

    int matches = 0;
    for (int i = 0; i < 512; ++i)
    {
      mem::Pte pte = pt.get_pte(i);
      if (pte.is_null() || !pte.is_valid())
      {
        continue;
      }
      if (pte.is_dir_page())
      {
        mem::PageTable child;
        child.set_base(to_vir((uint64)pte.pa()));
        matches += count_pa_as_leaf_mapping(child, target_pa);
        continue;
      }
      if ((uint64)pte.pa() == target_pa)
      {
        ++matches;
      }
    }
    return matches;
  }

  void debug_pthread_exit_robust_loop(proc::Pcb *p)
  {
    static uint64 same_pc_ticks = 0;
    static uint64 last_tid = 0;
    static uint64 last_pc = 0;

    constexpr uint64 k_exit_spin_pc = 0x1200354a8ULL;
    constexpr uint64 k_self_off_robust_head = 120;
    constexpr uint64 k_self_off_robust_pending = 136;
    constexpr uint64 k_self_off_detach_state = 40;

    if (p == nullptr || p->_trapframe == nullptr || p->get_pagetable() == nullptr)
    {
      same_pc_ticks = 0;
      last_tid = 0;
      last_pc = 0;
      return;
    }

    uint64 pc = p->_trapframe->era;
    if (pc != k_exit_spin_pc)
    {
      same_pc_ticks = 0;
      last_tid = 0;
      last_pc = 0;
      return;
    }

    if (last_tid == (uint64)p->_tid && last_pc == pc)
    {
      same_pc_ticks++;
    }
    else
    {
      last_tid = p->_tid;
      last_pc = pc;
      same_pc_ticks = 1;
    }

    if (same_pc_ticks < 400)
    {
      return;
    }

    uint64 self = p->_trapframe->s0;
    uint64 robust_head = 0;
    uint64 robust_pending = 0;
    uint32 detach_state = 0;
    uint64 rp_next = 0;
    uint64 rp_word0 = 0;
    uint64 t0 = p->_trapframe->t0;
    uint64 t1 = p->_trapframe->t1;
    uint64 t2 = p->_trapframe->t2;
    uint64 t3 = p->_trapframe->t3;
    uint64 lock_addr = 0;
    int32 lock_val = 0;
    int32 waiters_val = 0;
    uint64 lock_pte_raw = 0;
    int lock_pte_valid = 0;
    int lock_pte_present = 0;
    int lock_pte_writable = 0;
    int lock_pte_user = 0;
    uint64 lock_pte_mat = 0;
    uint64 lock_pa = 0;
    mem::PhysicalMemoryManager::PageDebugInfo lock_page_info{};
    uint64 self_pte_raw = 0;
    uint64 self_pa = 0;
    uint64 trapframe_pa = 0;
    mem::PhysicalMemoryManager::PageDebugInfo self_page_info{};
    int self_section_index = -1;
    const char *self_section_name = nullptr;
    int self_leaf_aliases = 0;
    int self_all_leaf_aliases = 0;
    int self_pt_page_aliases = 0;
    LoongarchTlbProbe self_tlb{};
    int lock_section_index = -1;
    const char *lock_section_name = nullptr;
    int lock_leaf_aliases = 0;
    int lock_all_leaf_aliases = 0;
    int lock_pt_page_aliases = 0;
    LoongarchTlbProbe lock_tlb{};
    int sibling_count = 0;
    int sibling_tid = -1;
    int sibling_state = -1;
    int sibling_killed = 0;
    uint64 sibling_chan = 0;
    uint64 sibling_futex_addr = 0;
    uint64 sibling_clear_tid = 0;
    uint64 sibling_era = 0;
    uint64 sibling_sp = 0;
    uint64 sibling_tp = 0;
    uint32 llbctl = 0;
    asm volatile("csrrd %0, %1" : "=r"(llbctl) : "i"(LOONGARCH_CSR_LLBCTL));

    mem::PageTable *pt = p->get_pagetable();
    proc::ProcessMemoryManager *mm = p->get_memory_manager();
    trapframe_pa = p->_trapframe == nullptr ? 0 : to_phy((uint64)p->_trapframe);
    mem::k_vmm.copy_in(*pt, &robust_head, self + k_self_off_robust_head, sizeof(robust_head));
    mem::k_vmm.copy_in(*pt, &robust_pending, self + k_self_off_robust_pending, sizeof(robust_pending));
    mem::k_vmm.copy_in(*pt, &detach_state, self + k_self_off_detach_state, sizeof(detach_state));
    if (robust_head != 0)
    {
      mem::k_vmm.copy_in(*pt, &rp_next, robust_head, sizeof(rp_next));
      if (robust_head >= 32)
      {
        mem::k_vmm.copy_in(*pt, &rp_word0, robust_head - 32, sizeof(rp_word0));
      }
    }

    if (self != 0)
    {
      mem::Pte self_pte = pt->walk(self, false);
      if (!self_pte.is_null())
      {
        self_pte_raw = self_pte.get_data();
        self_pa = (uint64)self_pte.pa();
        self_page_info = mem::PhysicalMemoryManager::debug_query_page((void *)self_pa);
        self_section_name = find_mm_section_name(mm, self, self_section_index);
        self_leaf_aliases = count_pa_leaf_mappings_in_sections(mm, pt, self_pa);
        self_all_leaf_aliases = count_pa_as_leaf_mapping(*pt, self_pa);
        self_pt_page_aliases = count_pa_as_pagetable_page(*pt, self_pa);
        self_tlb = probe_loongarch_tlb(self);
      }
    }

    if (t2 != 0)
    {
      lock_addr = t2;
      mem::k_vmm.copy_in(*pt, &lock_val, lock_addr, sizeof(lock_val));
      mem::k_vmm.copy_in(*pt, &waiters_val, lock_addr + sizeof(int32), sizeof(waiters_val));
      mem::Pte lock_pte = pt->walk(lock_addr, false);
      if (!lock_pte.is_null())
      {
        lock_pte_raw = lock_pte.get_data();
        lock_pte_valid = lock_pte.is_valid();
        lock_pte_present = lock_pte.is_present();
        lock_pte_writable = lock_pte.is_writable();
        lock_pte_user = !lock_pte.is_super_plv();
        lock_pte_mat = lock_pte.mat();
        lock_pa = (uint64)lock_pte.pa();
        lock_page_info = mem::PhysicalMemoryManager::debug_query_page((void *)lock_pa);
        lock_section_name = find_mm_section_name(mm, lock_addr, lock_section_index);
        lock_leaf_aliases = count_pa_leaf_mappings_in_sections(mm, pt, lock_pa);
        lock_all_leaf_aliases = count_pa_as_leaf_mapping(*pt, lock_pa);
        lock_pt_page_aliases = count_pa_as_pagetable_page(*pt, lock_pa);
        lock_tlb = probe_loongarch_tlb(lock_addr);
      }
    }

    for (uint i = 0; i < proc::num_process; ++i)
    {
      proc::Pcb &candidate = proc::k_proc_pool[i];
      if (&candidate == p || candidate._state == proc::ProcState::UNUSED)
      {
        continue;
      }
      if (candidate._tgid != p->_tgid)
      {
        continue;
      }

      ++sibling_count;
      if (sibling_tid == -1)
      {
        sibling_tid = candidate._tid;
        sibling_state = candidate._state;
        sibling_killed = candidate._killed;
        sibling_chan = (uint64)candidate._chan;
        sibling_futex_addr = (uint64)candidate._futex_addr;
        sibling_clear_tid = candidate._clear_tid_addr;
        if (candidate._trapframe != nullptr)
        {
          sibling_era = candidate._trapframe->era;
          sibling_sp = candidate._trapframe->sp;
          sibling_tp = candidate._trapframe->tp;
        }
      }
    }

    panic("debug pthread_exit robust loop: pid=%d tid=%d tgid=%d era=%p self=%p detach_state=%u robust_head=%p robust_pending=%p rp_next=%p rp_lock_word=%x self_sec=%d/%s self_pte=%p self_pa=%p trapframe_pa=%p self_managed=%d self_off=%u self_free=%d self_state=%d self_block=%u self_node=%d self_level=%d self_leaf_alias=%d self_all_leaf_alias=%d self_pt_alias=%d self_tlb_hit=%d self_tlb_idx=%u self_tlbehi=%p self_tlbelo0=%p self_tlbelo1=%p tl_lock=%p tl_sec=%d/%s tl_lock_val=%d tl_waiters=%d tl_pte=%p tl_pte_valid=%d tl_pte_present=%d tl_pte_w=%d tl_pte_user=%d tl_pte_mat=%d tl_pa=%p tl_managed=%d tl_off=%u tl_free=%d tl_state=%d tl_block=%u tl_node=%d tl_level=%d tl_leaf_alias=%d tl_all_leaf_alias=%d tl_pt_alias=%d tl_tlb_hit=%d tl_tlb_idx=%u tl_tlbehi=%p tl_tlbelo0=%p tl_tlbelo1=%p sibling_count=%d sibling_tid=%d sibling_state=%d sibling_killed=%d sibling_chan=%p sibling_futex=%p sibling_clear_tid=%p sibling_era=%p sibling_sp=%p sibling_tp=%p llbctl=%x t0=%p t1=%p t2=%p t3=%p sigmask=%p signal=%p sp=%p tp=%p s0=%p",
          p->_pid,
          p->_tid,
          p->_tgid,
          (void *)pc,
          (void *)self,
          detach_state,
          (void *)robust_head,
          (void *)robust_pending,
          (void *)rp_next,
          (uint32)rp_word0,
          self_section_index,
          self_section_name ? self_section_name : "(none)",
          (void *)self_pte_raw,
          (void *)self_pa,
          (void *)trapframe_pa,
          self_page_info.managed,
          (uint32)self_page_info.page_offset,
          self_page_info.buddy.is_free,
          (int)self_page_info.buddy.node_state,
          self_page_info.buddy.block_pages,
          self_page_info.buddy.node_index,
          self_page_info.buddy.node_level,
          self_leaf_aliases,
          self_all_leaf_aliases,
          self_pt_page_aliases,
          (int)self_tlb.hit,
          self_tlb.idx,
          (void *)self_tlb.tlbehi,
          (void *)self_tlb.tlbelo0,
          (void *)self_tlb.tlbelo1,
          (void *)lock_addr,
          lock_section_index,
          lock_section_name ? lock_section_name : "(none)",
          lock_val,
          waiters_val,
          (void *)lock_pte_raw,
          lock_pte_valid,
          lock_pte_present,
          lock_pte_writable,
          lock_pte_user,
          (int)lock_pte_mat,
          (void *)lock_pa,
          lock_page_info.managed,
          (uint32)lock_page_info.page_offset,
          lock_page_info.buddy.is_free,
          (int)lock_page_info.buddy.node_state,
          lock_page_info.buddy.block_pages,
          lock_page_info.buddy.node_index,
          lock_page_info.buddy.node_level,
          lock_leaf_aliases,
          lock_all_leaf_aliases,
          lock_pt_page_aliases,
          (int)lock_tlb.hit,
          lock_tlb.idx,
          (void *)lock_tlb.tlbehi,
          (void *)lock_tlb.tlbelo0,
          (void *)lock_tlb.tlbelo1,
          sibling_count,
          sibling_tid,
          sibling_state,
          sibling_killed,
          (void *)sibling_chan,
          (void *)sibling_futex_addr,
          (void *)sibling_clear_tid,
          (void *)sibling_era,
          (void *)sibling_sp,
          (void *)sibling_tp,
          llbctl,
          (void *)t0,
          (void *)t1,
          (void *)t2,
          (void *)t3,
          (void *)p->_sigmask,
          (void *)p->_signal,
          (void *)p->_trapframe->sp,
          (void *)p->_trapframe->tp,
          (void *)p->_trapframe->s0);
  }

  void debug_entry_static_user_stall(proc::Pcb *p)
  {
    static uint64 same_pc_ticks = 0;
    static uint64 last_tid = 0;
    static uint64 last_pc = 0;

    constexpr uint64 k_stall_ticks = 1200;

    if (!is_entry_static_thread(p) || p == nullptr || p->_trapframe == nullptr || p->get_memory_manager() == nullptr)
    {
      same_pc_ticks = 0;
      last_tid = 0;
      last_pc = 0;
      return;
    }

    uint64 pc = p->_trapframe->era;
    if (last_tid == (uint64)p->_tid && last_pc == pc)
    {
      same_pc_ticks++;
    }
    else
    {
      last_tid = p->_tid;
      last_pc = pc;
      same_pc_ticks = 1;
    }

    if (same_pc_ticks < k_stall_ticks)
    {
      return;
    }

    int pc_section_index = -1;
    const char *pc_section_name = find_mm_section_name(p->get_memory_manager(), pc, pc_section_index);
    int sp_section_index = -1;
    const char *sp_section_name = find_mm_section_name(p->get_memory_manager(), p->_trapframe->sp, sp_section_index);

    int sibling_count = 0;
    int sibling_tid = -1;
    int sibling_state = -1;
    int sibling_killed = 0;
    uint64 sibling_chan = 0;
    uint64 sibling_futex_addr = 0;
    uint64 sibling_clear_tid = 0;
    uint64 sibling_era = 0;
    uint64 sibling_sp = 0;
    uint64 sibling_tp = 0;

    for (uint i = 0; i < proc::num_process; ++i)
    {
      proc::Pcb &candidate = proc::k_proc_pool[i];
      if (&candidate == p || candidate._state == proc::ProcState::UNUSED)
      {
        continue;
      }
      if (candidate._tgid != p->_tgid)
      {
        continue;
      }

      ++sibling_count;
      if (sibling_tid == -1)
      {
        sibling_tid = candidate._tid;
        sibling_state = candidate._state;
        sibling_killed = candidate._killed;
        sibling_chan = (uint64)candidate._chan;
        sibling_futex_addr = (uint64)candidate._futex_addr;
        sibling_clear_tid = candidate._clear_tid_addr;
        if (candidate._trapframe != nullptr)
        {
          sibling_era = candidate._trapframe->era;
          sibling_sp = candidate._trapframe->sp;
          sibling_tp = candidate._trapframe->tp;
        }
      }
    }

    panic("debug entry-static stall: pid=%d tid=%d tgid=%d same_pc_ticks=%p era=%p pc_sec=%d/%s sp=%p sp_sec=%d/%s tp=%p ra=%p a0=%p a1=%p a2=%p a3=%p t0=%p t1=%p t2=%p t3=%p s0=%p s1=%p state=%d chan=%p futex=%p clear_tid=%p sigmask=%p signal=%p sibling_count=%d sibling_tid=%d sibling_state=%d sibling_killed=%d sibling_chan=%p sibling_futex=%p sibling_clear_tid=%p sibling_era=%p sibling_sp=%p sibling_tp=%p",
          p->_pid,
          p->_tid,
          p->_tgid,
          (void *)same_pc_ticks,
          (void *)pc,
          pc_section_index,
          pc_section_name ? pc_section_name : "(none)",
          (void *)p->_trapframe->sp,
          sp_section_index,
          sp_section_name ? sp_section_name : "(none)",
          (void *)p->_trapframe->tp,
          (void *)p->_trapframe->ra,
          (void *)p->_trapframe->a0,
          (void *)p->_trapframe->a1,
          (void *)p->_trapframe->a2,
          (void *)p->_trapframe->a3,
          (void *)p->_trapframe->t0,
          (void *)p->_trapframe->t1,
          (void *)p->_trapframe->t2,
          (void *)p->_trapframe->t3,
          (void *)p->_trapframe->s0,
          (void *)p->_trapframe->s1,
          p->_state,
          (void *)p->_chan,
          (void *)p->_futex_addr,
          (void *)p->_clear_tid_addr,
          (void *)p->_sigmask,
          (void *)p->_signal,
          sibling_count,
          sibling_tid,
          sibling_state,
          sibling_killed,
          (void *)sibling_chan,
          (void *)sibling_futex_addr,
          (void *)sibling_clear_tid,
          (void *)sibling_era,
          (void *)sibling_sp,
          (void *)sibling_tp);
  }

}

// 初始化锁
void trap_manager::init()
{
  ticks = 0;
  timeslice = 0;
  tickslock.init("tickslock");
  printfGreen("[trap] Trap Manager Init\n");
}

// 架构相关, 设置csr
void trap_manager::inithart()
{
  uint32 ecfg = (0U << CSR_ECFG_VS_SHIFT) | HWI_VEC | TI_VEC;
  // LoongArch 的 timer CSR 直接按周期数编程。这里必须与 tmm::cycles_per_tick()
  // 保持一致，否则 sleep()/CPU 计时/interval timer 会共同漂移，LTP 的
  // setitimer01 就会从毫秒级拖成几十秒。
  uint64 tcfg = tmm::cycles_per_tick() | CSR_TCFG_EN | CSR_TCFG_PER;

  w_csr_ecfg(ecfg);
  w_csr_tcfg(tcfg);

  w_csr_eentry((uint64)kernelvec);
  w_csr_tlbrentry((uint64)handle_tlbr);
  w_csr_merrentry((uint64)handle_merr);
  intr_on();
}

// 处理外部中断和软件中断
int trap_manager::devintr()
{
  static bool uart_irq_warned = false;
  static bool pcie_irq_warned = false;

  uint32 estat = r_csr_estat();
  uint32 ecfg = r_csr_ecfg();
  uint32 ecode = loongarch_exception_code(estat);

  // LoongArch 的 ESTAT 同时混合了“异常编码”和“中断待处理位”。
  // 如果只盯 pending bit，不检查 ecode，内核在处理别的同步异常时，
  // 只要此刻恰好挂着一个 timer pending，就会被误判成时钟中断，
  // 然后重入 timertick()，最终在 tickslock 上表现成同核递归拿锁。
  // 这里先确保当前 trap 的一级编码确实是“中断”场景，再进入中断分发。
  if (ecode != 0)
  {
    return 0;
  }

  if (estat & ecfg & HWI_VEC)
  {
    // this is a hardware interrupt, via IOCR.

    // irq indicates which device interrupted.
    uint64 irq = extioi_claim();
    // printf("%d\n", irq);
    // 处理串口中断
    if (irq & (1UL << UART0_IRQ))
    {
      if (!uart_irq_warned)
      {
        uart_irq_warned = true;
        printfYellow("[trap] UART0 中断尚未接入驱动，先记录并继续运行\n");
      }
      apic_complete(1UL << UART0_IRQ);
      extioi_complete(1UL << UART0_IRQ);
    }
    else if (irq & (1UL << PCIE_IRQ))
    {
      // TODO
      // intr_stats::k_intr_stats.record_interrupt(PCIE_IRQ);
      // loongarch::qemu::disk_driver.handle_intr();
            if (!pcie_irq_warned)
      {
        pcie_irq_warned = true;
        printfYellow("[trap] PCIE 中断当前未走内核分发路径，先确认并放行\n");
      }
      apic_complete(1UL << PCIE_IRQ);
      extioi_complete(1UL << PCIE_IRQ);
      printfYellow("未实现PCIE_IRQ中断处理,不过好像跟riscv不一样，跟蒙老师也不一样，现在好像不用这个\n");
    }
    else if (irq)
    {
      printf("unexpected interrupt irq=%d\n", irq);

      apic_complete(irq);
      extioi_complete(irq);
    }

    return 1;
  }
  else if (estat & ecfg & TI_VEC)
  {
    // timer interrupt.
    // 先清 pending，再做较重的 tick/唤醒/定时器检查，避免中途再进一次 trap
    // 时又把这同一个 pending timer 误当成“新的时钟中断”。
    loongarch_ack_timer_interrupt();

    if (proc::k_pm.get_cur_cpuid() == 0)
    {
      timertick();
    }

    return 2;
  }
  else
  {
    return 0;
  }
}

void trap_manager::timertick()
{
  // tickslock 只保护全局 tick 计数本身，复杂逻辑尽量放到锁外，
  // 降低中断路径里持锁时间，也减少后续异常把现场糊成“递归拿 tickslock”的概率。
  tickslock.acquire();
  ticks++;
  tickslock.release();

  proc::k_pm.wakeup(&ticks);

  // Check for expired POSIX timers and send signals
  check_expired_timers();
  proc::check_interval_timers(proc::k_pm.get_cur_pcb());
}

// !!写完进程后修改
void trap_manager::usertrap()
{
  // printfMagenta("==usertrap==\n");

  int which_dev = 0;

  if ((r_csr_prmd() & PRMD_PPLV) == 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_csr_eentry((uint64)kernelvec);

  proc::Pcb *p = proc::k_pm.get_cur_pcb();
  // 时间统计：从用户态切换到内核态
  uint64 cur_tick = tmm::get_ticks();
  if (p->_last_user_tick > 0)
  {
    // 累加用户态运行时间
    uint64 user_time = cur_tick - p->_last_user_tick;
    p->_user_ticks += user_time;
  }
  // 记录进入内核态的时间点
  p->_kernel_entry_tick = cur_tick;

  // save user program counter.
  p->_trapframe->era = r_csr_era();

  uint32 estat = r_csr_estat();
  uint32 ecode = loongarch_exception_code(estat);
  uint32 esubcode = loongarch_exception_subcode(estat);

  if (ecode == 0xb)
  {
    // system call

    if (p->_killed)
      proc::k_pm.exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->_trapframe->era += 4;

    // an interrupt will change crmd & prmd registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall::k_syscall_handler.invoke_syscaller();
  }

  else if (is_loongarch_page_fault_code(ecode))
  {
    uint64 badv = r_csr_badv();
    mem::Pte fault_pte = p->get_pagetable()->walk(badv, false);
    uint64 fault_pte_raw = fault_pte.is_null() ? 0 : fault_pte.get_data();
    if (is_entry_static_thread(p) &&
        p->_trapframe != nullptr &&
        p->_trapframe->era >= 0x1200354a8ULL &&
        p->_trapframe->era <= 0x1200354c4ULL)
    {
      panic("debug entry-static llsc fault: pid=%d tid=%d tgid=%d ecode=%u esub=%u era=%p badv=%p pte=%p valid=%d present=%d write=%d user=%d dirty=%d tl_lock=%p t0=%p t1=%p t2=%p t3=%p sp=%p tp=%p",
            p->_pid,
            p->_tid,
            p->_tgid,
            ecode,
            esubcode,
            (void *)p->_trapframe->era,
            (void *)badv,
            (void *)fault_pte_raw,
            (int)!fault_pte.is_null() && fault_pte.is_valid(),
            (int)!fault_pte.is_null() && fault_pte.is_present(),
            (int)!fault_pte.is_null() && fault_pte.is_writable(),
            (int)!fault_pte.is_null() && !fault_pte.is_super_plv(),
            (int)!fault_pte.is_null() && fault_pte.is_dirty(),
            (void *)p->_trapframe->t2,
            (void *)p->_trapframe->t0,
            (void *)p->_trapframe->t1,
            (void *)p->_trapframe->t2,
            (void *)p->_trapframe->t3,
            (void *)p->_trapframe->sp,
            (void *)p->_trapframe->tp);
    }
    if (fault_pte.is_null())
    {
      printfRed("usertrap(): badv=%p has null pte slot\n", badv);
    }

    // LoongArch 的软件 TLB 路径里，已映射用户页也可能因为 TLB 里残着 V=0 /
    // 旧权限状态而先打到 TLBL/TLBS/TLBI/TLBM。对于这种“PTE 已经合法存在”的页，
    // 先做一次按页失效，让硬件重走 refill；不要误丢进 mmap 懒分配分支。
    if (loongarch_can_retry_present_user_fault(fault_pte, ecode))
    {
      if (ecode == 0x4 && !fault_pte.is_dirty())
      {
        fault_pte.set_data(fault_pte.get_data() | loongarch::pte_dirty_m);
      }
      loongarch_invalidate_user_tlb_page(badv);
      goto usertrap_page_fault_done;
    }

    if (mmap_handler(badv, ecode) != 0)
    {
      if (is_entry_static_thread(p))
      {
        panic("debug entry-static pagefault-fail: pid=%d tid=%d tgid=%d ecode=%u esub=%u era=%p badv=%p pte_raw=%p pte_null=%d pt_base=%p sp=%p tp=%p s0=%p s1=%p a0=%p a1=%p a2=%p",
              p->_pid,
              p->_tid,
              p->_tgid,
              ecode,
              esubcode,
              (void *)p->_trapframe->era,
              (void *)badv,
              (void *)fault_pte_raw,
              (int)fault_pte.is_null(),
              (void *)p->get_pagetable()->get_base(),
              (void *)p->_trapframe->sp,
              (void *)p->_trapframe->tp,
              (void *)p->_trapframe->s0,
              (void *)p->_trapframe->s1,
              (void *)p->_trapframe->a0,
              (void *)p->_trapframe->a1,
              (void *)p->_trapframe->a2);
      }
      // 正常的惰性缺页不需要刷日志；只有补页失败时才展开上下文，方便定位真实异常。
      printfRed("usertrap(): page fault at %p, sending SIGSEGV to pid=%d\n", badv, p->_pid);
      printfYellow("usertrap(): fault pte=%p valid=%d present=%d user=%d read=%d write=%d exec=%d plv=%d\n",
                   (void *)fault_pte.get_data(),
                   (int)fault_pte.is_valid(),
                   (int)fault_pte.is_present(),
                   (int)!fault_pte.is_super_plv(),
                   (int)fault_pte.is_readable(),
                   (int)fault_pte.is_writable(),
                   (int)fault_pte.is_executable(),
                   (int)fault_pte.plv());
      printfYellow("usertrap(): regs ra=%p sp=%p fp=%p s0=%p s1=%p s2=%p s3=%p s4=%p t0=%p t1=%p a0=%p a1=%p a2=%p\n",
                   (void *)p->_trapframe->ra,
                   (void *)p->_trapframe->sp,
                   (void *)p->_trapframe->fp,
                   (void *)p->_trapframe->s0,
                   (void *)p->_trapframe->s1,
                   (void *)p->_trapframe->s2,
                   (void *)p->_trapframe->s3,
                   (void *)p->_trapframe->s4,
                   (void *)p->_trapframe->t0,
                   (void *)p->_trapframe->t1,
                   (void *)p->_trapframe->a0,
                   (void *)p->_trapframe->a1,
                   (void *)p->_trapframe->a2);
      p->add_signal(proc::ipc::signal::SIGSEGV);

      printf("usertrap(): unexpected trapcause 0x%x pid=%d ecode=%u esubcode=%u\n",
             estat, p->_pid, ecode, esubcode);
      printf("            era=%p badi=%x\n", r_csr_era(), r_csr_badi());
    }
  usertrap_page_fault_done:
    ;
  }
  else if (ecode == 0x8 || ecode == 0x9)
  {
    // LoongArch 手册：
    //   ecode=0x8, esubcode=0 => ADEF（取指地址错误）
    //   ecode=0x8, esubcode=1 => ADEM（访存地址错误）
    //   ecode=0x9            => ALE（地址对齐错误）
    // 这些都不是“缺页可补”的场景，直接按用户态同步地址错误送信号。
    printfRed("usertrap(): address error pid=%d ecode=%u esubcode=%u era=%p badv=%p badi=%x\n",
              p->_pid, ecode, esubcode, r_csr_era(), r_csr_badv(), r_csr_badi());
    printfYellow("usertrap(): address-error regs ra=%p sp=%p fp=%p s0=%p s1=%p s2=%p s3=%p s4=%p t0=%p t1=%p a0=%p a1=%p a2=%p\n",
                 (void *)p->_trapframe->ra,
                 (void *)p->_trapframe->sp,
                 (void *)p->_trapframe->fp,
                 (void *)p->_trapframe->s0,
                 (void *)p->_trapframe->s1,
                 (void *)p->_trapframe->s2,
                 (void *)p->_trapframe->s3,
                 (void *)p->_trapframe->s4,
                 (void *)p->_trapframe->t0,
                 (void *)p->_trapframe->t1,
                 (void *)p->_trapframe->a0,
                 (void *)p->_trapframe->a1,
                 (void *)p->_trapframe->a2);
    p->add_signal(proc::ipc::signal::SIGSEGV);
  }
  else if ((which_dev = devintr()) != 0)
  {
    // ok
  }
  else
  {
    printf("usertrap(): unexpected trapcause %x pid=%d\n", r_csr_estat(), p->_pid);
    printf("            era=%p badi=%x,badv=%p\n", r_csr_era(), r_csr_badi(), r_csr_badv());
    p->_killed = 1; // loongarch这里先不改, riscv的改为使用scuase判断信号 @todo
  }

  if (which_dev == 2 && p->_last_user_tick > 0 && cur_tick == p->_last_user_tick)
  {
    // LoongArch 这里和 RISC-V 一样，timer tick 是在 devintr() 里推进的。
    // 不补这一拍，用户态 CPU 时间不会随定时中断累计，VIRTUAL/PROF timer 会卡死。
    p->_user_ticks += 1;
    p->_kernel_entry_tick = tmm::get_ticks();
  }

  if (p->_killed)
    proc::k_pm.exit(-1);

  if (which_dev == 2)
  {
    debug_pthread_exit_robust_loop(p);
    debug_entry_static_user_stall(p);
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    if (timeslice >= 10)
    {
      timeslice = 0;
      printf("yield in usertrap\n");
      proc::k_scheduler.yield();
    }
  }
  proc::ipc::signal::handle_signal(); // 处理信号 - 在返回用户态之前检查并处理待处理的信号
  usertrapret();
}

void trap_manager::usertrapret(void)
{
  //   printfCyan("==usertrapret== pid=%d\n", proc::k_pm.get_cur_pcb()->_pid);
  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  // 优先处理同步信号(紧急信号) - 在返回用户态之前检查并处理
  proc::ipc::signal::handle_sync_signal();

  // 时间统计：从内核态切换到用户态
  uint64 cur_tick = tmm::get_ticks();
  if (p->_kernel_entry_tick > 0)
  {
    // 累加内核态运行时间
    uint64 kernel_time = cur_tick - p->_kernel_entry_tick;
    p->_stime += kernel_time;
  }
  // 记录进入用户态的时间点
  p->_last_user_tick = cur_tick;

  // LoongArch 下同一地址空间内线程共享 PGDL，但各自拥有独立 trapframe 物理页。
  // 这里不要每次返回用户态都无条件 remap + invtlb：
  // 同线程的普通 syscall/timer 往返若反复碰 invtlb，会把 LL/SC 的保留窗口打碎。
  // 只有当当前 TRAPFRAME 的页表落点不是“这个线程自己的 trapframe 页”时，才重绑它。
  bool need_remap_trapframe = true;
  mem::Pte trapframe_pte = p->get_pagetable()->walk(TRAPFRAME, false);
  if (!trapframe_pte.is_null() && trapframe_pte.is_valid() && trapframe_pte.is_present())
  {
    uint64 current_trapframe_pa = (uint64)trapframe_pte.pa();
    uint64 target_trapframe_pa = PGROUNDDOWN((uint64)p->get_trapframe());
    if (current_trapframe_pa == target_trapframe_pa)
    {
      need_remap_trapframe = false;
    }
  }

  if (need_remap_trapframe)
  {
    mem::k_vmm.vmunmap(*p->get_pagetable(), TRAPFRAME, 1, 0);

    if (mem::k_vmm.map_pages(*p->get_pagetable(), TRAPFRAME, PGSIZE, (uint64)(p->get_trapframe()),
                             PTE_V | PTE_NX | PTE_P | PTE_W | PTE_R | PTE_MAT | PTE_D) == 0)
    {
      panic("usertrapret: failed to dynamically map trapframe");
    }

#ifdef LOONGARCH
    // 仅在 trapframe 真正切到“别的物理页”时做一次按对失效，避免同 PGDL 多线程
    // 继续沿用上一个线程残留的 TRAPFRAME TLB。
    loongarch_invalidate_user_tlb_page(TRAPFRAME);
#endif
  }

  intr_off();

  // send syscalls, interrupts, and exceptions to uservec.S
  w_csr_eentry((uint64)uservec); // maybe todo

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->get_trapframe()->kernel_pgdl = r_csr_pgdl();                // kernel page table
  p->get_trapframe()->kernel_sp = p->get_kstack() + KSTACK_SIZE; // process's kernel stack
  p->get_trapframe()->kernel_trap = (uint64)wrap_usertrap;
  //   printf("usertrapret: p->get_trapframe()->kernel_trap: %p\n", p->get_trapframe()->kernel_trap);
  p->get_trapframe()->kernel_hartid = r_tp(); // hartid for cpuid()

  // set up the registers that uservec.S's ertn will use
  // to get to user space.

  // set Previous Privilege mode to User Privilege3.
  uint32 x = r_csr_prmd();
  x |= PRMD_PPLV; // set PPLV to 3 for user mode
  x |= PRMD_PIE;  // enable interrupts in user mode
  w_csr_prmd(x);

  // set S Exception Program Counter to the saved user pc.
  w_csr_era(p->get_trapframe()->era);

  // tell uservec.S the user page table to switch to.
  volatile uint64 pgdl = (p->get_pagetable()->get_base());

  // jump to uservec.S at the top of memory, which
  // switches to the user page table, restores user registers,
  // and switches to user mode with ertn.
  userret(TRAPFRAME, pgdl);
}
void trap_manager::machine_trap()
{
  panic("machine error");
}
// 处理内核态的中断
// 支持嵌套中断
void trap_manager::kerneltrap()
{
  // printf("==kerneltrap==\n");
  // 这些寄存器可能在yield时被修改
  int which_dev = 0;
  uint64 era = r_csr_era();
  uint64 prmd = r_csr_prmd();

  // 检查中断是否来自内核态

  if ((prmd & PRMD_PPLV) != 0)
    panic("kerneltrap: not from privilege0");
  if (intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if ((which_dev = devintr()) == 0)
  {
    uint32 estat = r_csr_estat();
    uint32 ecode = loongarch_exception_code(estat);
    uint32 esubcode = loongarch_exception_subcode(estat);
    uint64 badv = r_csr_badv();
    uint32 badi = r_csr_badi();
    uint32 crmd = r_csr_crmd();
    uint32 ecfg = r_csr_ecfg();
    uint64 save1 = r_csr_save1();
    uint64 save2 = r_csr_save2();
    uint64 extioi_isr = read_itr_cfg_64b(LOONGARCH_IOCSR_EXTIOI_ISR_BASE);
    uint64 ls7a_status = *(volatile uint64 *)(LS7A_INT_STATUS_REG);
    uint64 heap_cache = 0;
    uint64 heap_used = 0;
    uint32 heap_chunks = 0;
    uint64 heap_free_pages = 0;
    uint32 heap_max_block_pages = 0;
    mem::k_hmm.get_stats(heap_cache, heap_used, heap_chunks, heap_free_pages, heap_max_block_pages);
    proc::Pcb *cur = Cpu::get_cpu()->get_cur_proc();

    if (cur != nullptr)
    {
      panic("kerneltrap: estat=%x ecode=%d esub=%d era=%p eentry=%p badv=%p badi=%x crmd=%x prmd=%x ecfg=%x save1=%p save2=%p pgdl=%p extioi=%p ls7a=%p proc=%s pid=%d tid=%d state=%d pt=%p mm=%p heap_used=%p heap_cache=%p heap_chunks=%d heap_free_pages=%p heap_max_block_bytes=%p",
            estat,
            ecode,
            esubcode,
            r_csr_era(),
            r_csr_eentry(),
            (void *)badv,
            badi,
            crmd,
            prmd,
            ecfg,
            (void *)save1,
            (void *)save2,
            (void *)r_csr_pgdl(),
            (void *)extioi_isr,
            (void *)ls7a_status,
            cur->_name,
            cur->_pid,
            cur->_tid,
            cur->_state,
            cur->get_pagetable(),
            cur->get_memory_manager(),
            (void *)heap_used,
            (void *)heap_cache,
            heap_chunks,
            (void *)heap_free_pages,
            (void *)(static_cast<uint64>(heap_max_block_pages) * PGSIZE));
    }

    panic("kerneltrap: estat=%x ecode=%d esub=%d era=%p eentry=%p badv=%p badi=%x crmd=%x prmd=%x ecfg=%x save1=%p save2=%p pgdl=%p extioi=%p ls7a=%p no-current-proc heap_used=%p heap_cache=%p heap_chunks=%d heap_free_pages=%p heap_max_block_bytes=%p",
          estat,
          ecode,
          esubcode,
          r_csr_era(),
          r_csr_eentry(),
          (void *)badv,
          badi,
          crmd,
          prmd,
          ecfg,
          (void *)save1,
          (void *)save2,
          (void *)r_csr_pgdl(),
          (void *)extioi_isr,
          (void *)ls7a_status,
          (void *)heap_used,
          (void *)heap_cache,
          heap_chunks,
          (void *)heap_free_pages,
          (void *)(static_cast<uint64>(heap_max_block_pages) * PGSIZE));
  }

  ///@todo!! 写完进程后修改
  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && Cpu::get_cpu()->get_cur_proc() != nullptr && Cpu::get_cpu()->get_cur_proc()->_state == proc::RUNNING)
  {
    timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
    if (timeslice >= 5)
    {
      timeslice = 0;
      printf("yield in kerneltrap\n");
      proc::k_scheduler.yield();
    }
  }
  // if (which_dev == 2)
  // {
  //   timeslice++; // 让一个进程连续执行若干时间片，printf线程不安全
  //   // printf("timeslice: %d\n", timeslice);
  //   if (timeslice >= 5)
  //   {
  //     timeslice = 0;
  //   }
  // }

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_csr_era(era);
  w_csr_prmd(prmd);
}
/**
 * @brief mmap_handler 处理mmap惰性分配导致的页面错误
 * @param va 页面故障虚拟地址
 * @param cause 页面故障原因 (13=load fault, 15=store fault)
 * @return 0成功，-1失败
 */
int mmap_handler(uint64 va, int cause)
{
  int i;
  proc::Pcb *p = proc::k_pm.get_cur_pcb();

  // 根据地址查找属于哪一个VMA
  for (i = 0; i < proc::NVMA; ++i)
  {
    if (p->get_vma()->_vm[i].used)
    {
      // 检查是否在当前VMA范围内
      if (va >= p->get_vma()->_vm[i].addr && va < p->get_vma()->_vm[i].addr + p->get_vma()->_vm[i].len)
      {
        printfGreen("mmap_handler: found VMA %d for va %p\n", i, va);
        break; // 在当前VMA范围内
      }
    }
  }

  if (i == proc::NVMA)
  {
    printfRed("mmap_handler: no VMA found for va %p\n", va);
    return -1;
  }

  // 获取VMA结构
  struct proc::vma *vm = &p->get_vma()->_vm[i];

  // 确定访问类型 (LoongArch的异常码)
  int access_type = 0; // 默认读取
  if (cause == 2)
  {                  // Store page fault
    printfOrange("[mmap_handler] Store page fault at va: %p\n", va);
    access_type = 1; // 写入
  }
  else if (cause == 8||cause == 3)
  {                  // Instruction page fault
    printfOrange("[mmap_handler] Instruction page fault at va: %p\n", va);
    access_type = 2; // 执行
  }
  else{
    printfRed("[mmap_handler] Load page fault cause: %d at va: %p\n", cause, va);
  }

  // 使用统一的VMA页面分配函数
  return mem::k_vmm.allocate_vma_page(*p->get_pagetable(), va, vm, access_type);
}
#endif
