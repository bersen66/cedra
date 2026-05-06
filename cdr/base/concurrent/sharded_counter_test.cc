#include <cdr/base/concurrent/sharded_counter.h>
#include <gtest/gtest.h>

#include <random>

TEST(ShardedCounterTest, SanityCheck) {
    cdr::ShardedCounter sharded_counter;
    ASSERT_EQ(sharded_counter.ApproximateSize(), 0) << "Nothing to add";
    sharded_counter.Increment();
    ASSERT_EQ(sharded_counter.ApproximateSize(), 1) << "We must see the increment";
    sharded_counter.Decrement();
    ASSERT_EQ(sharded_counter.ApproximateSize(), 0) << "We must see the decrement";
}

TEST(ShardedCounterTest, ConcurrentIncrements) {
    static constexpr std::size_t kThreads = 16;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    cdr::ShardedCounter sharded_counter;
    std::atomic<std::size_t> etalon{0};
    for (std::size_t i = 0; i < kThreads; i++) {
        threads.emplace_back([&sharded_counter, &etalon] () {
            for (std::size_t i = 0; i < 1'000'000; i++) {
                etalon.fetch_add(1, std::memory_order_relaxed);
                sharded_counter.Increment();
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(etalon.load(), sharded_counter.ApproximateSize());
}


TEST(ShardedCounterTest, ConcurrentDecrementsAndIncrements) {
    static constexpr std::size_t kThreads = 16;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    cdr::ShardedCounter sharded_counter;
    std::atomic<i64> increments{0};
    std::atomic<i64> decrements{0};

    for (std::size_t i = 0; i < kThreads; i++) {
        threads.emplace_back([&sharded_counter, &increments, &decrements] () {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<> dis(0.0, 1.0);

            for (std::size_t i = 0; i < 1'000'000; i++) {
                increments.fetch_add(1, std::memory_order_relaxed);
                sharded_counter.Increment();

                if (dis(gen) <= 0.6) {
                    sharded_counter.Decrement();
                    decrements.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    const i64 diff = increments.load() - decrements.load();
    EXPECT_EQ(sharded_counter.ApproximateSize(), diff);
}

TEST(ShardedCounterTest, HighContention) {
    cdr::ShardedCounter sharded_counter;

    std::jthread increment_thread([&sharded_counter] () {
        for (i32 i = 0; i < 10'000; i++) {
            sharded_counter.Increment();
        }
    });
    increment_thread.join();


    std::stop_source stop_request;
    std::atomic<u64> reads_num{0};
    std::atomic<u64> score_num{0};
    std::jthread accumulator_thread([&] () {
        [[maybe_unused]] u64 score = 0;
        u64 iterations = 0;
        while (!stop_request.stop_requested()) {
            score += sharded_counter.ApproximateSize();
            iterations++;
        }
        reads_num.store(iterations);
        score_num.store(score);
    });
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
    stop_request.request_stop();
    EXPECT_GE(reads_num.load(), 500'000);
    EXPECT_GE(score_num.load(), 5'000'000'000);
}
