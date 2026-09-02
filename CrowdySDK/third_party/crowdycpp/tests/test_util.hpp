#pragma once

#include <cstdio>
#include <cstdlib>

/// Minimal assertion helpers for the unit tests (no framework dependency).
#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      std::fprintf(stderr, "CHECK failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    if (!((a) == (b))) {                                                         \
      std::fprintf(stderr, "CHECK_EQ failed: %s == %s at %s:%d\n", #a, #b, __FILE__, __LINE__); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)
