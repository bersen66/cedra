// Ported from userver framework
#pragma once

#include <cstdint>

#include <cdr/base/check.h>

namespace cdr::internal {

template <typename T>
class TaggedPtr final {
private:

    static_assert(sizeof(std::uintptr_t) <= sizeof(std::uint64_t));
    static constexpr std::uint64_t kTagShift = 48;

public:
    using Tag = std::uint16_t;

    constexpr TaggedPtr(std::nullptr_t) noexcept
        : impl_(0)
    {}

    TaggedPtr(T* ptr, Tag tag)
        : impl_(reinterpret_cast<std::uintptr_t>(ptr) | (std::uint64_t{tag} << kTagShift))
    {
        CDR_CHECK(!(reinterpret_cast<std::uintptr_t>(ptr) & 0xffff'0000'0000'0000));
    }

    T* GetDataPtr() const noexcept {
        return reinterpret_cast<T*>(static_cast<std::uintptr_t>(impl_ & ((std::uint64_t{1} << kTagShift) - 1)));
    }

    Tag GetTag() const noexcept {
        return static_cast<Tag>(impl_ >> kTagShift);
    }

    Tag GetNextTag() const noexcept {
        return static_cast<Tag>(GetTag() + 1);
    }

private:
    std::uint64_t impl_;
};

}  // namespace cdr::internal
