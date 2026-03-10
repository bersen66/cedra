cdr_cpp_library(
  NAME types
  HDRS
    "concepts.h"
    "percent.h"
    "integers.h"
    "floats.h"
    "types.h"
    "expect.h"
    "errors.h"
  SRCS
    "percent.cc"
  PUBLIC
)

cdr_cpp_test(
    NAME types_test
    SRCS
      "expect_tests.cc"
    DEPS
        cdr::types
        GTest::gtest_main
)
