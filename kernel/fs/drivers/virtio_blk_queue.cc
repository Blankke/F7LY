#include "virtio_blk_queue.hh"

#include "proc_manager.hh"
#include "scheduler.hh"
#include "tm/timer_manager.hh"
#include "virtual_memory_manager.hh"

namespace virtio_blk
{
    uint64 VirtioBlkQueue::now_us()
    {
        tmm::timeval tv = tmm::k_tm.get_time_val();
        return tv.tv_sec * 1000000ULL + tv.tv_usec;
    }

    uint64 VirtioBlkQueue::ceil_div_u64(uint64 numerator, uint64 denominator)
    {
        return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
    }

    void VirtioBlkQueue::initialize(const InitArgs &args)
    {
        lock_.init(args.lock_name);
        owner_token_ = args.owner_token;
        transport_ = args.transport;

        memset(pages_, 0, sizeof(pages_));
        desc_ = reinterpret_cast<VRingDesc *>(pages_);
        avail_ = reinterpret_cast<uint16 *>(reinterpret_cast<char *>(desc_) + k_queue_size * sizeof(VRingDesc));
        used_ = reinterpret_cast<UsedArea *>(pages_ + PGSIZE);

        reset_runtime();
    }

    void VirtioBlkQueue::reset_runtime()
    {
        used_idx_ = 0;
        inflight_count_ = 0;
        ewma_bps_ = MClockScheduler::default_ewma_bps();
        scheduler_.reset();

        for (int i = 0; i < k_queue_size; ++i)
        {
            free_[i] = 1;
            info_[i].request = nullptr;
            info_[i].status = 0;
            info_[i].dispatch_us = 0;
            memset(&info_[i].header, 0, sizeof(info_[i].header));
        }

        if (avail_ != nullptr)
        {
            avail_[0] = 0;
            avail_[1] = 0;
        }
        if (used_ != nullptr)
        {
            used_->flags = 0;
            used_->id = 0;
        }
    }

    void *VirtioBlkQueue::pages_base()
    {
        return pages_;
    }

    uint64 VirtioBlkQueue::pages_dma_base() const
    {
        return transport_->dma_addr(pages_);
    }

    VRingDesc *VirtioBlkQueue::desc_area()
    {
        return desc_;
    }

    uint16 *VirtioBlkQueue::avail_area()
    {
        return avail_;
    }

    UsedArea *VirtioBlkQueue::used_area()
    {
        return used_;
    }

    bool VirtioBlkQueue::has_free_desc_chain_locked() const
    {
        int free_count = 0;
        for (int i = 0; i < k_queue_size; ++i)
        {
            if (free_[i])
            {
                ++free_count;
                if (free_count >= 3)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool VirtioBlkQueue::alloc_desc_chain_locked(DescChain &chain)
    {
        int allocated[3] = {-1, -1, -1};
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < k_queue_size; ++j)
            {
                if (free_[j])
                {
                    free_[j] = 0;
                    allocated[i] = j;
                    break;
                }
            }

            if (allocated[i] < 0)
            {
                for (int j = 0; j < i; ++j)
                {
                    free_desc_locked(allocated[j]);
                }
                return false;
            }
        }

        chain.head = allocated[0];
        chain.middle = allocated[1];
        chain.tail = allocated[2];
        return true;
    }

    void VirtioBlkQueue::free_desc_locked(int idx)
    {
        if (idx < 0 || idx >= k_queue_size)
        {
            panic("VirtioBlkQueue::free_desc_locked: bad index");
        }

        desc_[idx].addr = 0;
        desc_[idx].len = 0;
        desc_[idx].flags = 0;
        desc_[idx].next = 0;
        free_[idx] = 1;
    }

    void VirtioBlkQueue::free_chain_locked(int head_idx)
    {
        int idx = head_idx;
        while (true)
        {
            int flag = desc_[idx].flags;
            int next = desc_[idx].next;
            free_desc_locked(idx);
            if ((flag & k_desc_flag_next) == 0)
            {
                break;
            }
            idx = next;
        }
    }

    bool VirtioBlkQueue::submit_one_locked(IoRequest *request)
    {
        DescChain chain;
        if (!alloc_desc_chain_locked(chain))
        {
            return false;
        }

        info_[chain.head].request = request;
        info_[chain.head].header.type = request->write ? k_req_type_write : k_req_type_read;
        info_[chain.head].header.reserved = 0;
        info_[chain.head].header.sector = request->start_sector;
        info_[chain.head].status = 0;
        info_[chain.head].dispatch_us = now_us();

        desc_[chain.head].addr = transport_->dma_addr(&info_[chain.head].header);
        desc_[chain.head].len = sizeof(info_[chain.head].header);
        desc_[chain.head].flags = k_desc_flag_next;
        desc_[chain.head].next = chain.middle;

        desc_[chain.middle].addr = transport_->dma_addr(request->data);
        desc_[chain.middle].len = request->data_len;
        desc_[chain.middle].flags = (request->write ? 0 : k_desc_flag_write) | k_desc_flag_next;
        desc_[chain.middle].next = chain.tail;

        desc_[chain.tail].addr = transport_->dma_addr(&info_[chain.head].status);
        desc_[chain.tail].len = 1;
        desc_[chain.tail].flags = k_desc_flag_write;
        desc_[chain.tail].next = 0;

        avail_[2 + (avail_[1] % k_queue_size)] = static_cast<uint16>(chain.head);
        __sync_synchronize();
        avail_[1] = static_cast<uint16>(avail_[1] + 1);

        ++inflight_count_;
        transport_->notify_queue(0);
        return true;
    }

    bool VirtioBlkQueue::dispatch_pending_locked(uint64 *next_gate_us)
    {
        bool submitted = false;
        uint64 local_next_gate = 0;

        while (has_free_desc_chain_locked())
        {
            MClockScheduler::DispatchDecision decision = scheduler_.dequeue_next(now_us());
            if (decision.request == nullptr)
            {
                local_next_gate = decision.next_gate_us;
                break;
            }

            if (!submit_one_locked(decision.request))
            {
                panic("VirtioBlkQueue::dispatch_pending_locked: descriptor accounting mismatch");
            }

            submitted = true;
        }

        if (next_gate_us != nullptr)
        {
            *next_gate_us = local_next_gate;
        }
        return submitted;
    }

    void VirtioBlkQueue::process_used_locked()
    {
        transport_->prepare_used_check();

        while (used_idx_ != used_->id)
        {
            int id = static_cast<int>(used_->elems[used_idx_ % k_queue_size].id);
            if (id < 0 || id >= k_queue_size)
            {
                panic("VirtioBlkQueue::process_used_locked: bad used id");
            }

            if (info_[id].status != 0)
            {
                panic("VirtioBlkQueue::process_used_locked: request status error");
            }

            IoRequest *done = info_[id].request;
            uint64 finish_us = now_us();
            if (done != nullptr && info_[id].dispatch_us != 0 && finish_us > info_[id].dispatch_us)
            {
                uint64 elapsed_us = finish_us - info_[id].dispatch_us;
                uint64 instant_bps = ceil_div_u64(done->request_bytes * 1000000ULL, elapsed_us);
                if (instant_bps != 0)
                {
                    ewma_bps_ = (ewma_bps_ * 7 + instant_bps) / 8;
                }
            }

            if (done != nullptr)
            {
                done->io_status = 0;
                done->completed = true;
                if (done->completion_type == IoCompletionType::BufferCache && done->owner_buf != nullptr)
                {
                    done->owner_buf->disk = 0;
                }
                proc::k_pm.wakeup(done->wait_channel);
            }

            info_[id].request = nullptr;
            info_[id].dispatch_us = 0;
            free_chain_locked(id);
            --inflight_count_;
            ++used_idx_;
        }

        dispatch_pending_locked(nullptr);
    }

    void VirtioBlkQueue::submit_request_and_wait(IoRequest &request)
    {
        lock_.acquire();
        scheduler_.enqueue(&request, now_us(), ewma_bps_);
        dispatch_pending_locked(nullptr);

        while (!request.completed)
        {
            process_used_locked();
            if (request.completed)
            {
                break;
            }

            uint64 next_gate_us = 0;
            dispatch_pending_locked(&next_gate_us);

            if (transport_->polling_wait())
            {
                lock_.release();
                proc::k_scheduler.yield();
                lock_.acquire();
                continue;
            }

            if (next_gate_us != 0 && inflight_count_ == 0)
            {
                lock_.release();
                proc::k_scheduler.yield();
                lock_.acquire();
                continue;
            }

            proc::k_pm.sleep(request.wait_channel, &lock_);
        }

        lock_.release();
    }

    void VirtioBlkQueue::submit_and_wait(struct buf *b, int write)
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();

        IoRequest request = {};
        request.data = b->data;
        request.data_len = BSIZE;
        request.start_sector = b->blockno;
        request.write = write != 0;
        request.request_bytes = BSIZE;
        request.submit_pid = current ? current->get_pid() : 0;
        request.submit_nice = current ? current->get_priority() : proc::default_proc_prio;
        request.wait_channel = b;
        request.completion_type = IoCompletionType::BufferCache;
        request.owner_buf = b;
        request.completed = false;
        request.io_status = 0;

        b->disk = owner_token_;
        submit_request_and_wait(request);
    }

    int VirtioBlkQueue::submit_transfer_and_wait(void *data, uint32 data_len, uint64 start_sector, bool write)
    {
        proc::Pcb *current = proc::k_pm.get_cur_pcb();

        IoRequest request = {};
        request.data = data;
        request.data_len = data_len;
        request.start_sector = start_sector;
        request.write = write;
        request.request_bytes = data_len;
        request.submit_pid = current ? current->get_pid() : 0;
        request.submit_nice = current ? current->get_priority() : proc::default_proc_prio;
        request.wait_channel = &request;
        request.completion_type = IoCompletionType::CallerWait;
        request.owner_buf = nullptr;
        request.completed = false;
        request.io_status = 0;

        submit_request_and_wait(request);
        return request.io_status;
    }

    void VirtioBlkQueue::handle_interrupt()
    {
        lock_.acquire();
        process_used_locked();
        transport_->ack_interrupt();
        lock_.release();
    }

    void VirtioBlkQueue::poll_once()
    {
        lock_.acquire();
        process_used_locked();
        lock_.release();
    }
} // namespace virtio_blk
