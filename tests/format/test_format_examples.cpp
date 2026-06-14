// Демонстрационные .tavl парсятся без критических ошибок.

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

// Минимальный обход парсера: флашим весь вход, finish() (данных больше не будет), затем тянем
// poll_event() до eof. К каждому событию прилагается ошибка; критическая (err.is_critical())
// делает вход невалидным. eof — конец потока событий.
static void check_parses_clean(const char* name) {
  const std::string path = std::string(TAVL_FORMAT_DIR) + "/" + name;
  const std::string src = read_file(path);
  REQUIRE_MESSAGE(!src.empty(), "не удалось прочитать " << path);

  tavl::parser p;
  p.add_default_operator();            // '=' — оператор строк формата

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
