// Demonstration .tavl files parse without critical errors.

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <string_view>
#include <iterator>
#include <vector>

#include "tavl/tavl.h"

#ifndef TAVL_FORMAT_DIR
#define TAVL_FORMAT_DIR "."
#endif

static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Minimal parser loop: flush the whole input, finish it, then poll_event() through eof. Every event
// carries an attached error; a critical error makes the example invalid. eof ends the event stream.
static void check_parses_clean(const char* name) {
  const std::string path = std::string(TAVL_FORMAT_DIR) + "/" + name;
  const std::string src = read_file(path);
  REQUIRE_MESSAGE(!src.empty(), "failed to read " << path);

  tavl::parser p;
  p.add_default_operator();            // '=' is the row operator for these examples

  p.flush(src);
  p.finish();

  bool critical = false;
  bool reached_eof = false;
  while (true) {
    const auto [ev, err] = p.poll_event();
    if (err.is_critical()) critical = true;
    if (ev.type == tavl::event_type::eof) { reached_eof = true; break; }
  }

  CHECK_FALSE(critical);
  CHECK(reached_eof);
}

TEST_CASE("format example: types.tavl")            { check_parses_clean("types.tavl"); }
TEST_CASE("format example: blocks.tavl")           { check_parses_clean("blocks.tavl"); }
TEST_CASE("format example: comments_strings.tavl") { check_parses_clean("comments_strings.tavl"); }

TEST_CASE("format example: ordinary-profile diagnostics are stable across chunks") {
  const std::string src = read_file(std::string(TAVL_FORMAT_DIR) + "/diagnostics.tavl");
  REQUIRE(!src.empty());

  struct diagnostic {
    tavl::event_type event;
    tavl::error_type error;
    size_t line, column;
    bool operator==(const diagnostic&) const = default;
  };
  const std::vector<diagnostic> expected = {
    {tavl::event_type::row_end, tavl::error_type::warn_object_without_value, 1, 11},
    {tavl::event_type::got_token, tavl::error_type::err_expected_identifier, 2, 17},
    {tavl::event_type::row_end, tavl::error_type::err_missing_value, 3, 17},
    {tavl::event_type::got_token, tavl::error_type::err_invalid_escape, 4, 11},
    {tavl::event_type::got_token, tavl::error_type::err_float_out_of_range, 5, 9},
    {tavl::event_type::empty_row, tavl::error_type::warn_empty_row, 6, 13},
    // Duplicate fields are diagnosed by the generic CLI, not the event parser.
    {tavl::event_type::array_end, tavl::error_type::err_bracket_missmatch, 8, 1},
    {tavl::event_type::got_comment, tavl::error_type::err_unterminated_block_comment, 9, 1},
  };

  const auto collect = [&](std::string_view source, size_t chunk_size) {
    tavl::parser p;
    p.add_default_operator();
    std::vector<diagnostic> found;
    const auto drain = [&]() {
      for (;;) {
        const auto [ev, err] = p.poll_event();
        if (!err.no_error())
          found.push_back({ev.type, err.type, err.span.line, err.span.column});
        if (ev.type == tavl::event_type::eof) return true;
        if (ev.type == tavl::event_type::not_enought_data) return false;
      }
    };
    for (size_t i = 0; i < source.size(); i += chunk_size) {
      p.flush(source.substr(i, chunk_size));
      if (drain()) return found;
    }
    p.finish();
    (void)drain();
    return found;
  };

  CHECK(collect(src, src.size()) == expected);
  CHECK(collect(src, 1) == expected);

  const std::string unterminated = read_file(
    std::string(TAVL_FORMAT_DIR) + "/unterminated_string.tavl");
  REQUIRE(!unterminated.empty());
  const std::vector<diagnostic> expected_string = {
    {tavl::event_type::got_token, tavl::error_type::err_unterminated_string, 1, 8},
  };
  CHECK(collect(unterminated, unterminated.size()) == expected_string);
  CHECK(collect(unterminated, 1) == expected_string);
}
