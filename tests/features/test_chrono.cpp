// chrono: datetime <-> time_point/duration.

#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "util.h"

namespace {
struct times {
  std::chrono::system_clock::time_point t;
  std::chrono::milliseconds d;
};
}

TEST_CASE("chrono: duration gets raw milliseconds since the epoch") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500");
  CHECK(v.d.count() == 1500);
}

TEST_CASE("chrono: serialize uses the '_' separator by default") {
  tavl::parser p;
  p.add_default_operator();

  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500");   // на входе допустим и 'T', и '_'

  std::string out;
  tavl::serialize(v, out);                                       // дефолт -> '_'
  CHECK(out == "t = 2026-06-14_12:30:00\nd = 1970-01-01_00:00:01.500");
}

TEST_CASE("chrono: the iso_datetime flag gives the 'T' separator") {
  tavl::parser p;
  p.add_default_operator();

  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14_12:30:00\nd = 1970-01-01_00:00:01.500");

  std::string out;
  tavl::serialize<tavl::sopts{ .iso_datetime = true }>(v, out);  // строгий ISO -> 'T'
  CHECK(out == "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500");
}

TEST_CASE("chrono: round-trip preserves the time point") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2000-01-02T03:04:05\nd = 1970-01-01T00:00:00");
  const auto r = tavl_test::round_trip(p, v);
  CHECK(r.t == v.t);
  CHECK(r.d == v.d);
}
