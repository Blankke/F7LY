#include "syscall_handler.hh"

#include "klib.hh"
#include "mem/virtual_memory_manager.hh"
#include "proc/capability.hh"
#include "proc/proc.hh"
#include "proc/proc_manager.hh"

namespace syscall
{
    uint64 SyscallHandler::sys_getppid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->get_ppid();
    }

    uint64 SyscallHandler::sys_getpid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->get_pid();
    }

    uint64 SyscallHandler::sys_set_tid_address()
    {
        uint64 tidptr;
        if (_arg_addr(0, tidptr) < 0)
        {
            printfRed("[SyscallHandler::sys_set_tid_address] Error fetching tidptr argument\n");
            return SYS_EFAULT;
        }
        return proc::k_pm.set_tid_address(tidptr);
    }

    uint64 SyscallHandler::sys_gettid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->get_tid();
    }

    uint64 SyscallHandler::sys_getuid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->_uid;
    }

    uint64 SyscallHandler::sys_geteuid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->_euid;
    }

    uint64 SyscallHandler::sys_getgid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->_gid;
    }

    uint64 SyscallHandler::sys_getegid()
    {
        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        return p == nullptr ? 0 : p->_egid;
    }

    uint64 SyscallHandler::sys_setuid()
    {
        int uid_raw;
        if (_arg_int(0, uid_raw) < 0 || uid_raw < 0)
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        uint32 old_uid = p->_uid;
        uint32 old_euid = p->_euid;
        uint32 old_suid = p->_suid;
        uint32 uid = static_cast<uint32>(uid_raw);

        if (p->_euid == 0)
        {
            // 特权进程的 setuid 同步 real/effective/saved/fs uid。
            p->_uid = uid;
            p->_euid = uid;
            p->_suid = uid;
            p->_fsuid = uid;
        }
        else
        {
            // 非特权进程只能切到 real/effective/saved uid 之一。
            if (uid != p->_uid && uid != p->_euid && uid != p->_suid)
            {
                return SYS_EPERM;
            }

            p->_euid = uid;
            p->_fsuid = uid;
        }

        proc::k_capability.update_after_uid_change(p, old_uid, old_euid, old_suid);
        return 0;
    }

    uint64 SyscallHandler::sys_setgid()
    {
        int gid_raw;
        if (_arg_int(0, gid_raw) < 0 || gid_raw < 0)
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        uint32 gid = static_cast<uint32>(gid_raw);
        if (p->_euid == 0)
        {
            // 特权进程的 setgid 同步 real/effective/saved/fs gid。
            p->_gid = gid;
            p->_egid = gid;
            p->_sgid = gid;
            p->_fsgid = gid;
            return 0;
        }

        if (gid != p->_gid && gid != p->_egid && gid != p->_sgid)
        {
            return SYS_EPERM;
        }

        p->_egid = gid;
        p->_fsgid = gid;
        return 0;
    }

    uint64 SyscallHandler::sys_setreuid()
    {
        int ruid;
        int euid;
        if (_arg_int(0, ruid) < 0 || _arg_int(1, euid) < 0)
        {
            printfRed("[SyscallHandler::sys_setreuid] 参数错误\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setreuid] ruid: %d, euid: %d\n", ruid, euid);

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_setreuid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 old_ruid = p->get_uid();
        uint32 old_euid = p->get_euid();
        uint32 old_suid = p->get_suid();
        uint32 new_ruid = (ruid == -1) ? old_ruid : static_cast<uint32>(ruid);
        uint32 new_euid = (euid == -1) ? old_euid : static_cast<uint32>(euid);

        printfCyan("[SyscallHandler::sys_setreuid] 当前状态: ruid=%u, euid=%u, suid=%u\n",
                   old_ruid, old_euid, old_suid);

        if ((ruid < -1) || (euid < -1))
        {
            printfRed("[SyscallHandler::sys_setreuid] 无效参数: ruid=%d, euid=%d\n", ruid, euid);
            return SYS_EINVAL;
        }

        if (old_euid != 0)
        {
            if (ruid != -1 && new_ruid != old_ruid && new_ruid != old_euid && new_ruid != old_suid)
            {
                printfRed("[SyscallHandler::sys_setreuid] 权限不足：无法设置真实用户ID为 %u\n", new_ruid);
                return SYS_EPERM;
            }
            if (euid != -1 && new_euid != old_ruid && new_euid != old_euid && new_euid != old_suid)
            {
                printfRed("[SyscallHandler::sys_setreuid] 权限不足：无法设置有效用户ID为 %u\n", new_euid);
                return SYS_EPERM;
            }
        }

        // Linux/POSIX: real uid 显式改变，或 effective uid 改成不同于新 real uid，
        // saved uid 都更新为新的 effective uid。
        uint32 new_suid = old_suid;
        if ((ruid != -1 && new_ruid != old_ruid) ||
            (euid != -1 && new_euid != new_ruid))
        {
            new_suid = new_euid;
        }

        p->set_uid(new_ruid);
        p->set_euid(new_euid);
        p->set_suid(new_suid);
        p->set_fsuid(new_euid);
        proc::k_capability.update_after_uid_change(p, old_ruid, old_euid, old_suid);
        printfGreen("[SyscallHandler::sys_setreuid] 成功设置用户ID: ruid=%u, euid=%u, suid=%u\n",
                    new_ruid, new_euid, new_suid);
        return 0;
    }

    uint64 SyscallHandler::sys_setregrid()
    {
        int rgid;
        int egid;
        if (_arg_int(0, rgid) < 0 || _arg_int(1, egid) < 0)
        {
            printfRed("[SyscallHandler::sys_setregrid] 参数错误\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setregrid] rgid: %d, egid: %d\n", rgid, egid);

        if (rgid < -1 || egid < -1)
        {
            printfRed("[SyscallHandler::sys_setregrid] 无效参数: rgid=%d, egid=%d\n", rgid, egid);
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_setregrid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 current_rgid = p->_gid;
        uint32 current_egid = p->_egid;
        uint32 current_sgid = p->_sgid;
        uint32 new_rgid = (rgid == -1) ? current_rgid : static_cast<uint32>(rgid);
        uint32 new_egid = (egid == -1) ? current_egid : static_cast<uint32>(egid);

        printfCyan("[SyscallHandler::sys_setregrid] 当前状态: rgid=%d, egid=%d, sgid=%d\n",
                   current_rgid, current_egid, current_sgid);

        if (rgid == -1 && egid == -1)
        {
            printfCyan("[SyscallHandler::sys_setregrid] 无操作，直接返回成功\n");
            return 0;
        }

        if (p->_euid != 0)
        {
            printfCyan("[SyscallHandler::sys_setregrid] 非特权进程，检查权限\n");
            if (rgid != -1 && rgid != static_cast<int>(current_rgid) && rgid != static_cast<int>(current_egid))
            {
                printfRed("[SyscallHandler::sys_setregrid] 非特权进程无权设置 rgid: %d (允许: %d, %d)\n",
                          rgid, current_rgid, current_egid);
                return SYS_EPERM;
            }
            if (rgid != -1)
            {
                printfCyan("[SyscallHandler::sys_setregrid] rgid权限检查通过: %d\n", rgid);
            }
            if (egid != -1 &&
                egid != static_cast<int>(current_rgid) &&
                egid != static_cast<int>(current_egid) &&
                egid != static_cast<int>(current_sgid))
            {
                printfRed("[SyscallHandler::sys_setregrid] 非特权进程无权设置 egid: %d (允许: %d, %d, %d)\n",
                          egid, current_rgid, current_egid, current_sgid);
                return SYS_EPERM;
            }
            if (egid != -1)
            {
                printfCyan("[SyscallHandler::sys_setregrid] egid权限检查通过: %d\n", egid);
            }
        }
        else
        {
            printfCyan("[SyscallHandler::sys_setregrid] 特权进程，可以设置任意值\n");
        }

        p->_gid = new_rgid;
        p->_egid = new_egid;
        if (rgid != -1 || (egid != -1 && new_egid != current_rgid))
        {
            p->_sgid = new_egid;
            printfCyan("[SyscallHandler::sys_setregrid] 更新 sgid 为: %d\n", p->_sgid);
        }
        p->_fsgid = p->_egid;
        printfCyan("[SyscallHandler::sys_setregrid] 成功设置组ID - rgid=%d, egid=%d, sgid=%d, fsgid=%d\n",
                   p->_gid, p->_egid, p->_sgid, p->_fsgid);
        return 0;
    }

    uint64 SyscallHandler::sys_setresuid()
    {
        int ruid;
        int euid;
        int suid;
        if (_arg_int(0, ruid) < 0 || _arg_int(1, euid) < 0 || _arg_int(2, suid) < 0)
        {
            printfRed("[SyscallHandler::sys_setresuid] 参数错误\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setresuid] ruid: %d, euid: %d, suid: %d\n", ruid, euid, suid);

        if (ruid < -1 || euid < -1 || suid < -1)
        {
            printfRed("[SyscallHandler::sys_setresuid] 无效参数: ruid=%d, euid=%d, suid=%d\n", ruid, euid, suid);
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_setresuid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 old_uid = p->get_uid();
        uint32 old_euid = p->get_euid();
        uint32 old_suid = p->get_suid();
        bool privileged = p->get_euid() == 0;

        if (!privileged)
        {
            if (ruid != -1 && ruid != static_cast<int>(old_uid) &&
                ruid != static_cast<int>(old_euid) && ruid != static_cast<int>(old_suid))
            {
                printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 ruid: %d\n", ruid);
                return SYS_EPERM;
            }
            if (euid != -1 && euid != static_cast<int>(old_uid) &&
                euid != static_cast<int>(old_euid) && euid != static_cast<int>(old_suid))
            {
                printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 euid: %d\n", euid);
                return SYS_EPERM;
            }
            if (suid != -1 && suid != static_cast<int>(old_uid) &&
                suid != static_cast<int>(old_euid) && suid != static_cast<int>(old_suid))
            {
                printfRed("[SyscallHandler::sys_setresuid] 非特权进程无权设置 suid: %d\n", suid);
                return SYS_EPERM;
            }
        }
        else
        {
            printfCyan("[SyscallHandler::sys_setresuid] 特权进程，可以设置任意值\n");
        }

        if (ruid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 ruid: %d\n", ruid);
            }
            p->set_uid(static_cast<uint32>(ruid));
        }
        if (euid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 euid: %d\n", euid);
            }
            p->set_euid(static_cast<uint32>(euid));
            p->set_fsuid(static_cast<uint32>(euid));
        }
        if (suid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresuid] 非特权进程设置 suid: %d\n", suid);
            }
            p->set_suid(static_cast<uint32>(suid));
        }

        proc::k_capability.update_after_uid_change(p, old_uid, old_euid, old_suid);
        return 0;
    }

    uint64 SyscallHandler::sys_getresuid()
    {
        uint64 ruid_addr;
        uint64 euid_addr;
        uint64 suid_addr;
        if (_arg_addr(0, ruid_addr) < 0 || _arg_addr(1, euid_addr) < 0 || _arg_addr(2, suid_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getresuid] 参数错误\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_getresuid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 ruid = p->get_uid();
        uint32 euid = p->get_euid();
        uint32 suid = p->get_suid();
        mem::PageTable *pt = p->get_pagetable();
        printfCyan("[SyscallHandler::sys_getresuid] ruid: %u, euid: %u, suid: %u\n", ruid, euid, suid);
        if (mem::k_vmm.copy_out(*pt, ruid_addr, &ruid, sizeof(ruid)) < 0 ||
            mem::k_vmm.copy_out(*pt, euid_addr, &euid, sizeof(euid)) < 0 ||
            mem::k_vmm.copy_out(*pt, suid_addr, &suid, sizeof(suid)) < 0)
        {
            printfRed("[SyscallHandler::sys_getresuid] 拷贝到用户空间失败\n");
            return SYS_EFAULT;
        }
        return 0;
    }

    uint64 SyscallHandler::sys_setresgid()
    {
        int rgid;
        int egid;
        int sgid;
        if (_arg_int(0, rgid) < 0 || _arg_int(1, egid) < 0 || _arg_int(2, sgid) < 0)
        {
            printfRed("[SyscallHandler::sys_setresgid] 参数错误\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setresgid] rgid: %d, egid: %d, sgid: %d\n", rgid, egid, sgid);

        if (rgid < -1 || egid < -1 || sgid < -1)
        {
            printfRed("[SyscallHandler::sys_setresgid] 无效参数: rgid=%d, egid=%d, sgid=%d\n", rgid, egid, sgid);
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_setresgid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 old_gid = p->get_gid();
        uint32 old_egid = p->get_egid();
        uint32 old_sgid = p->get_sgid();
        bool privileged = p->get_euid() == 0;

        if (!privileged)
        {
            if (rgid != -1 && rgid != static_cast<int>(old_gid) &&
                rgid != static_cast<int>(old_egid) && rgid != static_cast<int>(old_sgid))
            {
                printfRed("[SyscallHandler::sys_setresgid] 非特权进程无权设置 rgid: %d\n", rgid);
                return SYS_EPERM;
            }
            if (egid != -1 && egid != static_cast<int>(old_gid) &&
                egid != static_cast<int>(old_egid) && egid != static_cast<int>(old_sgid))
            {
                printfRed("[SyscallHandler::sys_setresgid] 非特权进程无权设置 egid: %d\n", egid);
                return SYS_EPERM;
            }
            if (sgid != -1 && sgid != static_cast<int>(old_gid) &&
                sgid != static_cast<int>(old_egid) && sgid != static_cast<int>(old_sgid))
            {
                printfRed("[SyscallHandler::sys_setresgid] 非特权进程无权设置 sgid: %d\n", sgid);
                return SYS_EPERM;
            }
        }
        else
        {
            printfCyan("[SyscallHandler::sys_setresgid] 特权进程，可以设置任意值\n");
        }

        if (rgid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresgid] 非特权进程设置 rgid: %d\n", rgid);
            }
            p->set_gid(static_cast<uint32>(rgid));
        }
        if (egid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresgid] 非特权进程设置 egid: %d\n", egid);
            }
            p->set_egid(static_cast<uint32>(egid));
            p->set_fsgid(static_cast<uint32>(egid));
        }
        if (sgid != -1)
        {
            if (!privileged)
            {
                printfCyan("[SyscallHandler::sys_setresgid] 非特权进程设置 sgid: %d\n", sgid);
            }
            p->set_sgid(static_cast<uint32>(sgid));
        }
        return 0;
    }

    uint64 SyscallHandler::sys_getresgid()
    {
        uint64 rgid_addr;
        uint64 egid_addr;
        uint64 sgid_addr;
        if (_arg_addr(0, rgid_addr) < 0 || _arg_addr(1, egid_addr) < 0 || _arg_addr(2, sgid_addr) < 0)
        {
            printfRed("[SyscallHandler::sys_getresgid] 参数错误\n");
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            printfRed("[SyscallHandler::sys_getresgid] 无法获取当前进程\n");
            return SYS_ESRCH;
        }

        uint32 rgid = p->get_gid();
        uint32 egid = p->get_egid();
        uint32 sgid = p->get_sgid();
        mem::PageTable *pt = p->get_pagetable();
        printfCyan("[SyscallHandler::sys_getresgid] 返回组ID: rgid=%u, egid=%u, sgid=%u\n",
                   rgid, egid, sgid);
        if (mem::k_vmm.copy_out(*pt, rgid_addr, &rgid, sizeof(rgid)) < 0 ||
            mem::k_vmm.copy_out(*pt, egid_addr, &egid, sizeof(egid)) < 0 ||
            mem::k_vmm.copy_out(*pt, sgid_addr, &sgid, sizeof(sgid)) < 0)
        {
            printfRed("[SyscallHandler::sys_getresgid] 拷贝到用户空间失败\n");
            return SYS_EFAULT;
        }
        return 0;
    }

    uint64 SyscallHandler::sys_setfsuid()
    {
        int fsuid_raw;
        if (_arg_int(0, fsuid_raw) < 0)
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        uint32 old_fsuid = p->_fsuid;
        if (fsuid_raw == -1)
        {
            return old_fsuid;
        }
        if (fsuid_raw < -1)
        {
            return old_fsuid;
        }

        uint32 fsuid = static_cast<uint32>(fsuid_raw);
        if (p->_euid == 0 || fsuid == p->_uid || fsuid == p->_euid || fsuid == p->_suid)
        {
            p->_fsuid = fsuid;
        }
        return old_fsuid;
    }

    uint64 SyscallHandler::sys_setfsgid()
    {
        int fsgid_raw;
        if (_arg_int(0, fsgid_raw) < 0)
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        uint32 old_fsgid = p->_fsgid;
        if (fsgid_raw == -1)
        {
            return old_fsgid;
        }
        if (fsgid_raw < -1)
        {
            return old_fsgid;
        }

        uint32 fsgid = static_cast<uint32>(fsgid_raw);
        if (p->_euid == 0 || fsgid == p->_gid || fsgid == p->_egid || fsgid == p->_sgid)
        {
            p->_fsgid = fsgid;
        }
        return old_fsgid;
    }

    uint64 SyscallHandler::sys_getgroups()
    {
        int size;
        uint64 list_addr;
        if (_arg_int(0, size) < 0 || _arg_addr(1, list_addr) < 0)
        {
            return SYS_EINVAL;
        }
        if (size < 0)
        {
            return SYS_EINVAL;
        }
        if (size > 0 && list_addr == 0)
        {
            return SYS_EFAULT;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        int group_count = p->_supplementary_group_count;
        if (size == 0)
        {
            return group_count;
        }
        if (size < group_count)
        {
            return SYS_EINVAL;
        }
        if (group_count > 0 &&
            mem::k_vmm.copy_out(*p->get_pagetable(),
                                list_addr,
                                p->_supplementary_groups,
                                group_count * sizeof(p->_supplementary_groups[0])) < 0)
        {
            return SYS_EFAULT;
        }
        return group_count;
    }

    uint64 SyscallHandler::sys_setgroups()
    {
        int size;
        uint64 list_addr;
        if (_arg_int(0, size) < 0 || _arg_addr(1, list_addr) < 0)
        {
            return SYS_EINVAL;
        }
        if (size < 0 || size > static_cast<int>(proc::max_supplementary_groups))
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }
        if (p->get_euid() != 0)
        {
            return SYS_EPERM;
        }
        if (size > 0)
        {
            if (list_addr == 0)
            {
                return SYS_EFAULT;
            }
            if (mem::k_vmm.copy_in(*p->get_pagetable(),
                                   p->_supplementary_groups,
                                   list_addr,
                                   size * sizeof(p->_supplementary_groups[0])) < 0)
            {
                return SYS_EFAULT;
            }
        }
        else
        {
            memset(p->_supplementary_groups, 0, sizeof(p->_supplementary_groups));
        }

        p->_supplementary_group_count = size;
        return 0;
    }

    uint64 SyscallHandler::sys_setpgid()
    {
        int pid;
        int pgid;
        if (_arg_int(0, pid) < 0 || _arg_int(1, pgid) < 0)
        {
            printfRed("[SyscallHandler::sys_setpgid] Error fetching arguments\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_setpgid] pid: %d, pgid: %d\n", pid, pgid);

        if (pgid < 0)
        {
            printfRed("[SyscallHandler::sys_setpgid] Invalid pgid: %d\n", pgid);
            return SYS_EINVAL;
        }
        if (static_cast<uint>(pgid) >= proc::pid_max)
        {
            printfRed("[SyscallHandler::sys_setpgid] Invalid pgid: %d\n", pgid);
            return SYS_EPERM;
        }

        proc::Pcb *current = proc::k_pm.get_cur_pcb();
        if (current == nullptr)
        {
            printfRed("[SyscallHandler::sys_setpgid] Current process is null\n");
            return SYS_ESRCH;
        }

        proc::Pcb *target = current;
        if (pid != 0)
        {
            target = proc::k_pm.find_proc_by_pid(pid);
            if (target == nullptr)
            {
                printfRed("[SyscallHandler::sys_setpgid] Process with pid %d not found\n", pid);
                return SYS_ESRCH;
            }
            if (target != current && target->get_parent() != current)
            {
                printfRed("[SyscallHandler::sys_setpgid] Permission denied: process %d is not the calling process or its child\n",
                          pid);
                return SYS_ESRCH;
            }
        }

        uint32 new_pgid = pgid == 0 ? target->get_pid() : static_cast<uint32>(pgid);
        target->set_pgid(new_pgid);
        printfGreen("[SyscallHandler::sys_setpgid] Successfully set pgid %u for process %d\n",
                    new_pgid, target->get_pid());
        return 0;
    }

    uint64 SyscallHandler::sys_getpgid()
    {
        int pid;
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_getpgid] Error fetching pid argument\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_getpgid] pid: %d\n", pid);

        proc::Pcb *target = nullptr;
        if (pid == 0)
        {
            target = proc::k_pm.get_cur_pcb();
        }
        else
        {
            target = proc::k_pm.find_proc_by_pid(pid);
        }

        if (target == nullptr)
        {
            printfRed("[SyscallHandler::sys_getpgid] Process with pid %d not found\n", pid);
            return SYS_ESRCH;
        }
        return target->get_pgid();
    }

    uint64 SyscallHandler::sys_setsid()
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();
        if (current == nullptr)
        {
            printfRed("[SyscallHandler::sys_setsid] Current process is null\n");
            return SYS_ESRCH;
        }

        // setsid 需要拒绝“调用者 pid 已经是任何现存进程组 pgid”的情况。
        for (proc::Pcb *target = proc::k_proc_pool; target < &proc::k_proc_pool[proc::num_process]; ++target)
        {
            if (target->_state != proc::ProcState::UNUSED && target->get_pgid() == current->get_pid())
            {
                return SYS_EPERM;
            }
        }

        current->set_sid(current->get_pid());
        current->set_pgid(current->get_pid());
        return current->get_sid();
    }

    uint64 SyscallHandler::sys_getsid()
    {
        int pid;
        if (_arg_int(0, pid) < 0)
        {
            printfRed("[SyscallHandler::sys_getsid] Error fetching pid argument\n");
            return SYS_EINVAL;
        }

        printfCyan("[SyscallHandler::sys_getsid] pid: %d\n", pid);

        proc::Pcb *current = proc::k_pm.get_cur_pcb();
        proc::Pcb *target = pid == 0 ? current : proc::k_pm.find_proc_by_pid(pid);
        if (target == nullptr)
        {
            printfRed("[SyscallHandler::sys_getsid] Process with pid %d not found\n", pid);
            return SYS_ESRCH;
        }
        if (current != nullptr && current->get_sid() != target->get_sid())
        {
            printfRed("[SyscallHandler::sys_getsid] Permission denied: process %d is not in the same session\n", pid);
            return SYS_EPERM;
        }
        printfGreen("[SyscallHandler::sys_getsid] Returning session ID %d for process %d\n", target->get_sid(), pid);
        return target->get_sid();
    }

    uint64 SyscallHandler::sys_umask()
    {
        int new_mask;
        if (_arg_int(0, new_mask) < 0)
        {
            return SYS_EINVAL;
        }

        proc::Pcb *p = proc::k_pm.get_cur_pcb();
        if (p == nullptr)
        {
            return SYS_ESRCH;
        }

        mode_t old_umask = p->_umask;
        p->_umask = static_cast<mode_t>(new_mask & 0777);
        return static_cast<uint64>(old_umask);
    }

    uint64 SyscallHandler::sys_personality()
    {
        constexpr uint32 k_query_personality = 0xffffffffU;

        proc::Pcb *current = proc::k_pm.get_cur_pcb();
        if (current == nullptr)
        {
            return SYS_ESRCH;
        }

        uint32 old_personality = current->get_personality();
        uint32 new_personality = static_cast<uint32>(_arg_raw(0) & 0xffffffffU);
        if (new_personality != k_query_personality)
        {
            current->set_personality(new_personality);
        }
        return old_personality;
    }
}
