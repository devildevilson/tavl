#include <doctest/doctest.h>

#include <bit>
#include <charconv>
#include <cstdint>
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

TEST_CASE("regression: leading assignment diagnostics survive byte-by-byte input") {
  for (const std::string_view source : {"{=}", "{= =}", "(=)", "(= =)",
                                        "(= [1])", "[=]", "[= =]", "= ="}) {
    const auto reference = read_events(source, source.size(), false);
    CHECK(read_events(source, 1, true) == reference);
  }
}

TEST_CASE("regression: UTF-8 and control validation survive byte-by-byte input") {
  for (const std::string source : {
      std::string("x=\"") + char(0xF0) + char(0x9F) + char(0x98) + char(0x80) + "\"",
      std::string("x=\"") + char(0xC3) + "\"",
      std::string("// ") + char(0xFF) + "\n",
      std::string("x=\"") + "\xc2\x85" + "\"",
      std::string("/*") + char(0x1b) + "*/"}) {
    CHECK(read_events(source, 1, true) == read_events(source, source.size(), false));
  }
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

TEST_CASE("regression: failed finite-float conversion leaves its output unchanged") {
  double value = 17;
  CHECK_FALSE(tavl::detail::is_float_number("1e9999", value));
  CHECK(value == 17);
  CHECK_FALSE(tavl::detail::is_float_number("1e+", value));
  CHECK(value == 17);
}

TEST_CASE("float: correctly rounded binary64 midpoint and range boundaries") {
  using tavl::detail::float_parse_result;
  const auto parse_bits = [](std::string_view text) {
    double value = 0;
    REQUIRE(tavl::detail::parse_float_number(text, value) == float_parse_result::valid);
    return std::bit_cast<uint64_t>(value);
  };
  CHECK(parse_bits("1.00000000000000011102230246251565404236316680908203124") ==
        0x3ff0000000000000ULL);
  CHECK(parse_bits("1.00000000000000011102230246251565404236316680908203125") ==
        0x3ff0000000000000ULL);
  CHECK(parse_bits("1.00000000000000011102230246251565404236316680908203126") ==
        0x3ff0000000000001ULL);
  std::string long_above_midpoint =
    "1.00000000000000011102230246251565404236316680908203125";
  long_above_midpoint.append(1200, '0');
  long_above_midpoint.push_back('1');
  CHECK(parse_bits(long_above_midpoint) == 0x3ff0000000000001ULL);
  CHECK(parse_bits("5e-324") == 1);
  CHECK(parse_bits("3e-324") == 1);
  CHECK(parse_bits("1.7976931348623157e308") == 0x7fefffffffffffffULL);
  CHECK(parse_bits("1.7976931348623158e308") == 0x7fefffffffffffffULL);
  CHECK(parse_bits("0e999999999999999999999") == 0);
  CHECK(parse_bits("-0.0") == (uint64_t{1} << 63));
  CHECK(parse_bits("-0F") == (uint64_t{1} << 63));
  CHECK(parse_bits("1f") == 0x3ff0000000000000ULL);

  double unchanged = 17;
  for (std::string_view text : {"1e4000", "1e-4000", "2e-324",
                                "1.7976931348623159e308"}) {
    CHECK(tavl::detail::parse_float_number(text, unchanged) ==
          float_parse_result::out_of_range);
    CHECK(unchanged == 17);
  }
  CHECK(tavl::detail::parse_float_number("1e+", unchanged) ==
        float_parse_result::invalid);
  CHECK(unchanged == 17);
}

TEST_CASE("float: out-of-range spellings are critical, not unknown tokens") {
  tavl::parser p;
  const auto events = tavl_test::poll_all(p, "[1e4000, 1e-4000]");
  CHECK(tavl_test::got_token_types(events) ==
        std::vector<token_type>{token_type::number_float, token_type::number_float});
  size_t range_errors = 0;
  for (const auto& e : events)
    if (e.error.type == error_type::err_float_out_of_range) ++range_errors;
  CHECK(range_errors == 2);
  CHECK(tavl_test::has_critical(events));

  const auto streamed = read_events("[1e4000,1e-4000]", 1, true);
  size_t streamed_errors = 0;
  for (const auto& item : streamed)
    if (std::get<6>(item) == error_type::err_float_out_of_range) ++streamed_errors;
  CHECK(streamed_errors == 2);
}

TEST_CASE("float: binary64 values round-trip through decimal text") {
  uint64_t state = 0x321b620ec952fbe3ULL;
  for (int i = 0; i < 128; ++i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const uint64_t bits = state & 0x7fefffffffffffffULL;
    const double original = std::bit_cast<double>(bits);
    char buffer[64];
    const auto printed = std::to_chars(buffer, buffer + sizeof(buffer), original);
    REQUIRE(printed.ec == std::errc{});
    std::string decimal(buffer, printed.ptr);
    if (decimal.find_first_of(".eE") == std::string::npos) decimal += ".0";
    double parsed = 0;
    REQUIRE(tavl::detail::parse_float_number(decimal, parsed) ==
            tavl::detail::float_parse_result::valid);
    CHECK(std::bit_cast<uint64_t>(parsed) == bits);
  }
}

TEST_CASE("float: integer tokens use the same ties-to-even binary64 rule") {
  tavl::parser p;
  const auto events = tavl_test::poll_all(
    p, "[9007199254740993,9007199254740995,18446744073709551615]");
  std::vector<uint64_t> bits;
  for (const auto& e : events) {
    if (e.event.type != event_type::got_token) continue;
    const auto value = p.to_float(e.event.token);
    REQUIRE(value.has_value());
    bits.push_back(std::bit_cast<uint64_t>(*value));
  }
  CHECK(bits == std::vector<uint64_t>{0x4340000000000000ULL,
                                      0x4340000000000002ULL,
                                      0x43f0000000000000ULL});
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

TEST_CASE("regression: fractional seconds fit iso_datetime nanoseconds exactly") {
  for (const auto& [fraction, ns] : std::vector<std::pair<std::string, int>>{
      {"1", 100000000}, {"12", 120000000}, {"123", 123000000},
      {"123456", 123456000}, {"123456789", 123456789}, {"000", 0}}) {
    const auto text = "1970-01-01T00:00:00." + fraction + "Z";
    tavl::iso_datetime dt;
    REQUIRE(tavl::detail::is_datetime(text, dt));
    CHECK(dt.ns == ns);
    CHECK(dt.is_utc);
    const auto converted = tavl::detail::iso_datetime_to_unix_ns(dt);
    REQUIRE(converted.has_value());
    CHECK(converted->count() == ns);
  }
  for (const auto fraction : {"1234567890", "0000000001"}) {
    const auto text = std::string("1970-01-01T00:00:00.") + fraction;
    tavl::iso_datetime dt;
    CHECK_FALSE(tavl::detail::is_datetime(text, dt));
    tavl::parser p; p.add_default_operator();
    const auto evs = tavl_test::poll_all(p, "t = " + text);
    CHECK(tavl_test::got_token_types(evs) == std::vector<token_type>{token_type::unrecognized});
  }
}

TEST_CASE("regression: unterminated strings and block comments report critical errors") {
  for (const auto [src, expected] : std::vector<std::pair<std::string_view, error_type>>{
      {"name = \"unfinished", error_type::err_unterminated_string},
      {"name = 'unfinished", error_type::err_unterminated_string},
      {"name = 1 /* unfinished", error_type::err_unterminated_block_comment}}) {
    tavl::parser p; p.add_default_operator();
    const auto evs = tavl_test::poll_all(p, src);
    CHECK(tavl_test::has_error(evs, expected));
    CHECK(tavl_test::has_critical(evs));
  }
  tavl::parser p; p.add_default_operator();
  const auto good = tavl_test::poll_all(p, "name = \"done\" /* done */ // line");
  CHECK_FALSE(tavl_test::has_critical(good));
}

TEST_CASE("regression: EOF diagnostics survive byte-by-byte input") {
  for (const auto [src, expected] : std::vector<std::pair<std::string_view, error_type>>{
      {"x = \"open", error_type::err_unterminated_string},
      {"x = 'open", error_type::err_unterminated_string},
      {"x = /* outer /* inner */", error_type::err_unterminated_block_comment}}) {
    tavl::parser p; p.add_default_operator();
    size_t offset = 0;
    int count = 0;
    bool finished = false;
    while (true) {
      const auto [ev, err] = p.poll_event();
      if (err.type == expected) ++count;
      if (ev.type == event_type::eof) break;
      if (ev.type == event_type::not_enought_data) {
        if (offset < src.size()) p.flush(src.substr(offset++, 1));
        else if (!finished) { p.finish(); finished = true; }
      }
    }
    CHECK(count == 1);
  }
}

TEST_CASE("regression: invalid escapes survive byte-by-byte input") {
  constexpr std::string_view src = R"(x="\uD83D\u0041")";
  tavl::parser p;
  p.add_default_operator();
  size_t offset = 0;
  int count = 0;
  bool finished = false;
  while (true) {
    const auto [ev, err] = p.poll_event();
    if (err.type == error_type::err_invalid_escape) ++count;
    if (ev.type == event_type::eof) break;
    if (ev.type == event_type::not_enought_data) {
      if (offset < src.size()) p.flush(src.substr(offset++, 1));
      else if (!finished) { p.finish(); finished = true; }
    }
  }
  CHECK(count == 1);
}

TEST_CASE("regression: deserializer receives unterminated-string diagnostic") {
  struct value { std::string text; };
  tavl::parser p; p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<value>(p, "text = \"open", ctx);
  bool found = false;
  for (const auto& d : ctx.diagnostics)
    found |= d.error.type == error_type::err_unterminated_string;
  CHECK(found);
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

TEST_CASE("regression: integral fields reject values outside their destination range") {
  struct narrow { int8_t signed_byte = 7; uint8_t unsigned_byte = 8; int tail = 0; };
  for (const std::string_view source : {
      "signed_byte=128\nunsigned_byte=256\ntail=42",
      "signed_byte=-129\nunsigned_byte=-1\ntail=42",
      "signed_byte=0x8000000000000000\nunsigned_byte=256\ntail=42"}) {
    tavl::parser p; p.add_default_operator();
    tavl::ct_context ctx;
    const auto value = tavl_test::deserialize_all<narrow>(p, source, ctx);
    CHECK(value.signed_byte == 7);
    CHECK(value.unsigned_byte == 8);
    CHECK(value.tail == 42);
    size_t range_errors = 0;
    for (const auto& d : ctx.diagnostics)
      if (d.error.type == error_type::err_number_out_of_range) ++range_errors;
    CHECK(range_errors == 2);
  }
  tavl::parser p; p.add_default_operator();
  tavl::ct_context ctx;
  const auto edge = tavl_test::deserialize_all<narrow>(p, "signed_byte=-128\nunsigned_byte=255\ntail=42", ctx);
  CHECK(edge.signed_byte == -128);
  CHECK(edge.unsigned_byte == 255);
  CHECK(edge.tail == 42);
  CHECK_FALSE(has_critical(ctx));
}

TEST_CASE("regression: each explicit array row gives one set element") {
  struct fields {
    std::set<int> values;
    std::multiset<int> repeated;
    std::set<std::vector<int>> nested;
    int tail;
  };
  const std::string source = "values=[1 2,3]\nrepeated=[1 2,1]\nnested=[1 2,3 4]\ntail=42";
  for (const size_t chunk : {size_t{1}, source.size()}) {
    tavl::parser p; p.add_default_operator();
    tavl::ct_context ctx;
    fields value{};
    for (size_t pos = 0; pos < source.size(); pos += chunk) {
      p.flush(std::string_view(source).substr(pos, chunk));
      if (pos + chunk >= source.size()) p.finish();
      tavl::deserialize(p, ctx, value);
      tavl::release_consumed(p, ctx);
    }
    CHECK_FALSE(ctx.stalled);
    CHECK(value.values == std::set<int>{1, 3});
    CHECK(value.repeated == std::multiset<int>{1, 1});
    CHECK(value.nested == std::set<std::vector<int>>{{1, 2}, {3, 4}});
    CHECK(value.tail == 42);
    size_t extra_values = 0;
    for (const auto& d : ctx.diagnostics)
      if (d.error.type == error_type::err_too_many_values) ++extra_values;
    CHECK(extra_values == 2);
  }
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
