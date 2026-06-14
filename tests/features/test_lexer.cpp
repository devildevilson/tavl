// Лексер: распознавание типов токенов и стриминг.

#include <doctest/doctest.h>

#include "util.h"

using tavl::token_type;
using tavl::event_type;

TEST_CASE("lexer: распознавание скалярных типов") {
  tavl::parser p;   // без зарегистрированных операторов
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

TEST_CASE("lexer: знаковые числа без зарегистрированного '-'") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[-7, -3.14, +5]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 3);
  CHECK(t[0] == token_type::number_int);
  CHECK(t[1] == token_type::number_float);
  CHECK(t[2] == token_type::number_int);
}

TEST_CASE("lexer: операторы расщепляются по longest-match") {
  tavl::parser p;
  p.add_math_default_operators();              // регистрирует =, +, -, *, ==, <=, ...
  const auto evs = tavl_test::poll_all(p, "[a, +, b, ==, c]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 5);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::op);
  CHECK(t[2] == token_type::identifier);
  CHECK(t[3] == token_type::op);
  CHECK(t[4] == token_type::identifier);
}

TEST_CASE("lexer: при зарегистрированном '-' число расщепляется на op + number") {
  tavl::parser p;
  p.add_math_default_operators();
  const auto evs = tavl_test::poll_all(p, "[-7]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 2);
  CHECK(t[0] == token_type::op);               // унарный минус
  CHECK(t[1] == token_type::number_int);
}

TEST_CASE("lexer: datetime имеет приоритет над оператором '-'") {
  tavl::parser p;
  p.add_math_default_operators();              // '-' зарегистрирован, но дата не должна расщепиться
  const auto evs = tavl_test::poll_all(p, "[2026-06-14T12:30:00]");
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 1);
  CHECK(t[0] == token_type::datetime);
}

TEST_CASE("lexer: комментарии не попадают в данные") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[a /* block */ b] // line\n");
  CHECK(tavl_test::count_event(evs, event_type::got_comment) == 2);
  const auto t = tavl_test::got_token_types(evs);
  REQUIRE(t.size() == 2);
  CHECK(t[0] == token_type::identifier);
  CHECK(t[1] == token_type::identifier);
}

TEST_CASE("lexer: значения конвертируются через parser::to_*") {
  tavl::parser p;
  // соберём токены из массива и проверим преобразования
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
  CHECK(p.to_string(toks[4]) == "hi\nthere");         // \n разэкранирован
  const auto dt = p.to_datetime(toks[5]);
  REQUIRE(dt.has_value());
  CHECK(dt->y == 2026);
  CHECK(dt->m == 6);
  CHECK(dt->d == 14);
  CHECK(dt->hh == 12);
}

TEST_CASE("lexer: стриминг по 1 байту даёт тот же поток токенов, что и целиком") {
  const std::string_view src = "[null, 42, \"a b\", ident]";

  tavl::parser whole;
  const auto a = tavl_test::got_token_types(tavl_test::poll_all(whole, src));

  // по байту
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
