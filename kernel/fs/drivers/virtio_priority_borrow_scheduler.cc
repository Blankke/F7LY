#include "virtio_priority_borrow_scheduler.hh"

namespace virtio_blk
{
    void PriorityBorrowScheduler::reset()
    {
        for (int i = 0; i < k_class_count; ++i)
        {
            class_state_[i].flow_head = nullptr;
            class_state_[i].flow_tail = nullptr;
            class_state_[i].rr_hint = nullptr;
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

    int PriorityBorrowScheduler::nice_to_service_class(int nice)
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

        int service_class = (clamped - proc::highest_proc_prio) / 5;
        return service_class >= k_class_count ? k_class_count - 1 : service_class;
    }

    PriorityBorrowScheduler::FlowState *PriorityBorrowScheduler::find_or_alloc_flow(int service_class,
                                                                                    uint pid,
                                                                                    int submit_nice)
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

        panic("PriorityBorrowScheduler::find_or_alloc_flow: no free flow slot");
        return nullptr;
    }

    void PriorityBorrowScheduler::release_flow(ClassState &class_state, FlowState *flow)
    {
        if (flow == nullptr)
        {
            return;
        }

        FlowState *next_hint = nullptr;
        if (class_state.rr_hint == flow)
        {
            next_hint = flow->next != nullptr ? flow->next : (flow->prev != nullptr ? class_state.flow_head : nullptr);
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
            class_state.rr_hint = next_hint;
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

    PriorityBorrowScheduler::FlowState *PriorityBorrowScheduler::next_flow(ClassState &class_state,
                                                                           FlowState *flow)
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

    IoRequest *PriorityBorrowScheduler::pop_flow_head(FlowState *flow)
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

    void PriorityBorrowScheduler::finish_flow_dispatch(FlowState *flow)
    {
        if (flow == nullptr)
        {
            return;
        }

        ClassState &class_state = class_state_[flow->service_class];
        if (flow->head == nullptr)
        {
            release_flow(class_state, flow);
            return;
        }

        class_state.rr_hint = next_flow(class_state, flow);
    }

    void PriorityBorrowScheduler::enqueue(IoRequest *request)
    {
        int service_class = nice_to_service_class(request->submit_nice);
        FlowState *flow = find_or_alloc_flow(service_class, request->submit_pid, request->submit_nice);

        request->service_class = service_class;
        request->flow_next = nullptr;

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

    uint32 PriorityBorrowScheduler::pending_class_mask() const
    {
        uint32 mask = 0;
        for (int service_class = 0; service_class < k_class_count; ++service_class)
        {
            if (class_state_[service_class].flow_head != nullptr)
            {
                mask |= (1U << service_class);
            }
        }
        return mask;
    }

    IoRequest *PriorityBorrowScheduler::dequeue_next()
    {
        for (int service_class = 0; service_class < k_class_count; ++service_class)
        {
            ClassState &class_state = class_state_[service_class];
            if (class_state.flow_head == nullptr)
            {
                continue;
            }

            FlowState *selected_flow = class_state.rr_hint != nullptr ? class_state.rr_hint : class_state.flow_head;
            IoRequest *selected_request = pop_flow_head(selected_flow);
            if (selected_request == nullptr)
            {
                panic("PriorityBorrowScheduler::dequeue_next: empty selected flow");
            }

            finish_flow_dispatch(selected_flow);
            return selected_request;
        }

        return nullptr;
    }
} // namespace virtio_blk
