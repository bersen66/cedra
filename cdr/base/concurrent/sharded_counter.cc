#include <cdr/base/concurrent/sharded_counter.h>

namespace cdr {

void ShardedCounter::Increment() {
    const std::thread::id this_thread_id = std::this_thread::get_id();
    const u64 shard = hasher_(this_thread_id) % kShardsCount;
    shards_[shard].counter.fetch_add(1, std::memory_order_release);
}

bool ShardedCounter::Empty() const noexcept {
    return ApproximateSize() == 0;
}

void ShardedCounter::Decrement() {
    const std::thread::id this_thread_id = std::this_thread::get_id();
    const u64 shard = hasher_(this_thread_id) % kShardsCount;
    shards_[shard].counter.fetch_sub(1, std::memory_order_release);
}

u64 ShardedCounter::ApproximateSize() const noexcept {
    u64 result = 0;

    for (const auto& [atomic_counter] : shards_) {
        if (const i64 v = atomic_counter.load(std::memory_order_acquire); v > 0) {
            result += v;
        }
    }

    return result;
}

void ShardedCounter::Reset() {
    for (auto& [counter] : shards_) {
        counter.store(0, std::memory_order_release);
    }
}

} // namespace cdr
