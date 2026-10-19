// Error handling and misuse checks: parser errors plus deserialize diagnostics.

#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <optional>

#include "util.h"

using tavl::error_type;
using tavl::event_type;

namespace {
struct rgb { int r, g, b; };
struct buf { std::array<char, 4> s; };
struct typed_fields { bool b; int i; unsigned u; double f; };
struct nested_typed_fields { std::vector<int> nums; std::optional<unsigned> maybe; std::unique_ptr<double> ptr; };

bool has_diag(const tavl::ct_context& ctx, error_type t) {
  for (const auto& d : ctx.diagnostics) if (d.error.type == t) return true;
  return false;
}

int g_check_calls = 0;   // TAVL_CHECK call count, intercepted by a test handler
}

// ---------------- parser errors ----------------

TEST_CASE("error: unclosed block -> critical err_bracket_missmatch") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ a = 1");
  CHECK(tavl_test::has_error(evs, error_type::err_bracket_missmatch));
  CHECK(tavl_test::has_critical(evs));
}

TEST_CASE("error: extra closing bracket -> critical") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "a = 1 }");
  CHECK(tavl_test::has_error(evs, error_type::err_bracket_missmatch));
}

TEST_CASE("error: empty row before a comma -> warn_empty_row (not critical)") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[1, , 2]");
  CHECK(tavl_test::has_error(evs, error_type::warn_empty_row));
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("error: object without a value -> warn_object_without_value") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ key }");
  CHECK(tavl_test::has_error(evs, error_type::warn_object_without_value));
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("error: named assignment without data is critical outside arrays") {
  tavl::parser p;
  p.add_default_operator();
  for (std::string_view source : {"a=", "x=(a=)", "x={a=}"}) {
    const auto evs = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(evs, error_type::err_missing_value));
    CHECK(tavl_test::has_critical(evs));
  }
  const auto array = tavl_test::poll_all(p, "x=[a =]");
  CHECK_FALSE(tavl_test::has_error(array, error_type::err_missing_value));
  CHECK_FALSE(tavl_test::has_critical(array));
}

TEST_CASE("error: invalid double-quoted escapes are critical and keep their source") {
  tavl::parser p;
  p.add_default_operator();
  for (const std::string_view source : {
      R"(x="\q")", R"(x="\u12G4")", R"(x="\u123")",
      R"(x="\U00110000")", R"(x="\uD83D")", R"(x="\uDE00")",
      R"(x="\uD83D\u0041")"}) {
    const auto evs = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(evs, error_type::err_invalid_escape));
    CHECK(tavl_test::has_critical(evs));
  }
  for (const std::string_view source : {
      R"(x="\n\u0041\U0001F600")", R"(x="\uD83D\uDE00")",
      R"(x='literal\q')"}) {
    CHECK_FALSE(tavl_test::has_critical(tavl_test::poll_all(p, source)));
  }
  const auto bad = tavl_test::poll_all(p, "x=\"line\n\\q\"");
  bool located = false;
  tavl::token raw_token;
  for (const auto& e : bad) {
    if (e.error.type == error_type::err_invalid_escape)
      located = e.error.span.line == 2 && e.error.span.column == 1;
    if (e.event.type == event_type::got_token &&
        e.event.token.type == tavl::token_type::doublequote_string)
      raw_token = e.event.token;
  }
  CHECK(located);
  CHECK(p.to_string(raw_token) == "line\n\\q");
}

TEST_CASE("error: ill-formed UTF-8 is critical in values and comments") {
  tavl::parser p; p.add_default_operator();
  const std::vector<std::string> invalid = {
    std::string("x=\"") + char(0xFF) + "\"",
    std::string("x=") + char(0xC0) + char(0xAF),
    std::string("// ") + char(0xED) + char(0xA0) + char(0x80),
    std::string("x=\"") + char(0xF4) + char(0x90) + char(0x80) + char(0x80) + "\"",
    std::string("x=\"") + char(0xE2) + char(0x82) + "\""
  };
  for (const auto& source : invalid) {
    const auto events = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(events, error_type::err_invalid_utf8));
    CHECK(tavl_test::has_critical(events));
  }
  const auto valid = tavl_test::poll_all(p, "x=\"\xD0\xBF\xF0\x9F\x98\x80\"");
  CHECK_FALSE(tavl_test::has_critical(valid));
}

TEST_CASE("error: literal NUL is critical but an escaped NUL is a string value") {
  tavl::parser p; p.add_default_operator();
  const std::vector<std::string> invalid = {
    std::string(1, '\0'),
    std::string("{\0}", 3),
    std::string("x=\"a\0b\"", 7),
    std::string("x='a\0b'", 7),
    std::string("// a\0b", 6),
    std::string("x=a\0b", 5),
  };
  for (const auto& source : invalid) {
    const auto events = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(events, error_type::err_forbidden_nul));
    CHECK(tavl_test::has_critical(events));
  }

  const auto events = tavl_test::poll_all(p, R"(x="a\u0000b")");
  CHECK_FALSE(tavl_test::has_critical(events));
  for (const auto& e : events) {
    if (e.event.type == event_type::got_token &&
        e.event.token.type == tavl::token_type::doublequote_string)
      CHECK(p.to_string(e.event.token) == std::string("a\0b", 3));
  }
}

TEST_CASE("error: forbidden literal controls are critical but Unicode escapes remain values") {
  tavl::parser p; p.add_default_operator();
  const std::vector<std::string> invalid = {
    std::string("x=\"") + char(0x1b) + "\"",      // ESC in double quotes
    std::string("x='") + char(0x7f) + "'",          // DEL in single quotes
    std::string("// ") + char(0x07) + "\n",      // BEL in a line comment
    std::string("/*") + char(0x1f) + "*/",       // US in a block comment
    std::string("x=") + char(0x1b),              // ESC in a bare scalar
    std::string("x=\"") + "\xc2\x85" + "\"", // C1 U+0085
  };
  for (const auto& source : invalid) {
    const auto events = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(events, error_type::err_forbidden_control));
    CHECK(tavl_test::has_critical(events));
  }

  const auto c1 = tavl_test::poll_all(p, "x=\"\xc2\x85\"");
  bool located = false;
  for (const auto& e : c1) if (e.error.type == error_type::err_forbidden_control) {
    CHECK(e.error.span.offset == 3);
    CHECK(e.error.span.size == 2);
    located = true;
  }
  CHECK(located);

  for (const std::string_view source : {
      "x=\"a\t\n\r\f\vb\"", "/*a\t\n\r\f\vb*/", "x\f=1", "x\v=1",
      R"(x="\u001b\u007f\u0085")"}) {
    CHECK_FALSE(tavl_test::has_critical(tavl_test::poll_all(p, source)));
  }
  const auto escaped = tavl_test::poll_all(p, R"(x="\u001b\u007f\u0085")");
  for (const auto& e : escaped) if (e.event.type == event_type::got_token &&
                                     e.event.token.type == tavl::token_type::doublequote_string)
    CHECK(p.to_string(e.event.token) == std::string("\x1b\x7f\xc2\x85", 4));
}

TEST_CASE("error: a leading assignment requires an identifier when followed by data") {
  tavl::parser p;
  p.add_default_operator();
  for (const std::string_view source : {"{=}", "{= =}", "{ = 6 }",
                                        "(= =)", "(= value)", "(= [1])",
                                        "(= = =)", "= ="}) {
    const auto evs = tavl_test::poll_all(p, source);
    CHECK(tavl_test::has_error(evs, error_type::err_expected_identifier));
    CHECK(tavl_test::has_critical(evs));
  }
  for (const std::string_view source : {"(=)", "[=]", "[= =]", "[= = =]"}) {
    const auto evs = tavl_test::poll_all(p, source);
    CHECK_FALSE(tavl_test::has_critical(evs));
    const size_t expected_tokens = source == "[= = =]" ? 3 : source == "[= =]" ? 2 : 1;
    CHECK(tavl_test::got_token_types(evs).size() == expected_tokens);
  }
}

TEST_CASE("error: structural operator diagnostics do not depend on default registration") {
  tavl::parser p;
  for (const std::string_view spelling : {"=", ":"}) {
    p.clear_operators();
    p.add_operator(spelling, tavl::op_fixity::binary, 1);
    for (const std::string_view source : {"{?}", "(? value)", "(? ?)", "(a?)", "a?"}) {
      std::string input(source);
      input.replace(input.find('?'), 1, spelling);
      if (const auto second = input.find('?'); second != std::string::npos)
        input.replace(second, 1, spelling);
      const auto evs = tavl_test::poll_all(p, input);
      CHECK(tavl_test::has_critical(evs));
      CHECK(tavl_test::has_error(evs, source == "(a?)" || source == "a?"
            ? error_type::err_missing_value : error_type::err_expected_identifier));
    }
    for (const std::string_view source : {"(?)", "[?]", "[? ?]"}) {
      std::string input(source);
      for (char& c : input) if (c == '?') c = spelling.front();
      CHECK_FALSE(tavl_test::has_critical(tavl_test::poll_all(p, input)));
    }
  }
}

// ---------------- struct-fill errors ----------------

TEST_CASE("error: unfilled fields -> warn_missing_field") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "r = 1", ctx);    // g and b are absent
  CHECK(has_diag(ctx, error_type::warn_missing_field));
}

TEST_CASE("error: duplicate field -> err_duplicate_field") {
  tavl::parser p;
  p.add_default_operator();
  for (const std::string_view source : {
      "r = 1\nr = 2\ng = 3\nb = 4",
      "{r = 1, r = 2, g = 3, b = 4}",
      "(r = 1, r = 2, g = 3, b = 4)"}) {
    p.clear();
    p.flush(source);
    p.finish();
    tavl::ct_context ctx;
    rgb value{};
    REQUIRE(tavl::deserialize_next(p, ctx, value));
    CHECK(has_diag(ctx, error_type::err_duplicate_field));
  }
}

TEST_CASE("error: named field after positional fill is duplicate") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "1\nr = 2\ng = 3\nb = 4", ctx);
  CHECK(has_diag(ctx, error_type::err_duplicate_field));
}

TEST_CASE("error: extra positional values -> err_too_many_values") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "1\n2\n3\n4", ctx);   // 4 values for 3 fields
  CHECK(has_diag(ctx, error_type::err_too_many_values));
}

TEST_CASE("error: too-long string in a char buffer -> err_string_too_long") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<buf>(p, "s = toolong", ctx);  // 7 chars into char[4]
  CHECK(has_diag(ctx, error_type::err_string_too_long));
}

TEST_CASE("error: primitive type mismatches produce err_expect_token diagnostics") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<typed_fields>(p, "b = 1\ni = text\nu = -1\nf = text", ctx);

  CHECK(has_diag(ctx, error_type::err_expect_token_boolean));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_int));
  CHECK(has_diag(ctx, error_type::err_number_out_of_range));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_float));
}

TEST_CASE("error: nested primitive type mismatches produce err_expect_token diagnostics") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<nested_typed_fields>(p, "nums = [1, text]\nmaybe = -1\nptr = text", ctx);

  CHECK(has_diag(ctx, error_type::err_expect_token_number_int));
  CHECK(has_diag(ctx, error_type::err_number_out_of_range));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_float));
}

// ---------------- TAVL_CHECK: loud configuration failure ----------------

TEST_CASE("misuse: invalid operator -> TAVL_CHECK (caught by a handler instead of abort)") {
  g_check_calls = 0;
  const auto prev = tavl::set_check_handler(
      [](const char*, const char*, int, const char*) { ++g_check_calls; });

  tavl::parser p;
  p.add_operator("abc", tavl::op_fixity::binary, 1);          // not operator_chars
  CHECK(g_check_calls == 1);

  p.add_litteral_operator("123", tavl::op_fixity::binary, 1); // not a valid identifier
  CHECK(g_check_calls == 2);

  tavl::set_check_handler(prev);   // restore default handler
}

// ---------------- memory limits ----------------

TEST_CASE("limit: nesting depth -> err_nesting_too_deep (critical)") {
  tavl::parser p;
  const std::string src(tavl::limits::max_nesting + 5, '(');   // more open brackets than the limit
  const auto evs = tavl_test::poll_all(p, src);
  CHECK(tavl_test::has_error(evs, error_type::err_nesting_too_deep));
  CHECK(tavl_test::has_critical(evs));
}

TEST_CASE("limit: row length -> err_row_too_long") {
  std::string src = "[";
  for (size_t i = 0; i < tavl::limits::max_row_tokens + 100; ++i) src += "1,";
  src += "]";

  tavl::parser p;
  p.clear(); p.flush(src); p.finish();
  tavl::event ev{}; tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);

  std::vector<tavl::node> nodes;
  tavl::ast_context ctx;
  std::tie(ev, err) = tavl::make_pair_ast(p, ctx, nodes);
  CHECK(err.type == error_type::err_row_too_long);
}
