// deserialize типов и контейнеров + round-trip с serialize.

#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

#include "util.h"

namespace {
struct rgb { int r, g, b; };
struct bag {
  std::vector<int> nums;
  std::array<int, 3> arr;
  std::pair<std::string, int> kv;
  std::tuple<int, std::string, bool> t;
};
struct assoc { std::map<std::string, int> m; std::set<int> s; };
struct opt  { std::optional<int> a; std::optional<int> b; std::unique_ptr<int> c; };
struct buf  { std::array<char, 8> name; };   // char[8] нельзя: reflect посчитал бы массив за 8 полей
struct data { int a; int b; std::array<int, 3> c; };
}

TEST_CASE("deserialize: named fields of primitives") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<rgb>(p, "r = 255\ng = 128\nb = 0");
  CHECK(v.r == 255);
  CHECK(v.g == 128);
  CHECK(v.b == 0);
}

TEST_CASE("deserialize: positional fill (values without names)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<rgb>(p, "255\n128\n0");
  CHECK(v.r == 255);
  CHECK(v.g == 128);
  CHECK(v.b == 0);
}

TEST_CASE("deserialize: containers (vector/array/pair/tuple)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<bag>(p,
      "nums = [1, 2, 3]\narr = [4, 5, 6]\nkv = (key, 7)\nt = (1, hi, true)");
  CHECK(v.nums == std::vector<int>{1, 2, 3});
  CHECK(v.arr == std::array<int, 3>{4, 5, 6});
  CHECK(v.kv.first == "key");
  CHECK(v.kv.second == 7);
  CHECK(std::get<0>(v.t) == 1);
  CHECK(std::get<1>(v.t) == "hi");
  CHECK(std::get<2>(v.t) == true);
}

TEST_CASE("deserialize: pair/tuple — inline, block and [] wrapper") {
  tavl::parser p;
  p.add_default_operator();

  struct forms {
    std::tuple<int, int, int> inline_tuple;          // 1 2 3
    std::tuple<int, int, int> block_tuple;           // (1, 2, 3)
    std::tuple<int, int, int> bracket_tuple;         // [1, 2, 3]
    std::pair<std::string, std::string> inline_pair; // a=b
    std::pair<std::string, std::string> block_pair;  // (a, b)
    std::pair<std::string, std::string> bracket_pair;// [x, y]
  };

  const auto v = tavl_test::deserialize_all<forms>(p,
      "inline_tuple = 1 2 3\n"
      "block_tuple = (1, 2, 3)\n"
      "bracket_tuple = [1, 2, 3]\n"
      "inline_pair = a=b\n"
      "block_pair = (a, b)\n"
      "bracket_pair = [x, y]");

  CHECK(v.inline_tuple  == std::tuple<int, int, int>{1, 2, 3});
  CHECK(v.block_tuple   == std::tuple<int, int, int>{1, 2, 3});
  CHECK(v.bracket_tuple == std::tuple<int, int, int>{1, 2, 3});
  CHECK(v.inline_pair   == std::pair<std::string, std::string>{"a", "b"});
  CHECK(v.block_pair    == std::pair<std::string, std::string>{"a", "b"});
  CHECK(v.bracket_pair  == std::pair<std::string, std::string>{"x", "y"});
}

TEST_CASE("deserialize: associative containers (map/set)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<assoc>(p, "m = {a = 1, b = 2}\ns = [3, 1, 2]");
  REQUIRE(v.m.size() == 2);
  CHECK(v.m.at("a") == 1);
  CHECK(v.m.at("b") == 2);
  CHECK(v.s == std::set<int>{1, 2, 3});
}

TEST_CASE("deserialize: optional/unique_ptr (null -> empty)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<opt>(p, "a = 5\nb = null\nc = 9");
  REQUIRE(v.a.has_value());
  CHECK(*v.a == 5);
  CHECK_FALSE(v.b.has_value());
  REQUIRE(v.c != nullptr);
  CHECK(*v.c == 9);
}

TEST_CASE("deserialize: char buffer with zero-filled tail") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<buf>(p, "name = hello");
  CHECK(std::string(v.name.data()) == "hello");
  CHECK(v.name[5] == '\0');
}

TEST_CASE("deserialize: integers cross-cast (hex/oct/bin)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = tavl_test::deserialize_all<rgb>(p, "r = 0xFF\ng = 0o20\nb = 0b1000");
  CHECK(v.r == 255);
  CHECK(v.g == 16);
  CHECK(v.b == 8);
}

TEST_CASE("deserialize: unknown fields are skipped with nested values") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  const auto v = tavl_test::deserialize_all<rgb>(p,
      "r = 1\n"
      "unknown = { a = 1, b = [2, 3] }\n"
      "g = 2\n"
      "b = 3",
      ctx);

  CHECK(v.r == 1);
  CHECK(v.g == 2);
  CHECK(v.b == 3);
  CHECK(ctx.diagnostics.empty());
}

TEST_CASE("deserialize: byte-by-byte streaming == all at once") {
  tavl::parser p;
  p.add_default_operator();
  const std::string_view src = "nums = [1, 2, 3]\narr = [4, 5, 6]\nkv = (key, 7)\nt = (1, hi, true)";

  const auto whole = tavl_test::deserialize_all<bag>(p, src);
  const auto streamed = tavl_test::deserialize_streamed<bag>(p, src, 1);

  CHECK(whole.nums == streamed.nums);
  CHECK(whole.arr == streamed.arr);
  CHECK(whole.kv == streamed.kv);
  CHECK(whole.t == streamed.t);
}

TEST_CASE("round-trip: serialize -> deserialize preserves values") {
  tavl::parser p;
  p.add_default_operator();

  // round-trip — для структур (целевой кейс); голые контейнеры на верхнем уровне оборачиваем в структуру
  SUBCASE("associative containers via a struct") {
    const assoc a{ {{"a", 1}, {"b", 2}, {"c", 3}}, {1, 2, 3} };
    const auto r = tavl_test::round_trip(p, a);
    CHECK(r.m == a.m);
    CHECK(r.s == a.s);
  }
  SUBCASE("struct compact") {
    const bag b{ {1, 2}, {3, 4, 5}, {"k", 6}, {7, "v", false} };
    const auto r = tavl_test::round_trip<tavl::sopts{ .prettify = false }>(p, b);
    CHECK(r.nums == b.nums);
    CHECK(r.arr == b.arr);
    CHECK(r.kv == b.kv);
    CHECK(r.t == b.t);
  }
}

// читает все экземпляры через deserialize_next (один и тот же цикл для single и list)
static std::vector<data> read_all(tavl::parser& p, std::string_view src) {
  p.clear();
  p.flush(src);
  p.finish();
  tavl::ct_context ctx;
  std::vector<data> out;
  for (data d{}; tavl::deserialize_next(p, ctx, d); d = data{}) out.push_back(d);
  return out;
}

TEST_CASE("deserialize_next: single file -> one instance") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = read_all(p, "a = 5\nb = 6\nc = 1 2 3");
  REQUIRE(v.size() == 1);
  CHECK(v[0].a == 5);
  CHECK(v[0].b == 6);
  CHECK(v[0].c == std::array<int, 3>{1, 2, 3});
}

TEST_CASE("deserialize_next: list file -> N instances (the same loop)") {
  tavl::parser p;
  p.add_default_operator();
  const auto v = read_all(p, "(a = 4, b = 2, c = (4,5,6)),(a = 0, b = 2, c = 7 8 9),");
  REQUIRE(v.size() == 2);
  CHECK(v[0].a == 4);
  CHECK(v[0].c == std::array<int, 3>{4, 5, 6});
  CHECK(v[1].a == 0);
  CHECK(v[1].c == std::array<int, 3>{7, 8, 9});
}

TEST_CASE("deserialize_next: various valid list forms") {
  tavl::parser p;
  p.add_default_operator();

  SUBCASE("case1: commas between fields, all on one line") {
    const auto v = read_all(p, "(a=1,b=2,c=1 2 3),(a=2,b=3,c=2 3 4)");
    REQUIRE(v.size() == 2);
    CHECK(v[0].a == 1); CHECK(v[0].b == 2); CHECK(v[0].c == std::array<int, 3>{1, 2, 3});
    CHECK(v[1].a == 2); CHECK(v[1].b == 3); CHECK(v[1].c == std::array<int, 3>{2, 3, 4});
  }

  SUBCASE("case2: multiline blocks, mixed ,/\\n, separated by a blank line") {
    const auto v = read_all(p,
        "(\n"
        "  a=1,\n"
        "  b=2\n"
        "  c=1 2 3\n"
        ")\n"
        "\n"
        "(\n"
        "  a=1\n"
        "  b=2,\n"
        "  c=4 5 6\n"
        ")");
    REQUIRE(v.size() == 2);
    CHECK(v[0].a == 1); CHECK(v[0].b == 2); CHECK(v[0].c == std::array<int, 3>{1, 2, 3});
    CHECK(v[1].a == 1); CHECK(v[1].b == 2); CHECK(v[1].c == std::array<int, 3>{4, 5, 6});
  }

  SUBCASE("case3: line breaks inside blocks, comma between blocks, trailing comma") {
    const auto v = read_all(p,
        "(\n"
        "a = 4, b = 2, c = (4,5,6)\n"
        "),(\n"
        "a = 0, b = 2, c = 7 8 9\n"
        "),");
    REQUIRE(v.size() == 2);
    CHECK(v[0].a == 4); CHECK(v[0].b == 2); CHECK(v[0].c == std::array<int, 3>{4, 5, 6});
    CHECK(v[1].a == 0); CHECK(v[1].b == 2); CHECK(v[1].c == std::array<int, 3>{7, 8, 9});
  }
}

TEST_CASE("streaming: release_consumed keeps the input buffer bounded") {
  // имитация сетевых пачек: по одному экземпляру за раз, между ними сбрасываем прочитанное
  const char* packets[] = {
    "(a=1,b=2,c=1 2 3),",
    "(a=2,b=3,c=2 3 4),",
    "(a=3,b=4,c=3 4 5),",
  };

  tavl::parser p;
  p.add_default_operator();
  p.clear();
  tavl::ct_context ctx;

  std::vector<data> out;
  size_t peak_buffer = 0;
  for (const char* pkt : packets) {
    p.flush(pkt);
    if (p.storage.buffer_size() > peak_buffer) peak_buffer = p.storage.buffer_size();
    data d{};
    while (tavl::deserialize_next(p, ctx, d)) {
      out.push_back(d);
      d = data{};
      tavl::release_consumed(p, ctx);          // сбросить разобранное
    }
  }

  REQUIRE(out.size() == 3);
  CHECK(out[0].a == 1);
  CHECK(out[1].b == 3);
  CHECK(out[2].c == std::array<int, 3>{3, 4, 5});
  // без release буфер дорос бы до всех ~54 байт; с release держится ~одной пачки
  CHECK(peak_buffer < 40);
}

TEST_CASE("single/list classification via parser::peek (doesn't consume the stream)") {
  // первое СОДЕРЖАТЕЛЬНОЕ событие: блок-begin -> список, иначе -> одиночный
  const auto first_content = [](tavl::parser& pp) {
    while (true) {
      const auto e = pp.peek();
      if (e.type == tavl::event_type::row_begin) { pp.poll_event(); continue; }
      return e.type;
    }
  };

  tavl::parser p;
  p.add_default_operator();

  p.clear(); p.flush("a = 5\nb = 6"); p.finish();
  CHECK(first_content(p) == tavl::event_type::got_row_identifier);   // одиночный

  p.clear(); p.flush("(a = 4),(a = 0)"); p.finish();
  CHECK(first_content(p) == tavl::event_type::tuple_begin);          // список
}
