// Lexer: token recognition and streaming.

#include <doctest/doctest.h>

#include "util.h"

using tavl::token_type;
using tavl::event_type;

TEST_CASE("lexer: recognition of scalar types") {
  tavl::parser p;   // no registered operators
  const auto evs = tavl_test::poll_all(p,
    "[null, true, false, 42, 0xFF, 0o17, 0b1010, 3.14, 'abc', \"def\", 2026-06-14T12:30:00, ident]");

  REQUIRE_FALSE(tavl_test::has_critical(evs));
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 12);
  CHECK(t[0]  == token_type::null);
  CHECK(t[1]  == token_type::boolean);
  CHECK(t[2]  == token_type::boolean);
  CHECK(t[3]  == token_type::number_int);
  CHECK(t[4]  == token_type::number_uint);     // hex
  CHECK(t[5]  == token_type::number_uint);     // oct
  CHECK(t[6]  == token_type::number_uint);     // bin
  CHECK(t[7]  == token_type::number_float);
  CHECK(t[8]  == token_type::singlequote_string);
  CHECK(t[9]  == token_type::doublequote_string);
  CHECK(t[10] == token_type::datetime);
  CHECK(t[11] == token_type::identifier);
}

TEST_CASE("lexer: signed numbers without a registered '-'") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[-7, -3.14, +5]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 3);
  CHECK(t[0] == token_type::number_int);
  CHECK(t[1] == token_type::number_float);
  CHECK(t[2] == token_type::number_int);
}

TEST_CASE("lexer: operators are split by longest-match") {
  tavl::parser p;
  p.add_math_default_operators();              // registers =, +, -, *, ==, <=, ...
  const auto evs = tavl_test::poll_all(p, "[a, +, b, ==, c]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 5);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::op);
  CHECK(t[2] == token_type::identifier);
  CHECK(t[3] == token_type::op);
  CHECK(t[4] == token_type::identifier);
}

TEST_CASE("lexer: with a registered '-' a number is split into op + number") {
  tavl::parser p;
  p.add_math_default_operators();
  const auto evs = tavl_test::poll_all(p, "[-7]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 2);
  CHECK(t[0] == token_type::op);               // unary minus
  CHECK(t[1] == token_type::number_int);
}

TEST_CASE("lexer: datetime takes precedence over the '-' operator") {
  tavl::parser p;
  p.add_math_default_operators();              // '-' is registered, but datetime must not split
  const auto evs = tavl_test::poll_all(p, "[2026-06-14T12:30:00]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 1);
  CHECK(t[0] == token_type::datetime);
}

TEST_CASE("lexer: comments do not appear in the data") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[a /* block */ b] // line\n");
  CHECK(tavl_test::count_event(evs, event_type::got_comment) == 2);
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 2);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::identifier);
}

TEST_CASE("lexer: nested block comments close only at the matching */") {
  tavl::parser p;
  // The inner */ must not end the comment; only the final */ does. Everything between the
  // outermost /* and its match is a single comment, and "still outer" is not lexed as data.
  const auto evs = tavl_test::poll_all(p, "[a /* outer /* inner */ still outer */ b]");

  REQUIRE_FALSE(tavl_test::has_critical(evs));
  CHECK(tavl_test::count_event(evs, event_type::got_comment) == 1);
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 2);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::identifier);

  // The single comment spans the whole nested region.
  for (const auto& e : evs) {
    if (e.event.type == event_type::got_comment)
      CHECK(p.content(e.event.token.span) == "/* outer /* inner */ still outer */");
  }
}

TEST_CASE("lexer: deeply nested block comments balance correctly") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[a /* 1 /* 2 /* 3 */ 2 */ 1 */ b /* x */ c]");

  REQUIRE_FALSE(tavl_test::has_critical(evs));
  CHECK(tavl_test::count_event(evs, event_type::got_comment) == 2);
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 3);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::identifier);
  CHECK(t[2] == token_type::identifier);
}

TEST_CASE("lexer: byte-by-byte streaming handles nested block comments") {
  const std::string_view src = "[a /* outer /* inner */ still outer */ b]";

  tavl::parser stream;
  stream.clear();
  std::vector<token_type> tokens;
  size_t comments = 0;
  size_t i = 0;
  bool finished = false;
  while (true) {
    auto [ev, err] = stream.poll_event();
    if (ev.type == event_type::got_token) tokens.push_back(ev.token.type);
    if (ev.type == event_type::got_comment) comments += 1;
    if (ev.type == event_type::eof) break;
    if (ev.type == event_type::not_enought_data) {
      if (i < src.size()) { stream.flush(src.substr(i, 1)); i += 1; }
      else if (!finished) { stream.finish(); finished = true; }
    }
  }

  REQUIRE(tokens.size() == 2);
  CHECK(tokens[0] == token_type::identifier);
  CHECK(tokens[1] == token_type::identifier);
  CHECK(comments == 1);
}

TEST_CASE("lexer: values are converted via parser::to_*") {
  tavl::parser p;
  // Collect tokens from an array and verify conversions.
  p.clear();
  p.flush("[true, -42, 0xFF, 3.5, \"hi\\nthere\", 2026-06-14T12:30:00]");
  p.finish();

  std::vector<tavl::token> toks;
  while (true) {
    auto [ev, err] = p.poll_event();
    if (ev.type == event_type::got_token) toks.push_back(ev.token);
    if (ev.type == event_type::eof) break;
  }
  REQUIRE(toks.size() == 6);

  CHECK(p.to_boolean(toks[0]) == true);
  CHECK(p.to_int(toks[1]) == -42);
  CHECK(p.to_uint(toks[2]) == 0xFFu);
  CHECK(p.to_float(toks[3]) == doctest::Approx(3.5));
  CHECK(p.to_string(toks[4]) == "hi\nthere");         // \n unescaped
  const auto dt = p.to_datetime(toks[5]);
  REQUIRE(dt.has_value());
  CHECK(dt->y == 2026);
  CHECK(dt->m == 6);
  CHECK(dt->d == 14);
  CHECK(dt->hh == 12);
}

TEST_CASE("lexer: malformed scalar tokens are reported as unrecognized") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[123abc, 0xZZ, @bad]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 3);
  CHECK(t[0] == token_type::unrecognized);
  CHECK(t[1] == token_type::unrecognized);
  CHECK(t[2] == token_type::unrecognized);
}

TEST_CASE("lexer: byte-by-byte streaming yields the same token stream as all at once") {
  const std::string_view src = "[null, 42, \"a b\", ident]";

  tavl::parser whole;
  const auto a = tavl_test::got_token_types(tavl_test::poll_all(whole, src));

  // byte by byte
  tavl::parser stream;
  stream.clear();
  std::vector<token_type> b;
  size_t i = 0;
  bool finished = false;
  while (true) {
    auto [ev, err] = stream.poll_event();
    if (ev.type == event_type::got_token) b.push_back(ev.token.type);
    if (ev.type == event_type::eof) break;
    if (ev.type == event_type::not_enought_data) {
      if (i < src.size()) { stream.flush(src.substr(i, 1)); i += 1; }
      else if (!finished) { stream.finish(); finished = true; }
    }
  }

  CHECK(a == b);
}

TEST_CASE("lexer: byte-by-byte streaming handles comments and escaped strings") {
  const std::string_view src = "[\"a\\\"b\", /* block */ c] // line\n";

  tavl::parser stream;
  stream.clear();
  std::vector<token_type> tokens;
  size_t comments = 0;
  std::string first_string;
  size_t i = 0;
  bool finished = false;
  while (true) {
    auto [ev, err] = stream.poll_event();
    if (ev.type == event_type::got_token) {
      tokens.push_back(ev.token.type);
      if (first_string.empty() && ev.token.type == token_type::doublequote_string)
        first_string = stream.to_string(ev.token);
    }
    if (ev.type == event_type::got_comment) comments += 1;
    if (ev.type == event_type::eof) break;
    if (ev.type == event_type::not_enought_data) {
      if (i < src.size()) { stream.flush(src.substr(i, 1)); i += 1; }
      else if (!finished) { stream.finish(); finished = true; }
    }
  }

  REQUIRE(tokens.size() == 2);
  CHECK(tokens[0] == token_type::doublequote_string);
  CHECK(tokens[1] == token_type::identifier);
  CHECK(first_string == "a\"b");
  CHECK(comments == 2);
}
