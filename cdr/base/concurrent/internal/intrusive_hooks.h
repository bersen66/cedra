// Ported from userver framework
#pragma once

#include <atomic>

namespace cdr::internal {

template<auto Member>
struct MemberHook final {
    template <typename T>
    auto& operator()(T& node) const noexcept {
        return node.*Member;
    }
};

template<typename T>
class SinglyLinkedHook final {
public:
    SinglyLinkedHook() = default;
private:

    template<typename U, typename HookExtractor>
    friend class IntrusiveStack;

    std::atomic<T*> next_{nullptr};
};

} // namespace cdr::internal
