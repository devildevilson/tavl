// Парсер: блоки, режимы строк, подмена parse_mode.

#include <doctest/doctest.h>

#include <vector>

#include "util.h"

using tavl::event_type;
using tavl::parse_mode;
using tavl::token_type;

TEST_CASE("parser: block types produce paired begin/end events") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "obj = {a=1}\ntup = (1, 2)\narr = [1, 2]");

  CHECK(tavl_test::count_event(evs, event_type::object_begin) == 1);
  CHECK(tavl_test::count_event(evs, event_type::object_end)   == 1);
  CHECK(tavl_test::count_event(evs, event_type::tuple_begin)  == 1);
  CHECK(tavl_test::count_event(evs, event_type::tuple_end)    == 1);
  CHECK(tavl_test::count_event(evs, event_type::array_begin)  == 1);
  CHECK(tavl_test::count_event(evs, event_type::array_end)    == 1);
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("parser: object_like extracts the row identifier and operator") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ k = v }");

  CHECK(tavl_test::count_event(evs, event_type::got_row_identifier) == 1);
  CHECK(tavl_test::count_event(evs, event_type::got_row_operator)   == 1);
  CHECK(tavl_test::count_event(evs, event_type::got_token)          == 1);   // только значение
}

TEST_CASE("parser: array (data_driven) treats all tokens as data") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "[ k = v ]");

  CHECK(tavl_test::count_event(evs, event_type::got_row_identifier) == 0);
  CHECK(tavl_test::count_event(evs, event_type::got_token)          == 3);   // k, =, v
}

TEST_CASE("parser: tuple is a hybrid (id+operator -> like object, otherwise data)") {
  tavl::parser p;
  p.add_default_operator();

  const auto named = tavl_test::poll_all(p, "(x = 10)");
  CHECK(tavl_test::count_event(named, event_type::got_row_identifier) == 1);

  const auto positional = tavl_test::poll_all(p, "(10, 20, 30)");
  CHECK(tavl_test::count_event(positional, event_type::got_row_identifier) == 0);
  CHECK(tavl_test::count_event(positional, event_type::got_token) == 3);
}

TEST_CASE("parser: nested blocks are counted correctly") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "a = { b = { c = [1, 2] } }");
  CHECK(tavl_test::count_event(evs, event_type::object_begin) == 2);
  CHECK(tavl_test::count_event(evs, event_type::object_end)   == 2);
  CHECK(tavl_test::count_event(evs, event_type::array_begin)  == 1);
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("parser: override_next_block_modes changes the mode of the next block") {
  tavl::parser p;
  p.add_default_operator();

  // по умолчанию {} -> object_like: есть got_row_identifier
  const auto def = tavl_test::poll_all(p, "{ k = v }");
  REQUIRE(tavl_test::count_event(def, event_type::got_row_identifier) == 1);

  // подменяем object -> data_driven: идентификатор строки не выделяется
  p.clear();
  p.add_default_operator();
  p.override_next_block_modes(parse_mode::data_driven, parse_mode::tuple_like, parse_mode::data_driven);
  p.flush("{ k = v }");
  p.finish();
  std::vector<tavl_test::ev_err> evs;
  while (true) {
    auto [ev, err] = p.poll_event();
    evs.push_back({ev, err});
    if (ev.type == event_type::eof) break;
  }
  CHECK(tavl_test::count_event(evs, event_type::got_row_identifier) == 0);
  CHECK(tavl_test::count_event(evs, event_type::got_token) == 3);
}

TEST_CASE("parser: the mode override is inherited by descendants") {
  tavl::parser p;
  p.add_default_operator();
  p.override_next_block_modes(parse_mode::data_driven, parse_mode::tuple_like, parse_mode::data_driven);
  p.flush("{ a = { b = c } }");   // оба {} -> data_driven (внутренний наследует карту)
  p.finish();

  std::vector<tavl_test::ev_err> evs;
  while (true) {
    auto [ev, err] = p.poll_event();
    evs.push_back({ev, err});
    if (ev.type == event_type::eof) break;
  }
  CHECK(tavl_test::count_event(evs, event_type::got_row_identifier) == 0);   // ни в одном блоке
  CHECK(tavl_test::count_event(evs, event_type::object_begin) == 2);
}

TEST_CASE("parser: a row identifier can be any token except brackets") {
  tavl::parser p;
  p.add_default_operator();
  // первый токен-неоператор каждой строки становится идентификатором независимо от типа
  const auto evs = tavl_test::poll_all(p, "42 = a\n\"k\" = b\n2026-01-01 = c\ntrue = d");

  std::vector<token_type> ids;
  for (const auto& e : evs)
    if (e.event.type == event_type::got_row_identifier) ids.push_back(e.event.token.type);

  REQUIRE(ids.size() == 4);
  CHECK(ids[0] == token_type::number_int);          // число
  CHECK(ids[1] == token_type::doublequote_string);  // строка
  CHECK(ids[2] == token_type::datetime);            // дата
  CHECK(ids[3] == token_type::boolean);             // boolean
}

TEST_CASE("document separator: //--- is recognized, a normal comment is not") {
  tavl::parser p;
  p.add_default_operator();
  p.flush("a = 1\n//---\nb = 2\n// normal comment");
  p.finish();

  int comments = 0, seps = 0;
  while (true) {
    const auto [ev, err] = p.poll_event();
    if (ev.type == event_type::got_comment) {
      ++comments;
      if (tavl::is_document_separator(p, ev.token)) ++seps;
    }
    if (ev.type == event_type::eof) break;
  }
  CHECK(comments == 2);
  CHECK(seps == 1);
}

TEST_CASE("document separator: //--- inside a string is not a separator") {
  tavl::parser p;
  p.add_default_operator();
  p.flush("x = \"//---\"");
  p.finish();

  bool any_sep = false;
  while (true) {
    const auto [ev, err] = p.poll_event();
    if (tavl::is_document_separator(p, ev.token)) any_sep = true;
    if (ev.type == event_type::eof) break;
  }
  CHECK_FALSE(any_sep);   // это строковый токен, а не комментарий
}
