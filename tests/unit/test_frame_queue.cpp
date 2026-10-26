// U5–U7：帧环形队列语义（docs/SparkCap-测试计划.md §5.1）

#include "../test_macros.h"

#include "frame_queue.h"

#include <string>

namespace {

struct TestSlot {
    std::string tag;
    int value = 0;
};

} // namespace

static void case_fifo_order()
{
    sparkcap::FrameQueue<TestSlot> q(4);
    for (int i = 1; i <= 3; ++i)
        *q.pushEmpty() = {"slot", i};
    CHECK_EQ(q.size(), static_cast<size_t>(3));

    TestSlot s;
    for (int i = 1; i <= 3; ++i) {  // 按序出队（U5）
        CHECK(q.pop(&s));
        CHECK_EQ(s.value, i);
    }
    CHECK(!q.pop(&s));  // 空
    CHECK(q.empty());
}

static void case_overflow_drops_oldest()
{
    sparkcap::FrameQueue<TestSlot> q(3);
    for (int i = 1; i <= 5; ++i)  // 5 进 3 容量：丢最旧 2 个
        *q.pushEmpty() = {"slot", i};
    CHECK_EQ(q.dropped(), static_cast<size_t>(2));  // U6
    CHECK_EQ(q.size(), static_cast<size_t>(3));

    TestSlot s;
    CHECK(q.pop(&s));
    CHECK_EQ(s.value, 3);  // 1、2 被丢，最旧剩 3
    CHECK(q.pop(&s));
    CHECK_EQ(s.value, 4);
    CHECK(q.pop(&s));
    CHECK_EQ(s.value, 5);  // 最新保留
}

static void case_newest_and_clear()
{
    sparkcap::FrameQueue<TestSlot> q(4);
    const TestSlot* n = q.newest();
    CHECK(n == nullptr);  // 空时 newest 为 null

    for (int i = 1; i <= 3; ++i)
        *q.pushEmpty() = {"slot", i};
    n = q.newest();
    CHECK(n != nullptr && n->value == 3);  // 队尾最新

    q.clear();
    CHECK(q.empty());
    CHECK_EQ(q.dropped(), static_cast<size_t>(0));  // clear 不算 dropped（U7）
}

int main()
{
    g_suiteName = "unit_frame_queue";
    RUN_CASE(case_fifo_order);
    RUN_CASE(case_overflow_drops_oldest);
    RUN_CASE(case_newest_and_clear);
    TEST_MAIN_END();
}
