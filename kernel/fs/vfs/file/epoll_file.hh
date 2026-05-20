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
        uint32 events;
        uint64 data;
    };

    class epoll_file : public file
    {
    private:
        eastl::vector<epoll_watch_entry> _watch_list;
        int _create_flags = 0;

    public:
        explicit epoll_file(int create_flags = 0)
            : file(FileAttrs(FileTypes::FT_DEVICE, 0600), "anon_inode:[eventpoll]"),
              _create_flags(create_flags)
        {
            dup();
            new (&_stat) Kstat(_attrs.filetype);
            _stat.mode = _attrs.transMode();
        }

        ~epoll_file() override = default;

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

        int add_watch(int fd, uint32 events, uint64 data)
        {
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd;
            });
            if (it != _watch_list.end())
            {
                return -EEXIST;
            }

            _watch_list.push_back(epoll_watch_entry{fd, events, data});
            return 0;
        }

        int mod_watch(int fd, uint32 events, uint64 data)
        {
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd;
            });
            if (it == _watch_list.end())
            {
                return -ENOENT;
            }

            it->events = events;
            it->data = data;
            return 0;
        }

        int del_watch(int fd)
        {
            auto it = eastl::find_if(_watch_list.begin(), _watch_list.end(), [&](const epoll_watch_entry &entry) {
                return entry.fd == fd;
            });
            if (it == _watch_list.end())
            {
                return -ENOENT;
            }

            _watch_list.erase(it);
            return 0;
        }

        int create_flags() const { return _create_flags; }
        size_t watch_count() const { return _watch_list.size(); }
    };
} // namespace fs
