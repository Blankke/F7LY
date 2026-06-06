#pragma once

#include "types.hh"

namespace dev
{
    class BlockDeviceIoctlState
    {
    public:
        static constexpr u32 k_blkraset = 0x1262;
        static constexpr u32 k_blkraget = 0x1263;
        static constexpr u32 k_blkgetsize = 0x1260;
        static constexpr u32 k_blkgetsize64 = 0x1272;

        // BLKRAGET/BLKRASET 目前是内核级模拟状态，集中放在块设备 ioctl 状态对象里。
        ulong read_ahead() const { return _read_ahead; }
        void set_read_ahead(ulong value) { _read_ahead = value; }

    private:
        ulong _read_ahead = 0;
    };

    extern BlockDeviceIoctlState k_block_device_ioctl_state;
}
