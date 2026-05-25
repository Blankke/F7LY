#include "virtio_mclock_scheduler.hh"

namespace virtio_blk
{
    namespace
    {
        constexpr uint64 k_usec_per_sec = 1000000ULL;
        constexpr uint64 k_default_ewma_bps_value = 64ULL * 1024ULL * 1024ULL;
        constexpr uint32 k_class_weight[MClockScheduler::k_class_count] = {256, 192, 128, 96, 64, 32, 16, 8};
        constexpr uint64 k_class_reservation_bps[MClockScheduler::k_class_count] = {
            24ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL,
            8ULL * 1024ULL * 1024ULL,
            4ULL * 1024ULL * 1024ULL,
            2ULL * 1024ULL * 1024ULL,
            1ULL * 1024ULL * 1024ULL,
            512ULL * 1024ULL,
            256ULL * 1024ULL};
        constexpr uint64 k_class_limit_bps[MClockScheduler::k_class_count] = {
            MClockScheduler::k_unlimited_bps, MClockScheduler::k_unlimited_bps,
            MClockScheduler::k_unlimited_bps, MClockScheduler::k_unlimited_bps,
            MClockScheduler::k_unlimited_bps, MClockScheduler::k_unlimited_bps,
            MClockScheduler::k_unlimited_bps, MClockScheduler::k_unlimited_bps};
        constexpr uint32 k_total_weight = 792;
    } // namespace

    uint64 MClockScheduler::default_ewma_bps()
    {
        return k_default_ewma_bps_value;
    }

    void MClockScheduler::reset()
    {
        for (int i = 0; i < k_class_count; ++i)
        {
            class_state_[i].flow_head = nullptr;
            class_state_[i].flow_tail = nullptr;
            class_state_[i].rr_hint = nullptr;
            class_state_[i].last_r_tag_us = 0;
            class_state_[i].last_w_tag_us = 0;
            class_state_[i].last_l_tag_us = 0;
        }

        for (int i = 0; i < k_max_flows; ++i)
        {
            flow_pool_[i].in_use = false;
            flow_pool_[i].pid = 0;
            flow_pool_[i].service_class = 0;
            flow_pool_[i].submit_nice = proc::default_proc_prio;
            flow_pool_[i].head = nullptr;
            flow_pool_[i].tail = nullptr;
            flow_pool_[i].next = nullptr;
            flow_pool_[i].prev = nullptr;
        }
    }

    int MClockScheduler::nice_to_service_class(int nice)
    {
        int clamped = nice;
        if (clamped < proc::highest_proc_prio)
        {
            clamped = proc::highest_proc_prio;
        }
        else if (clamped > proc::lowest_proc_prio)
        {
            clamped = proc::lowest_proc_prio;
        }
        return (clamped - proc::highest_proc_prio) / 5;
    }

    uint64 MClockScheduler::ceil_div_u64(uint64 numerator, uint64 denominator)
    {
        return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
    }

    uint64 MClockScheduler::bytes_to_service_us(uint64 bytes, uint64 bps)
    {
        if (bps == 0)
        {
            return 0;
        }

        uint64 usec = ceil_div_u64(bytes * k_usec_per_sec, bps);
        return usec == 0 ? 1 : usec;
    }

    uint64 MClockScheduler::weighted_service_us(uint64 bytes, uint32 weight, uint64 ewma_bps)
    {
        uint64 scaled_bytes = ceil_div_u64(bytes * k_total_weight, weight == 0 ? 1 : weight);
        return bytes_to_service_us(scaled_bytes, ewma_bps == 0 ? k_default_ewma_bps_value : ewma_bps);
    }

    MClockScheduler::FlowState *MClockScheduler::find_or_alloc_flow(int service_class, uint pid, int submit_nice)
    {
        ClassState &class_state = class_state_[service_class];
        for (FlowState *flow = class_state.flow_head; flow != nullptr; flow = flow->next)
        {
            if (flow->pid == pid)
            {
                flow->submit_nice = submit_nice;
                return flow;
            }
        }

        for (int i = 0; i < k_max_flows; ++i)
        {
            FlowState &flow = flow_pool_[i];
            if (flow.in_use)
            {
                continue;
            }

            flow.in_use = true;
            flow.pid = pid;
            flow.service_class = service_class;
            flow.submit_nice = submit_nice;
            flow.head = nullptr;
            flow.tail = nullptr;
            flow.next = nullptr;
            flow.prev = class_state.flow_tail;

            if (class_state.flow_tail != nullptr)
            {
                class_state.flow_tail->next = &flow;
            }
            else
            {
                class_state.flow_head = &flow;
            }

            class_state.flow_tail = &flow;
            if (class_state.rr_hint == nullptr)
            {
                class_state.rr_hint = &flow;
            }
            return &flow;
        }

        panic("MClockScheduler::find_or_alloc_flow: no free flow slot");
        return nullptr;
    }

    void MClockScheduler::release_flow(ClassState &class_state, FlowState *flow)
    {
        if (flow == nullptr)
        {
            return;
        }

        if (flow->prev != nullptr)
        {
            flow->prev->next = flow->next;
        }
        else
        {
            class_state.flow_head = flow->next;
        }

        if (flow->next != nullptr)
        {
            flow->next->prev = flow->prev;
        }
        else
        {
            class_state.flow_tail = flow->prev;
        }

        if (class_state.rr_hint == flow)
        {
            class_state.rr_hint = flow->next != nullptr ? flow->next : class_state.flow_head;
        }

        flow->in_use = false;
        flow->pid = 0;
        flow->service_class = 0;
        flow->submit_nice = proc::default_proc_prio;
        flow->head = nullptr;
        flow->tail = nullptr;
        flow->next = nullptr;
        flow->prev = nullptr;
    }

    MClockScheduler::FlowState *MClockScheduler::next_flow(ClassState &class_state, FlowState *flow)
    {
        if (class_state.flow_head == nullptr)
        {
            return nullptr;
        }
        if (flow == nullptr)
        {
            return class_state.flow_head;
        }
        return flow->next != nullptr ? flow->next : class_state.flow_head;
    }

    IoRequest *MClockScheduler::pop_flow_head(FlowState *flow)
    {
        if (flow == nullptr || flow->head == nullptr)
        {
            return nullptr;
        }

        IoRequest *request = flow->head;
        flow->head = request->flow_next;
        if (flow->head == nullptr)
        {
            flow->tail = nullptr;
        }
        request->flow_next = nullptr;
        return request;
    }

    void MClockScheduler::finish_flow_dispatch(FlowState *flow)
    {
        if (flow == nullptr)
        {
            return;
        }

        ClassState &class_state = class_state_[flow->service_class];
        class_state.rr_hint = next_flow(class_state, flow);
        if (flow->head == nullptr)
        {
            release_flow(class_state, flow);
        }
    }

    void MClockScheduler::enqueue(IoRequest *request, uint64 now_us, uint64 ewma_bps)
    {
        int service_class = nice_to_service_class(request->submit_nice);
        ClassState &class_state = class_state_[service_class];
        FlowState *flow = find_or_alloc_flow(service_class, request->submit_pid, request->submit_nice);

        request->service_class = service_class;
        request->enqueue_us = now_us;
        request->flow_next = nullptr;

        uint64 reservation_start = class_state.last_r_tag_us > now_us ? class_state.last_r_tag_us : now_us;
        request->r_tag_us = reservation_start;
        class_state.last_r_tag_us = reservation_start + bytes_to_service_us(request->request_bytes, k_class_reservation_bps[service_class]);

        uint64 weight_start = class_state.last_w_tag_us > now_us ? class_state.last_w_tag_us : now_us;
        request->w_tag_us = weight_start;
        class_state.last_w_tag_us = weight_start + weighted_service_us(request->request_bytes, k_class_weight[service_class], ewma_bps);

        if (k_class_limit_bps[service_class] == k_unlimited_bps)
        {
            request->l_tag_us = now_us;
            class_state.last_l_tag_us = now_us;
        }
        else
        {
            uint64 limit_start = class_state.last_l_tag_us > now_us ? class_state.last_l_tag_us : now_us;
            request->l_tag_us = limit_start;
            class_state.last_l_tag_us = limit_start + bytes_to_service_us(request->request_bytes, k_class_limit_bps[service_class]);
        }

        if (flow->tail == nullptr)
        {
            flow->head = request;
            flow->tail = request;
        }
        else
        {
            flow->tail->flow_next = request;
            flow->tail = request;
        }
    }

    MClockScheduler::PickState MClockScheduler::pick_locked(uint64 now_us)
    {
        PickState pick = {nullptr, nullptr, nullptr, nullptr, ~0ULL};

        for (int i = 0; i < k_class_count; ++i)
        {
            ClassState &class_state = class_state_[i];
            if (class_state.flow_head == nullptr)
            {
                continue;
            }

            FlowState *start = class_state.rr_hint != nullptr ? class_state.rr_hint : class_state.flow_head;
            FlowState *flow = start;
            if (flow == nullptr)
            {
                continue;
            }

            do
            {
                IoRequest *candidate = flow->head;
                if (candidate != nullptr)
                {
                    if (candidate->l_tag_us > now_us)
                    {
                        if (candidate->l_tag_us < pick.earliest_gate)
                        {
                            pick.earliest_gate = candidate->l_tag_us;
                        }
                    }
                    else if (candidate->r_tag_us <= now_us)
                    {
                        if (pick.best_reservation == nullptr ||
                            candidate->r_tag_us < pick.best_reservation->r_tag_us ||
                            (candidate->r_tag_us == pick.best_reservation->r_tag_us &&
                             candidate->w_tag_us < pick.best_reservation->w_tag_us))
                        {
                            pick.best_reservation = candidate;
                            pick.best_reservation_flow = flow;
                        }
                    }
                    else if (pick.best_weight == nullptr ||
                             candidate->w_tag_us < pick.best_weight->w_tag_us ||
                             (candidate->w_tag_us == pick.best_weight->w_tag_us &&
                              candidate->r_tag_us < pick.best_weight->r_tag_us))
                    {
                        pick.best_weight = candidate;
                        pick.best_weight_flow = flow;
                    }
                }
                flow = next_flow(class_state, flow);
            } while (flow != nullptr && flow != start);
        }

        return pick;
    }

    MClockScheduler::DispatchDecision MClockScheduler::dequeue_next(uint64 now_us)
    {
        PickState pick = pick_locked(now_us);
        FlowState *selected_flow = pick.best_reservation != nullptr ? pick.best_reservation_flow : pick.best_weight_flow;
        IoRequest *selected_request = pick.best_reservation != nullptr ? pick.best_reservation : pick.best_weight;

        if (selected_request == nullptr)
        {
            return {nullptr, pick.earliest_gate == ~0ULL ? 0 : pick.earliest_gate};
        }

        IoRequest *queued = pop_flow_head(selected_flow);
        if (queued != selected_request)
        {
            panic("MClockScheduler::dequeue_next: queue mismatch");
        }

        finish_flow_dispatch(selected_flow);
        return {selected_request, 0};
    }
} // namespace virtio_blk
