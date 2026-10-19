#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "tavl/tavl.h"

namespace {

std::vector<std::string> fields(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const size_t tab = line.find('\t', start);
    out.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
    if (tab == std::string::npos) return out;
    start = tab + 1;
  }
}

uint64_t hex_number(std::string_view text) {
  uint64_t value = 0;
  for (char c : text) {
    const unsigned digit = c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10);
    value = value * 16 + digit;
  }
  return value;
}

std::string hex_bytes(std::string_view hex) {
  std::string result;
  for (size_t i = 0; i < hex.size(); i += 2)
    result.push_back(char(hex_number(hex.substr(i, 2))));
  return result;
}

void append_number(std::string& out, size_t value) {
  out += value == SIZE_MAX ? "-" : std::to_string(value);
}

std::string event_snapshot(tavl::parser& parser, std::string_view source,
                           const tavl::event& event, const tavl::error& error) {
  const auto& s = event.token.span;
  std::string out;
  out += tavl::to_string(event.type);
  out += ':';
  out += tavl::to_string(event.token.type);
  out += ':';
  append_number(out, s.offset);
  out += ':';
  append_number(out, s.size);
  out += ':';
  append_number(out, s.line);
  out += ':';
  append_number(out, s.column);
  out += ':';
  out += tavl::to_string(error.type);
  // A recovery event can refer to an opener already released by a streaming caller.
  // Its source span remains valid even when content() is no longer resolvable.
  const auto bytes = s.offset <= source.size() && s.size <= source.size() - s.offset
    ? source.substr(s.offset, s.size) : std::string_view{};
  if (error.type != tavl::error_type::err_bracket_missmatch && s.size)
    CHECK(parser.content(s) == bytes);
  // Include source bytes, encoded as hex, so span drift cannot hide a wrong token boundary.
  out += ':';
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned char c : bytes) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

std::string parse_events(const std::string& source, const std::string& profile,
                         size_t chunk, bool release) {
  tavl::parser p;
  if (profile == "equal") p.add_default_operator();
  else if (profile == "colon") p.add_operator(":", tavl::op_fixity::binary, 1);
  else if (profile == "math") p.add_math_default_operators();
  else if (profile == "postfix") {
    p.add_math_default_operators();
    p.add_operator("!", tavl::op_fixity::postfix, 17);
  }
  std::string result;
  size_t pos = 0;
  for (;;) {
    const auto [event, error] = p.poll_event();
    if (event.type == tavl::event_type::not_enought_data) {
      if (release) p.release_before(p.consumed_offset());
      if (pos < source.size()) {
        const size_t count = std::min(chunk, source.size() - pos);
        p.flush(std::string_view(source).substr(pos, count));
        pos += count;
      } else p.finish();
      continue;
    }
    if (!result.empty()) result += ';';
    result += event_snapshot(p, source, event, error);
    if (release) p.release_before(p.consumed_offset());
    if (event.type == tavl::event_type::eof) return result;
  }
}

}  // namespace

TEST_CASE("conformance: independent decimal-to-binary64 vectors") {
  std::ifstream file(std::string(TAVL_CONFORMANCE_DIR) + "/numeric.tsv");
  REQUIRE(file.good());
  std::string line;
  size_t cases = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto f = fields(line);
    REQUIRE(f.size() == 4);
    INFO("numeric case ", f[0]);
    if (f[0] == "parse") {
      double value = 17;
      const auto status = tavl::detail::parse_float_number(f[1], value);
      CHECK((status == tavl::detail::float_parse_result::valid ? "valid" :
             status == tavl::detail::float_parse_result::out_of_range ? "out_of_range" : "invalid") == f[2]);
      if (status == tavl::detail::float_parse_result::valid)
        CHECK(std::bit_cast<uint64_t>(value) == hex_number(f[3]));
      else CHECK(value == 17);
    } else if (f[0] == "format") {
      const double value = std::bit_cast<double>(hex_number(f[1]));
      CHECK(tavl::detail::format_float_canonical(value) == f[3]);
    } else FAIL("unknown numeric case kind");
    ++cases;
  }
  CHECK(cases >= 60);
}

TEST_CASE("conformance: exact format events, spans, and diagnostics across chunks") {
  std::ifstream file(std::string(TAVL_CONFORMANCE_DIR) + "/format.tsv");
  REQUIRE(file.good());
  std::string line;
  size_t cases = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto f = fields(line);
    REQUIRE(f.size() == 4);
    INFO("format case ", f[0]);
    const auto source = hex_bytes(f[2]);
    CHECK(parse_events(source, f[1], source.size() + 1, false) == f[3]);
    for (size_t chunk : {size_t{1}, size_t{2}, size_t{7}})
      CHECK(parse_events(source, f[1], chunk, true) == f[3]);
    ++cases;
  }
  CHECK(cases >= 80);
}
