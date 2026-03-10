#pragma once
#include <cdr/types/concepts.h>

namespace cdr {

template<typename T>
concept NumberLike = Numeric<T> || requires(T obj) {
    std::is_same_v<T, Percent>;
};


template<NumberLike T>
[[nodiscard]] inline constexpr T MidPoint(const T& lhs, const T& rhs) noexcept {
    return (lhs / 2) + (rhs / 2);
}

template<NumberLike T>
[[nodiscard]] inline constexpr std::pair<T, bool> Clamp(const T& value, const T& left, const T& right) noexcept {
    if (value <= left) { return std::make_pair(left, true); }
    if (value >= right) { return std::make_pair(right, true); }
    return std::make_pair(value, false);
}

} // namespace cdr