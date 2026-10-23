// SparkCap 内部：帧环形队列（src/ 私有头，模板实现）。
// 技术选型 §4 统一帧输出层：环形队列缓冲 N 帧——拉取模式按序消费不丢帧（N2）；
// 满时丢弃最旧（保最新语义）。
// 无内部锁：生产（采集线程）与消费（应用线程）由会话锁串行化。

#pragma once

#include <cstddef>
#include <vector>

namespace sparkcap {

template <typename Slot>
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity)
        : m_slots(capacity)
    {
    }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // 返回可写槽：未满为尾槽（计数 +1）；已满复用最旧槽（该帧被丢弃，dropped +1）。
    // 调用方（生产者）持会话锁期间完成写入。
    Slot* pushEmpty()
    {
        Slot* s;
        if (m_count == m_slots.size()) {
            s = &m_slots[m_head];
            m_head = (m_head + 1) % m_slots.size();
            ++m_dropped;
        } else {
            s = &m_slots[(m_head + m_count) % m_slots.size()];
            ++m_count;
        }
        return s;
    }

    // 按序弹出最旧帧到 *out（移动语义；Slot 的移动赋值负责释放旧帧）。
    bool pop(Slot* out)
    {
        if (m_count == 0)
            return false;
        *out = std::move(m_slots[m_head]);
        m_head = (m_head + 1) % m_slots.size();
        --m_count;
        return true;
    }

    // 队尾（最新）帧只读访问：回调交付用（持锁调用，回调期间槽不被移走）。
    const Slot* newest() const
    {
        return (m_count == 0) ? nullptr : &m_slots[(m_head + m_count - 1) % m_slots.size()];
    }

    void clear()
    {
        Slot tmp;
        while (pop(&tmp)) {
        }
    }

    bool empty() const { return m_count == 0; }
    size_t size() const { return m_count; }
    size_t dropped() const { return m_dropped; }  // N2 统计

private:
    std::vector<Slot> m_slots;
    size_t m_head = 0;
    size_t m_count = 0;
    size_t m_dropped = 0;
};

} // namespace sparkcap
