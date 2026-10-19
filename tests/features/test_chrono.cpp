// chrono: datetime <-> time_point/duration.

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include "util.h"

namespace {
struct times {
  std::chrono::system_clock::time_point t;
  std::chrono::milliseconds d;
};
struct nanos { std::chrono::nanoseconds d; };
}

TEST_CASE("chrono: duration gets raw milliseconds since the epoch") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500Z");
  CHECK(v.d.count() == 1500);
}

TEST_CASE("chrono: serialize uses the '_' separator by default") {
  tavl::parser p;
  p.add_default_operator();

  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500Z");   // both 'T' and '_' are accepted on input

  std::string out;
  tavl::serialize(v, out);                                       // default -> '_'
  CHECK(out == "t = 2026-06-14_12:30:00\nd = 1970-01-01_00:00:01.500Z");
}

TEST_CASE("chrono: the iso_datetime flag gives the 'T' separator") {
  tavl::parser p;
  p.add_default_operator();

  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2026-06-14_12:30:00\nd = 1970-01-01_00:00:01.500Z");

  std::string out;
  tavl::serialize<tavl::sopts{ .iso_datetime = true }>(v, out);  // strict ISO-looking output -> 'T'
  CHECK(out == "t = 2026-06-14T12:30:00\nd = 1970-01-01T00:00:01.500Z");
}

TEST_CASE("chrono: round-trip preserves the time point") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2000-01-02T03:04:05\nd = 1970-01-01T00:00:00Z");
  const auto r = tavl_test::round_trip(p, v);
  CHECK(r.t == v.t);
  CHECK(r.d == v.d);
}

TEST_CASE("chrono: nine fractional digits round-trip without loss") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<nanos>(p,
      "d = 1970-01-01T00:00:01.123456789Z");
  CHECK(v.d.count() == 1123456789);
  CHECK(tavl_test::to_text(v) == "d = 1970-01-01_00:00:01.123456789Z");
  CHECK(tavl_test::round_trip(p, v).d == v.d);
}

TEST_CASE("chrono: negative epoch fractions retain nanoseconds") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<nanos>(p,
      "d = 1969-12-31T23:59:59.123456789Z");
  CHECK(v.d.count() == -876543211);
  CHECK(tavl_test::to_text(v) == "d = 1969-12-31_23:59:59.123456789Z");
}

TEST_CASE("chrono: coarse target reports fractional precision loss") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 1970-01-01T00:00:00Z\nd = 1970-01-01T00:00:01.123456789Z", ctx);
  CHECK(v.d.count() == 0);
  bool found = false;
  for (const auto& d : ctx.diagnostics)
    found |= d.error.type == tavl::error_type::err_datetime_precision_loss;
  CHECK(found);
}

TEST_CASE("chrono: explicit UTC serialization appends Z") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 2000-01-02T03:04:05Z\nd = 1970-01-01T00:00:00Z");
  CHECK(tavl_test::to_text<tavl::sopts{.utc_datetime = true}>(v) ==
      "t = 2000-01-02_03:04:05Z\nd = 1970-01-01_00:00:00Z");
}

TEST_CASE("chrono: dates outside nanosecond epoch range are diagnosed") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  const auto v = tavl_test::deserialize_all<times>(p,
      "t = 9999-01-01T00:00:00Z\nd = 1970-01-01T00:00:00Z", ctx);
  CHECK(v.t.time_since_epoch().count() == 0);
  bool found = false;
  for (const auto& d : ctx.diagnostics)
    found |= d.error.type == tavl::error_type::err_datetime_out_of_range;
  CHECK(found);
}

#if !defined(_WIN32)
TEST_CASE("chrono: timezone-free datetime uses system local time") {
  const char* prior = std::getenv("TZ");
  struct restore_tz {
    std::string saved;
    bool had_prior;
    ~restore_tz() {
      if (had_prior) setenv("TZ", saved.c_str(), 1);
      else unsetenv("TZ");
      tzset();
    }
  } restore{prior ? prior : "", prior != nullptr};
  REQUIRE(setenv("TZ", "UTC-6", 1) == 0);
  tzset();
  tavl::iso_datetime local, utc;
  REQUIRE(tavl::detail::is_datetime("2000-01-01T00:00:00", local));
  REQUIRE(tavl::detail::is_datetime("2000-01-01T00:00:00Z", utc));
  const auto local_ns = tavl::detail::iso_datetime_to_unix_ns(local);
  const auto utc_ns = tavl::detail::iso_datetime_to_unix_ns(utc);
  REQUIRE(local_ns.has_value());
  REQUIRE(utc_ns.has_value());
  CHECK(*utc_ns - *local_ns == std::chrono::hours{6});
}
#endif
