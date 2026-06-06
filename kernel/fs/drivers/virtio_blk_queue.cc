#include "virtio_blk_queue.hh"

#include "proc_manager.hh"
#include "scheduler.hh"
#include "tm/timer_manager.hh"
#include "virtual_memory_manager.hh"

namespace virtio_blk
{
    namespace
    {
        bool g_priority_borrow_experiment_mode = false;
        constexpr uint64 k_priority_active_window_us = 5000;
        constexpr uint64 k_low_class_borrow_spacing_us = 8000;
    }

    void set_priority_borrow_experiment_mode(bool enabled)
    {
        g_priority_borrow_experiment_mode = enabled;
    }

    bool priority_borrow_experiment_mode_enabled()
    {
        return g_priority_borrow_experiment_mode;
    }

    uint64 VirtioBlkQueue::now_us()
    {
        tmm::timeval tv = tmm::k_tm.get_time_val();
        return tv.tv_sec * 1000000ULL + tv.tv_usec;
    }

    uint64 VirtioBlkQueue::ceil_div_u64(uint64 numerator, uint64 denominator)
    {
        return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
    }

    int VirtioBlkQueue::first_pending_class(uint32 pending_mask)
    {
        for (int service_class = 0; service_class < PriorityBorrowScheduler::k_class_count; ++service_class)
        {
            if ((pending_mask & (1U << service_class)) != 0)
            {
                return service_class;
            }
        }
        return -1;
    }

    bool VirtioBlkQueue::has_multiple_pending_class(uint32 pending_mask)
    {
        return pending_mask != 0 && (pending_mask & (pending_mask - 1U)) != 0;
    }

    bool VirtioBlkQueue::has_lower_pending_class(uint32 pending_mask, int service_class)
    {
        for (int lower_class = service_class + 1; lower_class < PriorityBorrowScheduler::k_class_count; ++lower_class)
        {
            if ((pending_mask & (1U << lower_class)) != 0)
            {
                return true;
            }
        }
        return false;
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
        scheduler_.reset();
        reset_bandwidth_stats_locked();
        reset_priority_trace_locked();
        memset(inflight_by_class_, 0, sizeof(inflight_by_class_));
        memset(last_activity_us_by_class_, 0, sizeof(last_activity_us_by_class_));
        memset(next_borrow_submit_us_by_class_, 0, sizeof(next_borrow_submit_us_by_class_));

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

    void VirtioBlkQueue::reset_bandwidth_stats_locked()
    {
        for (int i = 0; i < PriorityBorrowScheduler::k_class_count; ++i)
        {
            class_stats_[i].completed_bytes = 0;
            class_stats_[i].completed_requests = 0;
            class_stats_[i].window_start_us = 0;
            class_stats_[i].ewma_bps = 0;
        }
    }

    void VirtioBlkQueue::reset_priority_trace_locked()
    {
        priority_trace_.total_dispatches = 0;
        priority_trace_.contended_dispatches = 0;
        priority_trace_.high_wins = 0;
        priority_trace_.low_while_high_pending = 0;
        priority_trace_.last_report_dispatch = 0;
        for (int i = 0; i < PriorityBorrowScheduler::k_class_count; ++i)
        {
            priority_trace_.selected_by_class[i] = 0;
        }
    }

    bool VirtioBlkQueue::has_recent_higher_activity_locked(int service_class, uint64 now) const
    {
        if (service_class <= 0)
        {
            return false;
        }

        for (int higher_class = 0; higher_class < service_class; ++higher_class)
        {
            uint64 last_activity = last_activity_us_by_class_[higher_class];
            if (last_activity != 0 && now >= last_activity &&
                now - last_activity <= k_priority_active_window_us)
            {
                return true;
            }
        }
        return false;
    }

    uint64 VirtioBlkQueue::reserve_lower_class_submit_time_locked(int service_class, uint64 now)
    {
        if (service_class <= 0 || !has_recent_higher_activity_locked(service_class, now))
        {
            if (service_class >= 0 && service_class < PriorityBorrowScheduler::k_class_count)
            {
                next_borrow_submit_us_by_class_[service_class] = 0;
            }
            return now;
        }

        /*
         * 优先级借用不是硬限速：高优先级 class 活跃时，低优先级 class 仍按固定间隔
         * 获得借用机会；高优先级停止或产生空窗后，下面会立即清掉节流状态。
         */
        uint64 reserved = next_borrow_submit_us_by_class_[service_class];
        if (reserved < now)
        {
            reserved = now;
        }
        next_borrow_submit_us_by_class_[service_class] = reserved + k_low_class_borrow_spacing_us;
        return reserved;
    }

    void VirtioBlkQueue::throttle_lower_class_submit_if_needed(int service_class)
    {
        if (!priority_borrow_experiment_mode_enabled() ||
            service_class <= 0 ||
            service_class >= PriorityBorrowScheduler::k_class_count)
        {
            return;
        }

        lock_.acquire();
        uint64 now = now_us();
        uint64 submit_after = reserve_lower_class_submit_time_locked(service_class, now);
        lock_.release();

        while (now < submit_after)
        {
            proc::k_scheduler.yield();
            now = now_us();
        }
    }

    uint32 VirtioBlkQueue::inflight_class_mask_locked() const
    {
        uint32 mask = 0;
        for (int service_class = 0; service_class < PriorityBorrowScheduler::k_class_count; ++service_class)
        {
            if (inflight_by_class_[service_class] != 0)
            {
                mask |= (1U << service_class);
            }
        }
        return mask;
    }

    void VirtioBlkQueue::record_priority_trace_locked(uint32 pending_mask, const IoRequest *request)
    {
        if (request == nullptr)
        {
            return;
        }

        uint32 inflight_mask = inflight_class_mask_locked();
        uint32 combined_mask = pending_mask | inflight_mask;
        int highest_class = first_pending_class(combined_mask);
        int selected_class = request->service_class;
        ++priority_trace_.total_dispatches;
        if (selected_class >= 0 && selected_class < PriorityBorrowScheduler::k_class_count)
        {
            ++priority_trace_.selected_by_class[selected_class];
        }

        bool combined_contended = highest_class >= 0 && has_lower_pending_class(combined_mask, highest_class);
        if (combined_contended)
        {
            ++priority_trace_.contended_dispatches;
            if (selected_class == highest_class)
            {
                ++priority_trace_.high_wins;
            }
            else if (selected_class > highest_class)
            {
                ++priority_trace_.low_while_high_pending;
            }
        }

        constexpr uint64 k_bootstrap_dispatch_reports = 48;
        constexpr uint64 k_report_interval = 128;
        bool should_report = combined_contended ||
                             priority_trace_.total_dispatches <= k_bootstrap_dispatch_reports ||
                             (priority_trace_.total_dispatches - priority_trace_.last_report_dispatch) >= k_report_interval ||
                             priority_trace_.low_while_high_pending != 0;
        if (!should_report)
        {
            return;
        }

        priority_trace_.last_report_dispatch = priority_trace_.total_dispatches;
        // printf("[priority-borrow] TRACE seq=%lu pending=0x%x inflight=0x%x combined=0x%x choose=%d "
        //        "pid=%u nice=%d bytes=%lu contended=%lu high_wins=%lu low_while_high=%lu\n",
        //        (unsigned long)priority_trace_.total_dispatches,
        //        pending_mask,
        //        inflight_mask,
        //        combined_mask,
        //        selected_class,
        //        request->submit_pid,
        //        request->submit_nice,
        //        (unsigned long)request->request_bytes,
        //        (unsigned long)priority_trace_.contended_dispatches,
        //        (unsigned long)priority_trace_.high_wins,
        //        (unsigned long)priority_trace_.low_while_high_pending);
    }

    void VirtioBlkQueue::record_completion_stats_locked(const IoRequest *request,
                                                        uint64 dispatch_us,
                                                        uint64 finish_us)
    {
        if (request == nullptr)
        {
            return;
        }

        int service_class = request->service_class;
        if (service_class < 0 || service_class >= PriorityBorrowScheduler::k_class_count)
        {
            return;
        }

        ClassBandwidthStats &stats = class_stats_[service_class];
        last_activity_us_by_class_[service_class] = finish_us;
        if (stats.window_start_us == 0)
        {
            stats.window_start_us = finish_us;
        }
        stats.completed_bytes += request->request_bytes;
        stats.completed_requests += 1;

        if (dispatch_us != 0 && finish_us > dispatch_us)
        {
            uint64 elapsed_us = finish_us - dispatch_us;
            uint64 instant_bps = ceil_div_u64(request->request_bytes * 1000000ULL, elapsed_us);
            if (instant_bps != 0)
            {
                stats.ewma_bps = stats.ewma_bps == 0 ? instant_bps : (stats.ewma_bps * 7 + instant_bps) / 8;
            }
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

        if (request->service_class >= 0 && request->service_class < PriorityBorrowScheduler::k_class_count)
        {
            ++inflight_by_class_[request->service_class];
        }
        ++inflight_count_;
        transport_->notify_queue(0);
        return true;
    }

    bool VirtioBlkQueue::dispatch_pending_locked()
    {
        bool submitted = false;
        const int dispatch_window =
            priority_borrow_experiment_mode_enabled() ? k_priority_borrow_experiment_dispatch_window : k_queue_size;

        while (inflight_count_ < dispatch_window && has_free_desc_chain_locked())
        {
            uint32 pending_mask = scheduler_.pending_class_mask();
            IoRequest *request = scheduler_.dequeue_next();
            if (request == nullptr)
            {
                break;
            }

            record_priority_trace_locked(pending_mask, request);

            if (!submit_one_locked(request))
            {
                panic("VirtioBlkQueue::dispatch_pending_locked: descriptor accounting mismatch");
            }

            submitted = true;
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
            record_completion_stats_locked(done, info_[id].dispatch_us, finish_us);

            if (done != nullptr)
            {
                if (done->service_class >= 0 &&
                    done->service_class < PriorityBorrowScheduler::k_class_count &&
                    inflight_by_class_[done->service_class] != 0)
                {
                    --inflight_by_class_[done->service_class];
                }
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

        dispatch_pending_locked();
    }

    void VirtioBlkQueue::submit_request_and_wait(IoRequest &request)
    {
        int submit_class = PriorityBorrowScheduler::nice_to_service_class(request.submit_nice);
        throttle_lower_class_submit_if_needed(submit_class);

        lock_.acquire();
        last_activity_us_by_class_[submit_class] = now_us();
        scheduler_.enqueue(&request);
        dispatch_pending_locked();

        while (!request.completed)
        {
            process_used_locked();
            if (request.completed)
            {
                break;
            }

            dispatch_pending_locked();

            if (transport_->polling_wait())
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
        request.submit_nice = current ? current->get_io_priority() : proc::default_proc_prio;
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
        request.submit_nice = current ? current->get_io_priority() : proc::default_proc_prio;
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
