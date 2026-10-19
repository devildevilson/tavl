#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <string>
#include <chrono>
#include <algorithm>
#include <optional>
#include <vector>

#include "tavl/common.h"

// Low-level parsing utilities: character classification, type predicates,
// and handling of string escape sequences. Pulled out here and made public
// so the lexer, the parser, and external code can all use them.
namespace tavl::detail {

// ':' is allowed here so a consumer can register it as an operator: 'name : type' reads better than
// a '(name, type)' tuple in a schema-like document, and without the registration 'name:type' would
// not be split at all. Datetime keeps its priority in make_token (it contains ':' too), so a
// registered ':' cannot break '2026-09-02T20:00:00'.
inline constexpr std::string_view operator_chars = "!@#$%^&*-+=<>/?|~:";
inline constexpr std::string_view identificator_start_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
inline constexpr std::string_view identificator_every_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_.0123456789";
inline constexpr std::string_view binary_chars = "01";
inline constexpr std::string_view octal_chars = "01234567";
inline constexpr std::string_view hex_chars = "0123456789ABCDEFabcdef";

inline constexpr bool check_chars(const char c, const std::string_view& arr) {
  bool any = false;
  for (size_t i = 0; i < arr.size() && !any; ++i) { any = any || c == arr[i]; }
  return any;
}

inline constexpr bool check_valid_string_chars(const std::string_view& str, const std::string_view& arr) {
  for (const char c : str) {
    if (!check_chars(c, arr)) return false;
  }

  return true;
}

inline constexpr bool is_whitespace_except_newline(const char c) {
  constexpr std::string_view matches = " \r\f\v\t";
  return check_chars(c, matches);
}

inline constexpr bool is_valid_identificator(const std::string_view& str) {
  if (str.empty()) return false;
  if (!check_chars(str[0], identificator_start_chars)) return false;
  return check_valid_string_chars(str.substr(1), identificator_every_chars);
}

bool is_boolean(const std::string_view& str, bool& out);
bool is_null(const std::string_view& str);
bool is_bin_number(const std::string_view& str, uint64_t& out);
bool is_oct_number(const std::string_view& str, uint64_t& out);
bool is_hex_number(const std::string_view& str, uint64_t& out);
bool is_dec_number(const std::string_view& str, int64_t& out);
bool is_dec_unsigned_number(const std::string_view& str, uint64_t& out);
enum class float_parse_result { invalid, valid, out_of_range };
// The lexer keeps this scratch space across tokens and parser::clear(). Large decimal
// tokens may grow it, but ordinary parsing then reuses that capacity.
struct float_big_uint {
  std::vector<uint32_t> words{0};
  explicit float_big_uint(uint32_t value = 0) : words{value} {}
  static float_big_uint from_u64(uint64_t value);
  void reset(uint32_t value);
  void copy_from(const float_big_uint& other);
  void normalize();
  void mul_small(uint32_t factor);
  void add_small(uint32_t value);
  void add(const float_big_uint& other);
  void shift_left(unsigned bits);
  void mul_u64(uint64_t factor, float_big_uint& upper);
  uint32_t div_small(uint32_t divisor);
  std::string decimal() const;
  int compare(const float_big_uint& other) const noexcept;
};
struct float_parse_workspace {
  std::string digits;
  float_big_uint numerator, denominator, left, right, upper;
};
float_parse_result parse_float_number(std::string_view str, double& val);
float_parse_result parse_float_number(std::string_view str, double& val,
                                      float_parse_workspace& workspace);
bool is_float_number(const std::string_view& str, double& val);
double integer_to_binary64(uint64_t magnitude, bool negative = false) noexcept;
// Deterministic shortest-significand spelling for an IEEE 754 binary64 value.
// The result is always a TAVL float token and round-trips through parse_float_number.
std::string format_float_canonical(double value);

bool parse_int(std::string_view& s, int32_t& out, const size_t n);
bool is_datetime(const std::string_view& str, iso_datetime& data);

// Offset of the first ill-formed UTF-8 byte sequence, if any.
std::optional<size_t> invalid_utf8(std::string_view input) noexcept;
// First source-text violation in byte order. Source validation also rejects
// literal NUL and C0/DEL/C1 controls except TAB/LF/CR/FF/VT.
struct source_text_issue {
  error_type type;
  size_t offset;
  size_t size;
};
std::optional<source_text_issue> invalid_source_text(std::string_view input) noexcept;

// Convert string literal contents (without quotes) into the final string:
// single quotes have minimal escaping, double quotes support standard escapes and \u/\U Unicode.
void unescape_singlequote_string(const std::string_view& input, std::string& result);
void unescape_doublequote_string(const std::string_view& input, std::string& result);
// Return the offset of the first invalid escape in a double-quoted string body.
std::optional<size_t> invalid_doublequote_escape(std::string_view input);

// Serialization escape helpers without temporary buffers: each output byte is sent to put(char)->bool.
// put may return false to stop (for example, bounded output reached capacity), and escape_*_to
// propagates that false. serialize.h uses this to write straight into the destination string.

// Minimal escape: syntax bytes and controls forbidden as raw source text.
// TAB/LF/CR/FF/VT remain literal; C1 UTF-8 sequences become \u00XX.
template <typename Put>
bool escape_doublequote_minimal_to(std::string_view input, Put&& put) {
  constexpr std::string_view hex = "0123456789abcdef";
  const auto emit_u00 = [&put, hex](unsigned c) {
    return put('\\') && put('u') && put('0') && put('0') &&
           put(hex[(c >> 4) & 0xf]) && put(hex[c & 0xf]);
  };
  for (size_t i = 0; i < input.size(); ++i) {
    const auto c = static_cast<unsigned char>(input[i]);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' &&
        c != '\f' && c != '\v') {
      if (!emit_u00(c)) return false;
      continue;
    }
    if (c == 0x7f) {
      if (!emit_u00(c)) return false;
      continue;
    }
    if (c == 0xc2 && i + 1 < input.size()) {
      const auto next = static_cast<unsigned char>(input[i + 1]);
      if (next >= 0x80 && next <= 0x9f) {
        if (!emit_u00(next)) return false;
        ++i;
        continue;
      }
    }
    if ((c == '\\' || c == '\"') && !put('\\')) return false;
    if (!put(static_cast<char>(c))) return false;
  }
  return true;
}

// Full escape: backslash/quote, control chars, and Unicode. UTF-8 bytes >= 0x80 are decoded into
// \uXXXX / \UXXXXXXXX so the result is ASCII and round-trips through unescape_doublequote_string.
template <typename Put>
bool escape_doublequote_full_to(std::string_view input, Put&& put) {
  const auto emit_u = [&put](uint32_t cp) -> bool {
    char buf[16];
    const int k = cp <= 0xFFFF ? std::snprintf(buf, sizeof(buf), "\\u%04x", cp)
                               : std::snprintf(buf, sizeof(buf), "\\U%08x", cp);
    const size_t n = k > 0 ? std::min(static_cast<size_t>(k), sizeof(buf)) : 0;
    for (size_t i = 0; i < n; ++i) if (!put(buf[i])) return false;
    return true;
  };
  const auto esc = [&put](char c) { return put('\\') && put(c); };

  const size_t n = input.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(input[i]);
    if (c < 0x80) {                                   // ASCII
      bool ok = true;
      switch (c) {
        case '\\': ok = esc('\\'); break;
        case '\"': ok = esc('\"'); break;
        case '\n': ok = esc('n');  break;
        case '\r': ok = esc('r');  break;
        case '\t': ok = esc('t');  break;
        case '\a': ok = esc('a');  break;
        case '\b': ok = esc('b');  break;
        case '\f': ok = esc('f');  break;
        case '\v': ok = esc('v');  break;
        default:   ok = (c < 0x20) ? emit_u(c) : put(static_cast<char>(c));
      }
      if (!ok) return false;
      i += 1;
      continue;
    }

    uint32_t cp = 0;                                  // multibyte UTF-8 -> code point
    int len = 0;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }

    bool ok = len != 0 && i + static_cast<size_t>(len) <= n;
    for (int j = 1; ok && j < len; ++j) {
      const unsigned char cc = static_cast<unsigned char>(input[i + j]);
      if ((cc & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) { if (!emit_u(c)) return false; i += 1; continue; }  // malformed/truncated UTF-8
    if (!emit_u(cp)) return false;
    i += static_cast<size_t>(len);
  }
  return true;
}

// Convert to/from an absolute instant at nanosecond precision. Missing timezone means
// the system's local time; Z or an explicit numeric offset yields an unambiguous instant.
std::optional<std::chrono::nanoseconds> iso_datetime_to_unix_ns(const iso_datetime& dt);
iso_datetime unix_ns_to_iso_datetime(std::chrono::nanoseconds ns, bool utc = false);
// Render YYYY-MM-DD<sep>HH:MM:SS[.fraction][Z|+HH:MM|-HH:MM].
// char buf[48] is enough; sep is '_' by default or 'T'.
size_t format_iso_datetime(const iso_datetime& dt, char* buf, size_t cap, char sep = '_');

}
