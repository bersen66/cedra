#pragma once

#include <cstddef>
#include <new>

namespace cdr {

#if defined(__s390__) || defined(__s390x__)
inline constexpr std::size_t kDestructiveInterferenceSize = 256;
#elif defined(powerpc) || defined(__powerpc__) || defined(__ppc__)
inline constexpr std::size_t kDestructiveInterferenceSize = 128;
#else
inline constexpr std::size_t kDestructiveInterferenceSize = 64;
#endif


}  // namespace cdr