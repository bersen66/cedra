cdr_cpp_library(
  NAME base
  HDRS
    "generator.h"
    "check.h"
    "internal/check_impl.h"
    "numeric.h"
  SRCS
    "internal/check_impl.cc"
  DEPS
    cdr::types
  PUBLIC
)
