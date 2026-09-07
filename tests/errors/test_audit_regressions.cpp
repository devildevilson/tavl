#include <doctest/doctest.h>

#include <limits>
#include <set>

#include "util.h"

namespace {
using tavl::event_type;
using tavl::token_type;
using tavl::error_type;

bool has_critical(const tavl::ct_context& ctx) {
  for (const auto& d : ctx.diagnostics) if (d.error.is_critical()) return true;
  return false;
}

struct strings { std::string path; std::string next; int n; };
struct integers { int64_t low; uint64_t high; };
struct sequence { std::vector<int> nums; int tail; };

// Capture both token contents and positions before releasing any storage.
using snapshot = std::tuple<event_type, token_type, std::string, size_t, size_t, size_t, error_type>;
std::vector<snapshot> read_events(std::string_view text, size_t chunk, bool release) {
  tavl::parser p;
  p.add_default_operator();
  p.add_operator(":", tavl::op_fixity::binary, 2);
  std::vector<snapshot> out;
  size_t pos = 0;
  for (;;) {
    const auto [ev, err] = p.poll_event();
    if (ev.type == event_type::not_enought_data) {
      if (release) p.release_before(p.consumed_offset());
      if (pos < text.size()) {
        p.flush(text.substr(pos, chunk));
        pos += std::min(chunk, text.size() - pos);
      } else p.finish();
      continue;
    }
    const auto& s = ev.token.span;
    out.emplace_back(ev.type, ev.token.type, p.content(s), s.offset, s.line, s.column, err.type);
    if (release) p.release_before(p.consumed_offset());
    if (ev.type == event_type::eof) return out;
  }
}
}

TEST_CASE("regression: serialized trailing backslashes preserve the next field") {
  tavl::parser p; p.add_default_operator();
  for (size_t slashes : {size_t{1}, size_t{2}, size_t{3}}) {
    const strings src{"C:" + std::string(slashes, '\\'), "quoted\"value", 42};
    for (bool full : {false, true}) {
      const auto text = full ? tavl_test::to_text<tavl::sopts{.full_escape = true}>(src)
                             : tavl_test::to_text(src);
      for (size_t chunk : {size_t{1}, text.size()}) {
        const auto dst = tavl_test::deserialize_streamed<strings>(p, text, chunk);
        CHECK(dst.path == src.path);
        CHECK(dst.next == src.next);
        CHECK(dst.n == src.n);
      }
    }
  }
  const auto dst = tavl_test::deserialize_streamed<strings>(p, R"(path = 'C:\\', next = '', n = 42)");
  CHECK(dst.path == "C:\\");
  CHECK(dst.next.empty());
  CHECK(dst.n == 42);
}

TEST_CASE("regression: operator registrations own strings across copies and moves") {
  tavl::parser p;
  std::string symbolic(40, ':');
  std::string literal = "long_literal_operator_name";
  p.add_operator(symbolic, tavl::op_fixity::binary, 2);
  p.add_litteral_operator(literal, tavl::op_fixity::binary, 3);
  symbolic.assign(40, '!');
  literal.assign(30, 'x');
  p.add_operator(std::string("="), tavl::op_fixity::binary, 1);
  auto copy = p;
  p.clear_operators();
  auto moved = std::move(copy);
  const auto evs = tavl_test::poll_all(moved, "[a" + std::string(40, ':') + "b, a long_literal_operator_name b, a=b]");
  const auto types = tavl_test::got_token_types(evs);
  REQUIRE(types.size() == 9);
  CHECK(types[1] == token_type::op);
  CHECK(types[4] == token_type::op);
  CHECK(types[7] == token_type::op);
  REQUIRE(moved.operator_info(std::string(40, ':'), tavl::op_fixity::binary));
  CHECK(moved.operator_info(std::string(40, ':'), tavl::op_fixity::binary)->precedence == 2);
}

TEST_CASE("regression: releasing input retains delayed row tokens") {
  const std::string text = "abc = 123\nobject = { inner = value }\ncreated:2026-06-14T12:30:00\n";
  const auto reference = read_events(text, text.size(), false);
  for (size_t chunk : {size_t{1}, size_t{4}, text.size()})
    CHECK(read_events(text, chunk, true) == reference);

  tavl::char_storage storage;
  storage.append("abc def");
  storage.release_before(4);
  CHECK(storage.resolve({0, 3}).empty());
  CHECK(storage.resolve({3, 3}).empty());
  CHECK(storage.resolve({4, SIZE_MAX}) == "def");
}

TEST_CASE("regression: releasing input during deserialization preserves fields") {
  tavl::parser p; p.add_default_operator();
  tavl::ct_context ctx;
  sequence dst{};
  const std::string text = "nums = [1, 2, 3]\ntail = 42";
  for (size_t i = 0; i < text.size(); ++i) {
    p.flush(std::string_view(text).substr(i, 1));
    if (i + 1 == text.size()) p.finish();
    tavl::deserialize(p, ctx, dst);
    tavl::release_consumed(p, ctx);
  }
  CHECK_FALSE(ctx.stalled);
  CHECK_FALSE(has_critical(ctx));
  CHECK(dst.nums == std::vector<int>{1, 2, 3});
  CHECK(dst.tail == 42);
}

TEST_CASE("regression: integer limits round-trip and repeated signs are rejected") {
  tavl::parser p; p.add_default_operator();
  const integers src{INT64_MIN, UINT64_MAX};
  const auto text = tavl_test::to_text(src);
  tavl::ct_context ctx;
  const auto dst = tavl_test::deserialize_all<integers>(p, text, ctx);
  CHECK_FALSE(has_critical(ctx));
  CHECK(dst.low == src.low);
  CHECK(dst.high == src.high);
  const auto streamed = tavl_test::deserialize_streamed<integers>(p, text);
  CHECK(streamed.low == src.low);
  CHECK(streamed.high == src.high);

  for (std::string_view text : {"", "+", "-", "--1", "+-1", "--9223372036854775808"}) {
    int64_t v = 17;
    CHECK_FALSE(tavl::detail::is_dec_number(text, v));
    CHECK(v == 17);
  }
  const auto evs = tavl_test::poll_all(p, "[--1, +-1, --9223372036854775808]");
  for (const auto t : tavl_test::got_token_types(evs)) CHECK(t == token_type::unrecognized);
}

TEST_CASE("regression: datetime remains whole after an unspaced operator") {
  for (bool math : {false, true}) {
    tavl::parser p;
    if (math) p.add_math_default_operators(); else p.add_default_operator();
    p.add_operator(":", tavl::op_fixity::binary, 2);
    for (const auto text : {"[created:2026-06-14T12:30:00]", "[created=2026-06-14T12:30:00.12+05:00]"}) {
      const auto types = tavl_test::got_token_types(tavl_test::poll_all(p, text));
      REQUIRE(types.size() == 3);
      CHECK(types[2] == token_type::datetime);
    }
  }
  tavl::parser p;
  const auto types = tavl_test::got_token_types(tavl_test::poll_all(p, "[position:v3]"));
  REQUIRE(types.size() == 1);
  CHECK(types[0] == token_type::unrecognized);
}

TEST_CASE("regression: datetime checks decimal syntax but preserves calendar fields") {
  tavl::iso_datetime dt;
  REQUIRE(tavl::detail::is_datetime("2026-99-99T99:99:99+99:99", dt));
  CHECK(dt.y == 2026);
  CHECK(dt.m == 99);
  CHECK(dt.d == 99);
  CHECK(dt.hh == 99);
  CHECK(dt.mm == 99);
  CHECK(dt.ss == 99);
  CHECK(dt.tz_offset_mm == 99 * 60 + 99);
  for (const auto s : {"2026-0x-01", "202x-01-01", "2026-01-0x", "12:3x:00", "12:30:00+0x:00", "12:30:00+01:", "12:30:00+01:0x"}) {
    tavl::iso_datetime invalid;
    CHECK_FALSE(tavl::detail::is_datetime(s, invalid));
  }
}

TEST_CASE("regression: fractional seconds are scaled and truncated to milliseconds") {
  for (const auto& [fraction, ms] : std::vector<std::pair<std::string, int>>{
      {"1", 100}, {"12", 120}, {"123", 123}, {"123456", 123}, {"0009", 0}, {std::string(2000, '9'), 999}}) {
    const auto text = "1970-01-01T00:00:00." + fraction;
    tavl::iso_datetime dt;
    REQUIRE(tavl::detail::is_datetime(text, dt));
    CHECK(dt.ms == ms);
    CHECK(tavl::detail::iso_datetime_to_unix_ms(dt).count() == ms);
  }
}

TEST_CASE("regression: adjacent comments preserve tokens and source spans") {
  for (const std::string text : {"[1/*x*/,2]", "[a//x\nb]", "[a=b/* outer /* inner */ end */,c]", "[/*a*//*b*/1]"}) {
    tavl::parser p; p.add_default_operator();
    const auto evs = tavl_test::poll_all(p, text);
    for (const auto& e : evs) {
      CHECK(e.event.token.type != token_type::unrecognized);
      if (e.event.type == event_type::got_comment) {
        const auto content = p.content(e.event.token.span);
        CHECK((content.starts_with("/*") || content.starts_with("//")));
      }
    }
    CHECK(read_events(text, 1, true) == read_events(text, text.size(), false));
  }
}

TEST_CASE("regression: scalar block mismatches recover in whole and streamed input") {
  const std::string text = "nums = [[1], 2, {a = [3]}, 4]\ntail = 42";
  for (size_t chunk : {size_t{1}, text.size()}) {
    tavl::parser p; p.add_default_operator();
    tavl::ct_context ctx;
    sequence dst{};
    for (size_t i = 0; i < text.size(); i += chunk) {
      p.flush(std::string_view(text).substr(i, chunk));
      if (i + chunk >= text.size()) p.finish();
      tavl::deserialize(p, ctx, dst);
      tavl::release_consumed(p, ctx);
    }
    CHECK_FALSE(ctx.stalled);
    CHECK(ctx.frames.empty());
    CHECK(has_critical(ctx));
    CHECK(dst.nums == std::vector<int>{0, 2, 0, 4});
    CHECK(dst.tail == 42);
  }
}

TEST_CASE("regression: scalar recovery works in arrays sets tuples and wrappers") {
  struct fields {
    std::array<int, 2> array;
    std::set<int> set;
    std::pair<int, int> pair;
    std::optional<int> opt;
    std::unique_ptr<int> ptr;
    bool flag;
    std::string str;
    double fp;
    int tail;
  };
  const std::string text = "array=[[1],2]\nset=[[1],2]\npair=([1],2)\nopt=[1]\nptr=[1]\nflag=[true]\nstr=[text]\nfp=[1.5]\ntail=42";
  tavl::parser p; p.add_default_operator();
  const auto dst = tavl_test::deserialize_streamed<fields>(p, text);
  CHECK(dst.array == std::array<int, 2>{0, 2});
  CHECK(dst.set == std::set<int>{0, 2});
  CHECK(dst.pair == std::pair<int, int>{0, 2});
  REQUIRE(dst.opt);
  CHECK(*dst.opt == 0);
  REQUIRE(dst.ptr);
  CHECK(*dst.ptr == 0);
  CHECK_FALSE(dst.flag);
  CHECK(dst.str.empty());
  CHECK(dst.fp == 0);
  CHECK(dst.tail == 42);
}

TEST_CASE("regression: AST builders preserve parser errors across resume") {
  for (const auto builder : {tavl::make_pair_ast, tavl::make_tag_ast, tavl::make_math_ast}) {
    for (const std::string text : {"x = [1}", "x = [1", "x = [1}\ny = 2"}) {
      tavl::parser p; p.add_default_operator();
      const auto whole = tavl_test::build_ast_result(p, text, builder);
      CHECK(whole.error.type == error_type::err_bracket_missmatch);
      CHECK(whole.nodes.empty());
      const auto streamed = tavl_test::build_ast_streamed(p, text, builder);
      CHECK(streamed.error.type == error_type::err_bracket_missmatch);
      CHECK(streamed.nodes.empty());
    }
  }
}
