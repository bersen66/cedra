// Ported from userver framework
#pragma once

#include <cstdint>
#include <cdr/base/concurrent/internal/tagged_ptr.h>
#include <cdr/base/concurrent/internal/intrusive_hooks.h>


namespace cdr::internal {

template <typename T, typename HookExtractor>
class IntrusiveStack final {
    static_assert(std::is_empty_v<HookExtractor>);

public:
    constexpr IntrusiveStack() = default;

    IntrusiveStack(IntrusiveStack&&) = delete;
    IntrusiveStack& operator=(IntrusiveStack&&) = delete;

    void Push(T& node) noexcept {
        CDR_CHECK(GetNext(node).load(std::memory_order_relaxed) == nullptr) << "This node is already contained in an IntrusiveStack";

        NodeTaggedPtr expected = stack_head_.load();
        while (true) {
            GetNext(node).store(expected.GetDataPtr());
            const NodeTaggedPtr desired(&node, expected.GetTag());
            if (stack_head_.compare_exchange_weak(expected, desired)) {
                break;
            }
        }
    }

    T* TryPop() noexcept {
        NodeTaggedPtr expected = stack_head_.load();
        while (true) {
            T* const expected_ptr = expected.GetDataPtr();
            if (!expected_ptr) {
                return nullptr;
            }
            const NodeTaggedPtr desired(GetNext(*expected_ptr).load(), expected.GetNextTag());
            if (stack_head_.compare_exchange_weak(expected, desired)) {
                // 'relaxed' is OK, because popping a node must happen-before pushing it
                GetNext(*expected_ptr).store(nullptr, std::memory_order_relaxed);
                return expected_ptr;
            }
        }
    }

    template <typename Func>
    void WalkUnsafe(const Func& func) {
        DoWalk<T&>(func);
    }

    template <typename Func>
    void WalkUnsafe(const Func& func) const {
        DoWalk<const T&>(func);
    }

    template <typename DisposerFunc>
    void DisposeUnsafe(const DisposerFunc& disposer) noexcept {
        T* iter = stack_head_.load().GetDataPtr();
        stack_head_.store(nullptr);
        while (iter) {
            T* const old_iter = iter;
            iter = GetNext(*iter).load();
            disposer(*old_iter);
        }
    }

    std::size_t GetSizeUnsafe() const noexcept {
        std::size_t size = 0;
        WalkUnsafe([&](auto& /*item*/) { ++size; });
        return size;
    }

private:

    using NodeTaggedPtr = TaggedPtr<T>;

    static_assert(std::atomic<NodeTaggedPtr>::is_always_lock_free);
    static_assert(std::has_unique_object_representations_v<NodeTaggedPtr>);

    static std::atomic<T*>& GetNext(T& node) noexcept {
        SinglyLinkedHook<T>& hook = HookExtractor{}(node);
        return hook.next_;
    }

    template <typename U, typename Func>
    void DoWalk(const Func& func) const {
        for (auto* iter = stack_head_.load().GetDataPtr(); iter; iter = GetNext(*iter).load()) {
            func(static_cast<U>(*iter));
        }
    }

    std::atomic<NodeTaggedPtr> stack_head_{nullptr};
};

}  // namespace cdr::internal
