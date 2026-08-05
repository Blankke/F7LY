#pragma once

#include "fs/vfs/file/file.hh"
#include <EASTL/algorithm.h>
#include <EASTL/vector.h>
#include <asm-generic/errno-base.h>

namespace fs
{
    struct epoll_watch_entry
    {
        int fd;
        // epoll 持有被监视 open-file-description 的引用；扫描时仍需从 fd 表取临时
        // 引用并核对身份，避免 fd 关闭并复用后把事件错误投递给新对象。
        file *target_identity;
        uint32 events;
        uint64 data;
        uint32 last_ready_events = 0;
        bool oneshot_disabled = false;
    };

    class epoll_file : public file
    {
    private:
        eastl::vector<epoll_watch_entry> _watch_list;
        int _create_flags = 0;
        SpinLock _watch_lock;
        // readiness 扫描的轮转起点；即使零超时只使用小型栈缓冲，也不会让
        // 长期 level-triggered 的前几个 fd 饿死后续 watch。
        size_t _scan_cursor = 0;

    public:
        explicit epoll_file(int create_flags = 0)
            : file(FileAttrs(FileTypes::FT_DEVICE, 0600), "anon_inode:[eventpoll]"),
              _create_flags(create_flags)
        {
            _watch_lock.init("epoll_watch");
            dup();
            new (&_stat) Kstat(_attrs.filetype);
            _stat.mode = _attrs.transMode();
        }

        ~epoll_file() override
        {
            for (auto &entry : _watch_list)
            {
                entry.target_identity->free_file();
            }
        }

        long read(uint64, size_t, long, bool) override { return -EINVAL; }
        long write(uint64, size_t, long, bool) override { return -EINVAL; }
        bool read_ready() override { return false; }
        bool write_ready() override { return false; }
        off_t lseek(off_t, int) override { return -ESPIPE; }
        bool is_epoll_file() const override { return true; }

        size_t read_sub_dir(ubuf &) override
        {
            return 0;
        }

        int add_watch(int fd, file *target, uint32 events, uint64 data)
        {
            _watch_lock.acquire();
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd && entry.target_identity == target;
            });
            if (it != _watch_list.end())
            {
                _watch_lock.release();
                return -EEXIST;
            }

            target->dup();
            _watch_list.push_back(epoll_watch_entry{fd, target, events, data, 0, false});
            _watch_lock.release();
            return 0;
        }

        int mod_watch(int fd, file *target, uint32 events, uint64 data)
        {
            _watch_lock.acquire();
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd && entry.target_identity == target;
            });
            if (it == _watch_list.end())
            {
                _watch_lock.release();
                return -ENOENT;
            }

            it->events = events;
            it->data = data;
            it->last_ready_events = 0;
            it->oneshot_disabled = false;
            _watch_lock.release();
            return 0;
        }

        int del_watch(int fd, file *target)
        {
            _watch_lock.acquire();
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd && entry.target_identity == target;
            });
            if (it == _watch_list.end())
            {
                _watch_lock.release();
                return -ENOENT;
            }

            file *removed_target = it->target_identity;
            _watch_list.erase(it);
            _watch_lock.release();
            removed_target->free_file();
            return 0;
        }

        int create_flags() const { return _create_flags; }
        void lock_watches() { _watch_lock.acquire(); }
        void unlock_watches() { _watch_lock.release(); }
        eastl::vector<epoll_watch_entry> &watch_list() { return _watch_list; }
        size_t &scan_cursor() { return _scan_cursor; }

        eastl::vector<epoll_watch_entry> snapshot_watches()
        {
            eastl::vector<epoll_watch_entry> snapshot;
            _watch_lock.acquire();
            snapshot.reserve(_watch_list.size());
            for (const auto &entry : _watch_list)
            {
                entry.target_identity->dup();
                snapshot.push_back(entry);
            }
            _watch_lock.release();
            return snapshot;
        }
    };
} // namespace fs
