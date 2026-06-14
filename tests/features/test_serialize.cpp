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
}

TEST_CASE("serialize: pretty (по умолчанию)") {
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

TEST_CASE("serialize: compact — root через \\n, вложенное ',' без пробела") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .prettify = false }>(d) ==
        "name=hi\n"
        "v={x=1,y=2,z=3}\n"
        "nums=[4,5,6]");
}

TEST_CASE("serialize: names_depth=0 — имён нет нигде (позиционно)") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .names_depth = 0 }>(d) ==
        "hi\n"
        "(1, 2, 3)\n"
        "[4, 5, 6]");
}

TEST_CASE("serialize: full_escape — все строки в кавычках (ASCII)") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  CHECK(tavl_test::to_text<tavl::sopts{ .prettify = false, .full_escape = true }>(d) ==
        "name=\"hi\"\n"
        "v={x=1,y=2,z=3}\n"
        "nums=[4,5,6]");
}

TEST_CASE("serialize: wrap_at — перенос последовательности по N на строку") {
  const std::vector<int> nums{ 1, 2, 3, 4, 5 };
  CHECK(tavl_test::to_text<tavl::sopts{ .wrap_at = 2 }>(nums) ==
        "[\n"
        "  1, 2\n"
        "  3, 4\n"
        "  5\n"
        "]");
}

TEST_CASE("serialize: строки — кавычки только когда нужно") {
  CHECK(tavl_test::to_text(std::string("plain_id")) == "plain_id");
  CHECK(tavl_test::to_text(std::string("has space")) == "\"has space\"");
  CHECK(tavl_test::to_text(std::string("true")) == "\"true\"");      // зарезервированное -> кавычки
}

TEST_CASE("serialize: optional/null") {
  CHECK(tavl_test::to_text(std::optional<int>{}) == "null");
  CHECK(tavl_test::to_text(std::optional<int>{42}) == "42");
}

TEST_CASE("serialize: bounded — упор в capacity() даёт false без переполнения") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  std::string out;
  out.reserve(20);                       // заведомо меньше полного вывода (~36)
  const size_t cap = out.capacity();
  const bool ok = tavl::serialize<tavl::sopts{ .prettify = false, .bounded = true }>(d, out);
  CHECK_FALSE(ok);                       // не влезло -> false
  CHECK(out.size() <= cap);              // не вышли за reserve
  CHECK(out.capacity() == cap);          // capacity не вырос (без реаллокации)
}

TEST_CASE("serialize: bounded с достаточной capacity — true и полный вывод") {
  const doc d{ "hi", {1, 2, 3}, {4, 5, 6} };
  const std::string ref = tavl_test::to_text<tavl::sopts{ .prettify = false }>(d);
  std::string out;
  out.reserve(ref.size() + 16);
  const bool ok = tavl::serialize<tavl::sopts{ .prettify = false, .bounded = true }>(d, out);
  CHECK(ok);
  CHECK(out == ref);
}

TEST_CASE("serialize: крупная структура со всеми поддерживаемыми типами (round-trip)") {
  tavl::parser p;
  p.add_default_operator();

  const mega m{
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

  const auto r = tavl_test::round_trip(p, m);

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
