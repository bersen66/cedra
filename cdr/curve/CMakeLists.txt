cdr_cpp_library(
  NAME curve
  HDRS
    "curve.h"
    "interpolation/linear.h"
  SRCS
    "curve.cc"
    "interpolation/linear.cc"
  DEPS
    cdr::base
    cdr::calendar
  PUBLIC
)

cdr_cpp_test(
  NAME curve_test
  SRCS
    "curve_test.cc"
  DEPS
    cdr::curve
    GTest::gtest_main
)
