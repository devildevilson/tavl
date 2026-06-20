// Demonstration .tavl files parse without critical errors.

#include <doctest/doctest.h>

#include <fstream>
#include <string>
#include <string_view>
#include <iterator>

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
