cdr_cpp_library(
  NAME base
  HDRS
    "generator.h"
    "internal/generator_impl.h"
    "internal/export.h"
    "check.h"
    "internal/check_impl.h"
    "hardware_interference_size.h"
    "aligned_alloc.h"
    "concurrent/internal/tagged_ptr.h"
    "concurrent/internal/intrusive_hooks.h"
    "concurrent/internal/intrusive_stack.h"
    "concurrent/rcu.h"
    "concurrent/sharded_counter.h"
  SRCS
    "concurrent/sharded_counter.cc"
    "internal/check_impl.cc"
  DEPS
    cdr::types
  PUBLIC
)

cdr_cpp_test(
  NAME base_concurrent_test
  SRCS
    "concurrent/rcu_test.cc"
  DEPS
    cdr::base
    GTest::gtest_main
    GTest::gmock
)

cdr_cpp_executable(
  NAME
    generator_benchmark
  SRCS
    "internal/generator_impl_bench.cc"
  DEPS
    cdr::base
    benchmark::benchmark
  COPTS
    "-O3"
  BENCH
)
