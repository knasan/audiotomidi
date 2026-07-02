#include <catch2/catch_test_macros.hpp>

#include "dsp/spscRingBuffer.hpp"

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("SpscRingBuffer push and pop single-threaded", "[ringbuffer]") {
    dsp::SpscRingBuffer<int, 8> buffer;

    REQUIRE(buffer.try_push(42));
    REQUIRE(buffer.try_push(7));

    int value = 0;
    REQUIRE(buffer.try_pop(value));
    REQUIRE(value == 42);
    REQUIRE(buffer.try_pop(value));
    REQUIRE(value == 7);
    REQUIRE_FALSE(buffer.try_pop(value));
}

TEST_CASE("SpscRingBuffer reports full when capacity exceeded", "[ringbuffer]") {
    dsp::SpscRingBuffer<int, 4> buffer;

    REQUIRE(buffer.try_push(1));
    REQUIRE(buffer.try_push(2));
    REQUIRE(buffer.try_push(3));
    REQUIRE_FALSE(buffer.try_push(4));
}

TEST_CASE("SpscRingBuffer producer consumer threads", "[ringbuffer]") {
    dsp::SpscRingBuffer<int, 256> buffer;
    constexpr int kTotal = 10'000;

    std::vector<int> received;
    received.reserve(static_cast<std::size_t>(kTotal));

    std::atomic<bool> producer_done{false};

    std::thread consumer([&]() {
        int value = 0;
        while (!producer_done.load(std::memory_order_acquire) ||
               buffer.size_approx() > 0) {
            if (buffer.try_pop(value)) {
                received.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < kTotal; ++i) {
        while (!buffer.try_push(i)) {
            std::this_thread::yield();
        }
    }
    producer_done.store(true, std::memory_order_release);
    consumer.join();

    REQUIRE(static_cast<int>(received.size()) == kTotal);
    for (int i = 0; i < kTotal; ++i) {
        REQUIRE(received[static_cast<std::size_t>(i)] == i);
    }
}
