// Ported and adapted from userver
#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

#include <cdr/base/check.h>
#include <cdr/base/concurrent/sharded_counter.h>
#include <cdr/base/concurrent/internal/intrusive_hooks.h>
#include <cdr/base/concurrent/internal/intrusive_stack.h>

namespace cdr {

namespace internal {

template<typename T>
struct SnapshotRecord {
    std::optional<T> data;
    ShardedCounter readers_indicator;
    internal::SinglyLinkedHook<SnapshotRecord> free_list_hook;
    SnapshotRecord* next_retired{nullptr};
};

template<typename T>
struct SnapshotRecordFreeList final {
    SnapshotRecordFreeList() = default;

    struct FreeListHookExtractor {
        auto& operator()(SnapshotRecord<T>& snapshot) const noexcept {
            return snapshot.free_list_hook;
        }
    };

    ~SnapshotRecordFreeList() {
        list.DisposeUnsafe([](SnapshotRecord<T>& record) { delete &record; });
    }

    internal::IntrusiveStack<SnapshotRecord<T>, FreeListHookExtractor> list;
};

template <typename T>
class SnapshotRecordRetiredList final {
public:
    bool IsEmpty() const noexcept {
        return head_ == nullptr;
    }

    void Push(SnapshotRecord<T>& record) noexcept {
        record.next_retired = head_;
        head_ = &record;
    }

    template <typename Predicate, typename Disposer>
    void RemoveAndDisposeIf(Predicate predicate, Disposer disposer) {
        SnapshotRecord<T>** ptr = &head_;

        while (*ptr != nullptr) {
            SnapshotRecord<T>* current = *ptr;

            if (predicate(*current)) {
                *ptr = std::exchange(current->next_retired, nullptr);
                disposer(*current);
            } else {
                ptr = &current->next_retired;
            }
        }
    }

private:
    SnapshotRecord<T>* head_{nullptr};
};

} // namespace internal


template <typename T>
class SnapshotHandle final {
public:

    SnapshotHandle(SnapshotHandle&& other) noexcept
        : record_(std::exchange(other.record_, nullptr))
        , free_list_(std::exchange(other.free_list_, nullptr))
    {}

    ~SnapshotHandle() {
        if (record_) {
            CDR_CHECK(free_list_);
            record_->data.reset();
            free_list_->list.Push(*record_);
        }
    }

private:
    template <typename, typename>
    friend class RcuVariable;

    explicit SnapshotHandle(internal::SnapshotRecord<T>& record,
                            internal::SnapshotRecordFreeList<T>& free_list) noexcept
        : record_(&record), free_list_(&free_list)
    {}

    internal::SnapshotRecord<T>* record_;
    internal::SnapshotRecordFreeList<T>* free_list_;
};


struct SyncDeleter {
    template <typename T>
    void Delete(SnapshotHandle<T>&& handle) noexcept {
        [[maybe_unused]] auto h = std::move(handle);
    }
};


struct DefaultRcuTraits {
    using MutexType = std::mutex;
    using DeleterType = SyncDeleter;
};

template<typename T, typename RcuTraits>
class RcuVariable;

template <typename T, typename RcuTraits>
class [[nodiscard]] ReadablePtr final {
public:
    explicit ReadablePtr(const RcuVariable<T, RcuTraits>& var) {
        Acquire(var);
    }

    ReadablePtr(ReadablePtr&&) noexcept = default;
    ReadablePtr& operator=(ReadablePtr&&) noexcept = default;
    ReadablePtr(const ReadablePtr&) = default;
    ReadablePtr& operator=(const ReadablePtr&) = default;

    const T* Get() const {
        CDR_CHECK(ptr_);
        return ptr_;
    }

    const T& operator*() const { return *Get(); }
    const T* operator->() const { return Get(); }

private:
    void Acquire(const RcuVariable<T, RcuTraits>& var) {
        while (true) {
            auto* record = var.current_.load(std::memory_order_acquire);

            guard_.emplace(record->readers_indicator);

            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (record == var.current_.load(std::memory_order_seq_cst)) {
                ptr_ = &*record->data;
                break;
            }

            guard_.reset();
        }
    }

    const T* ptr_{nullptr};
    std::optional<TickRAII> guard_;
};


template <typename T, typename RcuTraits>
class [[nodiscard]] WritablePtr final {
public:
    using MutexType = typename RcuTraits::MutexType;

    explicit WritablePtr(RcuVariable<T, RcuTraits>& var)
        : var_(var)
        , lock_(var.mutex_)
        , record_(&var.EmplaceSnapshot(*var.current_.load()->data))
    {}

    template <typename... Args>
    WritablePtr(RcuVariable<T, RcuTraits>& var, std::in_place_t, Args&&... args)
        : var_(var)
        , lock_(var.mutex_)
        , record_(&var.EmplaceSnapshot(std::forward<Args>(args)...))
    {}

    T* operator->() { return &*record_->data; }
    T& operator*() { return *record_->data; }

    void Commit() {
        var_.DoAssign(*record_, lock_);
        record_ = nullptr;
    }

private:
    RcuVariable<T, RcuTraits>& var_;
    std::unique_lock<MutexType> lock_;
    internal::SnapshotRecord<T>* record_;
};


template<typename T, typename RcuTraits = DefaultRcuTraits>
class RcuVariable {
public:
    using MutexType = typename RcuTraits::MutexType;
    using DeleterType = typename RcuTraits::DeleterType;

    template <typename... Args>
    explicit RcuVariable(Args&&... args) {
        auto* record = new internal::SnapshotRecord<T>;
        record->data.emplace(std::forward<Args>(args)...);
        current_.store(record, std::memory_order_release);
    }

    ~RcuVariable() {
        Cleanup();
        auto* ptr = current_.load();
        delete ptr;
    }

    ReadablePtr<T, RcuTraits> Read() const {
        return ReadablePtr<T, RcuTraits>(*this);
    }

    WritablePtr<T, RcuTraits> StartWrite() {
        return WritablePtr<T, RcuTraits>(*this);
    }

    template <typename... Args>
    WritablePtr<T, RcuTraits> StartWriteEmplace(Args&&... args) {
        return WritablePtr<T, RcuTraits>(*this, std::in_place, std::forward<Args>(args)...);
    }

    void Assign(T value) {
        StartWriteEmplace(std::move(value)).Commit();
    }

    template <typename... Args>
    void Emplace(Args&&... args) {
        StartWriteEmplace(std::forward<Args>(args)...).Commit();
    }

    void Cleanup() {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        ScanRetiredList(lock);
    }

private:
    friend class ReadablePtr<T, RcuTraits>;
    friend class WritablePtr<T, RcuTraits>;

    template <typename... Args>
    internal::SnapshotRecord<T>& EmplaceSnapshot(Args&&... args) {
        auto* reused = free_list_.list.TryPop();
        auto& record = reused ? *reused : *new internal::SnapshotRecord<T>;

        CDR_CHECK(!record.data);

        try {
            record.data.emplace(std::forward<Args>(args)...);
        } catch (...) {
            free_list_.list.Push(record);
            throw;
        }

        return record;
    }

    void DoAssign(internal::SnapshotRecord<T>& new_snapshot,
                  std::unique_lock<MutexType>& lock) {
        CDR_CHECK(lock.owns_lock());

        auto* old = current_.load(std::memory_order_acquire);
        current_.store(&new_snapshot, std::memory_order_seq_cst);

        retired_list_.Push(*old);
        ScanRetiredList(lock);
    }

    void ScanRetiredList(std::unique_lock<MutexType>& lock) noexcept {
        CDR_CHECK(lock.owns_lock());
        if (retired_list_.IsEmpty()) return;

        std::atomic_thread_fence(std::memory_order_seq_cst);

        retired_list_.RemoveAndDisposeIf(
            [](internal::SnapshotRecord<T>& record) {
                return record.readers_indicator.Empty();
            },
            [this](internal::SnapshotRecord<T>& record) {
                DeleteSnapshot(record);
            }
        );
    }

    void DeleteSnapshot(internal::SnapshotRecord<T>& record) noexcept {
        deleter_.Delete(SnapshotHandle<T>{record, free_list_});
    }

private:
    mutable MutexType mutex_{};
    internal::SnapshotRecordFreeList<T> free_list_;
    internal::SnapshotRecordRetiredList<T> retired_list_;
    DeleterType deleter_{};
    std::atomic<internal::SnapshotRecord<T>*> current_{nullptr};
};

} // namespace cdr
