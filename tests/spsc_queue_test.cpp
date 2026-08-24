// The control-plane <-> audio-thread transport (design_doc.md sec. 7.4.2). What matters here
// is that it never allocates, that it refuses work instead of growing, and that `drain`
// honours a bound.

#include "aip/rt/spsc_queue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace aip;

namespace {

struct Command {
    int kind = 0;
    int value = 0;
};

} // namespace

TEST_CASE("spsc queue refuses to grow past its fixed capacity", "[rt][spsc]") {
    rt::SpscQueue<Command, 8> queue;
    REQUIRE(rt::SpscQueue<Command, 8>::capacity() == 7);
    REQUIRE(queue.empty());

    for (int i = 0; i < 7; ++i) {
        REQUIRE(queue.push(Command{1, i}));
    }
    REQUIRE(queue.size() == 7);
    // Full: the producer is told so and decides what to drop. No reallocation, ever.
    REQUIRE_FALSE(queue.push(Command{1, 99}));

    Command out;
    REQUIRE(queue.pop(out));
    REQUIRE(out.value == 0);
    REQUIRE(queue.push(Command{1, 7}));
}

TEST_CASE("drain performs bounded work", "[rt][spsc]") {
    rt::SpscQueue<Command, 64> queue;
    for (int i = 0; i < 40; ++i) {
        REQUIRE(queue.push(Command{1, i}));
    }

    // Sec. 7.4.2: a backlogged queue is drained a fixed maximum per block, not caught up in one go.
    std::vector<int> seen;
    const std::size_t consumed = queue.drain(10, [&](const Command& c) { seen.push_back(c.value); });

    REQUIRE(consumed == 10);
    REQUIRE(seen.size() == 10);
    REQUIRE(seen.front() == 0);
    REQUIRE(seen.back() == 9);
    REQUIRE(queue.size() == 30);
}

TEST_CASE("spsc queue preserves order across threads", "[rt][spsc]") {
    constexpr int kCount = 100000;
    rt::SpscQueue<Command, 1024> queue;
    std::atomic<bool> failed{false};

    std::thread consumer([&] {
        int expected = 0;
        Command item;
        while (expected < kCount) {
            if (queue.pop(item)) {
                if (item.value != expected) {
                    failed.store(true);
                    return;
                }
                ++expected;
            }
        }
    });

    for (int i = 0; i < kCount;) {
        if (queue.push(Command{1, i})) {
            ++i;
        }
    }
    consumer.join();

    REQUIRE_FALSE(failed.load());
}
