// Parser: blocks, row modes, parse_mode override.

#include <doctest/doctest.h>

#include <vector>
#include <string>
#include <optional>

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
  CHECK(tavl_test::count_event(evs, event_type::got_token)          == 1);   // value only
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

  // By default {} is object_like and emits got_row_identifier.
  const auto def = tavl_test::poll_all(p, "{ k = v }");
  REQUIRE(tavl_test::count_event(def, event_type::got_row_identifier) == 1);

  // Override object -> data_driven: row identifier is not separated.
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
  p.flush("{ a = { b = c } }");   // both {} are data_driven; the inner one inherits the map
  p.finish();

  std::vector<tavl_test::ev_err> evs;
  while (true) {
    auto [ev, err] = p.poll_event();
    evs.push_back({ev, err});
    if (ev.type == event_type::eof) break;
  }
  CHECK(tavl_test::count_event(evs, event_type::got_row_identifier) == 0);   // no block extracts identifiers
  CHECK(tavl_test::count_event(evs, event_type::object_begin) == 2);
}

TEST_CASE("parser: a row identifier can be any token except brackets") {
  tavl::parser p;
  p.add_default_operator();
  // First non-operator token of each row becomes the identifier, regardless of token type.
  const auto evs = tavl_test::poll_all(p, "42 = a\n\"k\" = b\n2026-01-01 = c\ntrue = d");

  std::vector<token_type> ids;
  for (const auto& e : evs)
    if (e.event.type == event_type::got_row_identifier) ids.push_back(e.event.token.type);

  REQUIRE(ids.size() == 4);
  CHECK(ids[0] == token_type::number_int);          // number
  CHECK(ids[1] == token_type::doublequote_string);  // string
  CHECK(ids[2] == token_type::datetime);            // date
  CHECK(ids[3] == token_type::boolean);             // boolean
}

TEST_CASE("parser: peek returns the next event without consuming it") {
  tavl::parser p;
  p.add_default_operator();
  p.flush("a = 1");
  p.finish();

  const auto peeked = p.peek();
  const auto [polled, err] = p.poll_event();
  CHECK(err.type == tavl::error_type::no_error);
  CHECK(peeked.type == polled.type);
  CHECK(peeked.token.type == polled.token.type);
  CHECK(peeked.token.span.offset == polled.token.span.offset);
}

TEST_CASE("document separator: //--- is recognized, a normal comment is not") {
  tavl::parser p;
  p.add_default_operator();
  p.flush("a = 1\n//--- \t\r\nb = 2\n// normal comment");
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
  CHECK_FALSE(any_sep);   // string token, not a comment
}

TEST_CASE("document separator: buffered next document is not consumed by finish") {
  struct input_case {
    std::vector<std::string_view> chunks;
    std::string_view buffered_tail;
    std::string_view later;
  };
  for (const auto& input : {
         input_case{{"a=b,c=v,123//---\na=c,c="}, "a=c,c=", "352,6463"},
         input_case{{"a=b,c=v,123", "//---\na=c,c="}, "a=c,c=", "352,6463"},
         input_case{{"a=b,c=v,123/", "/---\na=c,c="}, "a=c,c=", "352,6463"},
         input_case{{"a=b,c=v,123", "//---\na=c,c=352,6463"},
                    "a=c,c=352,6463", ""}}) {
    tavl::parser p;
    p.add_default_operator();
    std::vector<std::string> before;
    std::vector<std::string> after;
    bool critical = false;
    const auto record = [&](const tavl::event& ev, const tavl::error& err,
                            std::vector<std::string>& output) {
      critical |= err.is_critical();
      if (ev.type == event_type::got_row_identifier || ev.type == event_type::got_token)
        output.emplace_back(p.content(ev.token.span));
    };

    std::optional<std::string> remainder;
    for (std::string_view chunk : input.chunks) {
      p.flush(chunk);
      for (;;) {
        const auto [ev, err] = p.poll_event();
        if (ev.type == event_type::not_enought_data) break;
        record(ev, err, before);
        if (ev.type == event_type::got_comment && tavl::is_document_separator(p, ev.token)) {
          remainder = tavl::finish_at_document_separator(p, ev.token);
          break;
        }
      }
      if (remainder) break;
    }
    REQUIRE(remainder.has_value());
    CHECK(*remainder == input.buffered_tail);
    for (;;) {
      const auto [ev, err] = p.poll_event();
      record(ev, err, before);
      if (ev.type == event_type::eof) break;
    }
    CHECK(before == std::vector<std::string>{"a", "b", "c", "v", "123"});
    CHECK_FALSE(critical);

    p.clear();
    p.flush(*remainder);
    for (;;) {
      const auto [ev, err] = p.poll_event();
      if (ev.type == event_type::not_enought_data) break;
      record(ev, err, after);
    }
    CHECK_FALSE(critical); // an incomplete trailing row is not an EOF error yet
    p.flush(input.later);
    p.finish();
    for (;;) {
      const auto [ev, err] = p.poll_event();
      record(ev, err, after);
      if (ev.type == event_type::eof) break;
    }
    CHECK(after == std::vector<std::string>{"a", "c", "c", "352", "6463"});
    CHECK_FALSE(critical);
  }
}
