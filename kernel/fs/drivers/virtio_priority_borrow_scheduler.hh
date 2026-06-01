#pragma once

#include "types.hh"
#include "proc/proc.hh"
#include "virtio_io_request.hh"

namespace virtio_blk
{
    /**
     * @brief 优先级分层 + 空槽借用的软件调度器。
     *
     * 设计目标：
     * 1. 高优先级 class 只要还有 pending 请求，就优先获得设备派发机会；
     * 2. 高优先级 class 当前派发空后，低优先级 class 可以借用剩余派发槽；
     * 3. class 内使用 per-flow 队列和轮转，避免同优先级进程互相饿死；
     * 4. 不依赖 STL、堆分配与静态构造，便于内核早期初始化与跨架构复用。
     */
    class PriorityBorrowScheduler
    {
    public:
        static constexpr int k_class_count = 8;
        static constexpr int k_max_flows = proc::num_process * 2;

        void reset();
        void enqueue(IoRequest *request);
        IoRequest *dequeue_next();
        uint32 pending_class_mask() const;

        static int nice_to_service_class(int nice);

    private:
        struct FlowState;

        struct ClassState
        {
            FlowState *flow_head;
            FlowState *flow_tail;
            FlowState *rr_hint;
        };

        struct FlowState
        {
            bool in_use;
            uint pid;
            int service_class;
            int submit_nice;
            IoRequest *head;
            IoRequest *tail;
            FlowState *next;
            FlowState *prev;
        };

        FlowState *find_or_alloc_flow(int service_class, uint pid, int submit_nice);
        void release_flow(ClassState &class_state, FlowState *flow);
        FlowState *next_flow(ClassState &class_state, FlowState *flow);
        IoRequest *pop_flow_head(FlowState *flow);
        void finish_flow_dispatch(FlowState *flow);

        ClassState class_state_[k_class_count];
        FlowState flow_pool_[k_max_flows];
    };
} // namespace virtio_blk
