#pragma once

#include "types.hh"
#include "proc/proc.hh"
#include "virtio_io_request.hh"

namespace virtio_blk
{
    /**
     * @brief mClock 风格的软件调度器。
     *
     * 设计目标：
     * 1. class 间通过 R/W/L 三类标签逼近保底 + 权重共享 + 上限控制；
     * 2. class 内通过 per-flow 队列避免单 FIFO 带来的头阻塞与同类不公平；
     * 3. 不依赖 STL、堆分配与静态构造，便于内核早期初始化与跨架构复用。
     */
    class MClockScheduler
    {
    public:
        static constexpr int k_class_count = 8;
        static constexpr int k_max_flows = proc::num_process * 2;
        static constexpr uint64 k_unlimited_bps = 0;

        struct DispatchDecision
        {
            IoRequest *request;
            uint64 next_gate_us;
        };

        void reset();
        void enqueue(IoRequest *request, uint64 now_us, uint64 ewma_bps);
        DispatchDecision dequeue_next(uint64 now_us);

        static int nice_to_service_class(int nice);
        static uint64 default_ewma_bps();

    private:
        struct FlowState;

        struct ClassState
        {
            FlowState *flow_head;
            FlowState *flow_tail;
            FlowState *rr_hint;
            uint64 last_r_tag_us;
            uint64 last_w_tag_us;
            uint64 last_l_tag_us;
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

        struct PickState
        {
            IoRequest *best_reservation;
            FlowState *best_reservation_flow;
            IoRequest *best_weight;
            FlowState *best_weight_flow;
            uint64 earliest_gate;
        };

        static uint64 ceil_div_u64(uint64 numerator, uint64 denominator);
        static uint64 bytes_to_service_us(uint64 bytes, uint64 bps);
        static uint64 weighted_service_us(uint64 bytes, uint32 weight, uint64 ewma_bps);

        FlowState *find_or_alloc_flow(int service_class, uint pid, int submit_nice);
        void release_flow(ClassState &class_state, FlowState *flow);
        FlowState *next_flow(ClassState &class_state, FlowState *flow);
        IoRequest *pop_flow_head(FlowState *flow);
        void finish_flow_dispatch(FlowState *flow);
        PickState pick_locked(uint64 now_us);

        ClassState class_state_[k_class_count];
        FlowState flow_pool_[k_max_flows];
    };
} // namespace virtio_blk
