#pragma once

#include <cdr/base/hardware_interference_size.h>
#include <cdr/base/internal/export.h>
#include <cdr/types/integers.h>
#include <utility>

#include <array>
#include <atomic>
#include <thread>

namespace cdr {

struct TickRAII;

class CDR_BASE_EXPORT ShardedCounter final {
public:

    static constexpr std::size_t kShardsCount = 32;

    void Increment();
    void Decrement();
    void Reset();

    [[nodiscard]] u64 ApproximateSize() const noexcept;

    [[nodiscard]] bool Empty() const noexcept;

    ShardedCounter() = default;

    // Non-copyable
    ShardedCounter(const ShardedCounter&) = delete;
    ShardedCounter& operator=(const ShardedCounter&) = delete;

    // Non-movable
    ShardedCounter(ShardedCounter&&) noexcept = delete;
    ShardedCounter& operator=(ShardedCounter&&) noexcept = delete;

private:

    struct alignas(kDestructiveInterferenceSize) Shard {
        std::atomic<i64> counter{0};
    };

    static_assert(sizeof(Shard) == kDestructiveInterferenceSize);
private:
    std::array<Shard, kShardsCount> shards_;
    std::hash<std::thread::id> hasher_;
};

struct CDR_BASE_EXPORT [[nodiscard]] TickRAII final {
public:
    explicit TickRAII(ShardedCounter& counter)
        : protected_(&counter), moved(false)
    {
        protected_->Increment();
    }

    TickRAII(const TickRAII&) = delete;
    TickRAII& operator=(const TickRAII&) = delete;

    TickRAII(TickRAII&& other) noexcept
        : protected_(other.protected_), moved(std::exchange(other.moved, true))
    {}

    TickRAII& operator=(TickRAII&& other) noexcept {
        if (&other != this) {
            if (!moved) {
                protected_->Decrement();
            }

            protected_ = std::exchange(other.protected_, nullptr);
            moved = std::exchange(other.moved, true);
        }
        return *this;
    }

    ~TickRAII() {
        if (!moved) {
            protected_->Decrement();
        }
    }

private:
    ShardedCounter* protected_;
    bool moved;
};


}  // namespace cdr
