// serialize: текст для разных опций + крупная структура.

#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <optional>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <memory>
#include <chrono>

#include "util.h"

namespace {
struct vec3 { int x, y, z; };
struct doc  { std::string name; vec3 v; std::vector<int> nums; };

// крупная структура со ВСЕМИ поддерживаемыми категориями типов
struct inner { int a; std::string b; };
struct mega {
  bool flag;
  int i; unsigned u; double d;
  std::string s;
  std::array<char, 8> buf;
  std::optional<int> opt_set;
  std::optional<int> opt_null;
  std::pair<std::string, int> pr;
  std::tuple<int, std::string, bool> tup;
  std::array<int, 3> arr;
  std::vector<int> vec;
  std::map<std::string, int> mp;
  std::set<int> st;
  inner nested;
};

inline mega make_mega() {
  return mega{
    /*flag*/ true,
    /*i*/ -7, /*u*/ 42u, /*d*/ 3.5,
    /*s*/ "text value",
    /*buf*/ {'b', 'u', 'f', '\0'},
    /*opt_set*/ 9, /*opt_null*/ std::nullopt,
    /*pr*/ {"key", 1},
    /*tup*/ {2, "mid", false},
    /*arr*/ {3, 4, 5},
    /*vec*/ {6, 7, 8},
    /*mp*/ {{"a", 1}, {"b", 2}},
    /*st*/ {9, 10, 11},
    /*nested*/ {12, "deep"},
  };
}

inline void check_mega(const mega& r) {
  CHECK(r.flag == true);
  CHECK(r.i == -7);
  CHECK(r.u == 42u);
  CHECK(r.d == doctest::Approx(3.5));
  CHECK(r.s == "text value");
  CHECK(std::string(r.buf.data()) == "buf");
  REQUIRE(r.opt_set.has_value());
  CHECK(*r.opt_set == 9);
  CHECK_FALSE(r.opt_null.has_value());
  CHECK(r.pr == std::pair<std::string, int>{"key", 1});
  CHECK(r.tup == std::tuple<int, std::string, bool>{2, "mid", false});
  CHECK(r.arr == std::array<int, 3>{3, 4, 5});
  CHECK(r.vec == std::vector<int>{6, 7, 8});
  CHECK(r.mp == std::map<std::string, int>{{"a", 1}, {"b", 2}});
  CHECK(r.st == std::set<int>{9, 10, 11});
  CHECK(r.nested.a == 12);
  CHECK(r.nested.b == "deep");
}
}

TEST_CASE("serialize: pretty (default)") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text(d) ==
        "name = hi\n"
        "v = {\n"
        "  x = 1\n"
        "  y = 2\n"
        "  z = 3\n"
        "}\n"
        "nums = [4, 5, 6]");
}

TEST_CASE("serialize: compact — root via \\n, nested ',' without space") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .prettify = false }>(d) ==
        "name=hi\n"
        "v={x=1,y=2,z=3}\n"
        "nums=[4,5,6]");
}

TEST_CASE("serialize: names_depth=0 — no names anywhere (positional)") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .names_depth = 0 }>(d) ==
        "hi\n"
        "(1, 2, 3)\n"
        "[4, 5, 6]");
}

TEST_CASE("serialize: full_escape — all strings quoted (ASCII)") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .prettify = false, .full_escape = true }>(d) ==
        "name=\"hi\"\n"
        "v={x=1,y=2,z=3}\n"
        "nums=[4,5,6]");
}

TEST_CASE("serialize: wrap_at — wraps a sequence at N per line") {
  const std::vector<int> nums{ 1, 2, 3, 4, 5 };
  CHECK(tavl_test::to_text<tavl::sopts{ .wrap_at = 2 }>(nums) ==
        "[\n"
        "  1, 2\n"
        "  3, 4\n"
        "  5\n"
        "]");
}

TEST_CASE("serialize: strings — quotes only when needed") {
  CHECK(tavl_test::to_text(std::string("plain_id")) == "plain_id");
  CHECK(tavl_test::to_text(std::string("has space")) == "\"has space\"");
  CHECK(tavl_test::to_text(std::string("true")) == "\"true\"");      // зарезервированное -> кавычки
}

TEST_CASE("serialize: optional/null") {
  CHECK(tavl_test::to_text(std::optional<int>{}) == "null");
  CHECK(tavl_test::to_text(std::optional<int>{42}) == "42");
}

TEST_CASE("serialize: bounded — hitting capacity() yields false without overflowing") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  std::string out;
  out.reserve(20);                       // заведомо меньше полного вывода (~36)
  const size_t cap = out.capacity();
  const bool ok = tavl::serialize<tavl::sopts{ .prettify = false, .bounded = true }>(d, out);
  CHECK_FALSE(ok);                       // не влезло -> false
  CHECK(out.size() <= cap);              // не вышли за reserve
  CHECK(out.capacity() == cap);          // capacity не вырос (без реаллокации)
}

TEST_CASE("serialize: bounded with sufficient capacity — true and full output") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  const std::string ref = tavl_test::to_text<tavl::sopts{ .prettify = false }>(d);
  std::string out;
  out.reserve(ref.size() + 16);
  const bool ok = tavl::serialize<tavl::sopts{ .prettify = false, .bounded = true }>(d, out);
  CHECK(ok);
  CHECK(out == ref);
}

TEST_CASE("serialize: large struct with all supported types (round-trip)") {
  tavl::parser p;
  p.add_default_operator();

  check_mega(tavl_test::round_trip(p, make_mega()));
}

// ===== serialize / deserialize mirror each other with the asymmetric-type flags OFF =====
// serialize_cstr=false disables std::string_view / const char* / char*, follow_pointers=false disables
// raw pointers. With both off, serialize emits ONLY what deserialize can read back, so a round-trip is
// lossless. Those asymmetric types are rejected at compile time (static_asserts in serialize.h), so they
// cannot silently break the mirror. Both flags default to off, so the default config IS the mirror.

TEST_CASE("mirror (flags off): is the default for serialize_cstr/follow_pointers") {
  constexpr tavl::sopts def{};
  static_assert(!def.serialize_cstr, "serialize_cstr must default to false (mirror by default)");
  static_assert(!def.follow_pointers, "follow_pointers must default to false (mirror by default)");

  // default serialization == explicit-flags-off serialization
  constexpr tavl::sopts off{ .serialize_cstr = false, .follow_pointers = false };
  CHECK(tavl_test::to_text(make_mega()) == tavl_test::to_text<off>(make_mega()));
}

TEST_CASE("mirror (flags off): large struct round-trips losslessly") {
  tavl::parser p;
  p.add_default_operator();

  constexpr tavl::sopts off{ .serialize_cstr = false, .follow_pointers = false };
  check_mega(tavl_test::round_trip<off>(p, make_mega()));
}

TEST_CASE("mirror (flags off): chrono / optional / unique_ptr / nested containers") {
  using namespace std::chrono;
  tavl::parser p;
  p.add_default_operator();

  constexpr tavl::sopts off{ .serialize_cstr = false, .follow_pointers = false };

  struct rec {
    system_clock::time_point when;          // time_point -> ISO datetime
    milliseconds dur;                       // duration   -> ISO datetime
    std::optional<std::string> some;
    std::optional<std::string> none;
    std::unique_ptr<int> ptr;
    std::vector<std::vector<int>> grid;     // nested sequences
    std::map<std::string, int> tags;
  };

  rec src{};
  src.when = system_clock::time_point{milliseconds{1718368205500}};
  src.dur  = milliseconds{1500};
  src.some = "hello world";                 // has a space -> must be quoted and read back
  src.none = std::nullopt;
  src.ptr  = std::make_unique<int>(42);
  src.grid = {{1, 2}, {3, 4, 5}};
  src.tags = {{"a", 1}, {"b", 2}};

  const auto r = tavl_test::round_trip<off>(p, src);

  CHECK(r.when == src.when);
  CHECK(r.dur == src.dur);
  REQUIRE(r.some.has_value());
  CHECK(*r.some == "hello world");
  CHECK_FALSE(r.none.has_value());
  REQUIRE(r.ptr != nullptr);
  CHECK(*r.ptr == 42);
  CHECK(r.grid == src.grid);
  CHECK(r.tags == src.tags);
}
