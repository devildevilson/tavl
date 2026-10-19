#include "tavl/detail.h"

#include <limits>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace tavl {

namespace {
check_handler g_check_handler = nullptr;
}

check_handler set_check_handler(check_handler h) {
  const check_handler prev = g_check_handler;
  g_check_handler = h;
  return prev;
}

void fail_check(const char* expr, const char* file, int line, const char* msg) {
  if (g_check_handler) { g_check_handler(expr, file, line, msg); return; }
  std::fprintf(stderr, "tavl: check failed: %s  [%s]  at %s:%d\n", msg, expr, file, line);
  std::abort();
}

// The only non-template serializer helper; kept here next to the predicates it uses.
bool ser_string_needs_quote(std::string_view s) {
  if (s.empty()) return true;
  if (!detail::is_valid_identificator(s)) return true;
  bool b; if (detail::is_boolean(s, b)) return true;
  if (detail::is_null(s)) return true;
  return false;
}

std::string_view to_string(const enum token_type type) noexcept {
  static constexpr std::string_view names[] = {
#define X(name) #name,
    TAVL_TOKEN_TYPE_LIST
#undef X
  };
  static constexpr size_t size = sizeof(names) / sizeof(names[0]);

  const size_t index = static_cast<size_t>(type);
  if (index >= size) return std::string_view();
  return names[index];
}

std::string_view to_string(const enum event_type type) noexcept {
  static constexpr std::string_view names[] = {
#define X(name) #name,
    TAVL_EVENT_TYPE_LIST
#undef X
  };
  static constexpr size_t size = sizeof(names) / sizeof(names[0]);

  const size_t index = static_cast<size_t>(type);
  if (index >= size) return std::string_view();
  return names[index];
}

std::string_view to_string(const enum error_type type) noexcept {
  static constexpr std::string_view names[] = {
#define X(name) #name,
    TAVL_ERROR_TYPE_LIST
#undef X
  };
  static constexpr size_t size = sizeof(names) / sizeof(names[0]);

  const size_t index = static_cast<size_t>(type);
  if (index >= size) return std::string_view();
  return names[index];
}

}

namespace tavl::detail {

namespace {
std::optional<source_text_issue> scan_source_text(std::string_view input,
                                                  bool check_controls) noexcept {
  const auto continuation = [](unsigned char c) { return c >= 0x80 && c <= 0xBF; };
  for (size_t i = 0; i < input.size();) {
    const auto a = static_cast<unsigned char>(input[i]);
    if (a < 0x80) {
      if (check_controls) {
        if (a == 0) return source_text_issue{error_type::err_forbidden_nul, i, 1};
        if ((a < 0x20 && a != '\t' && a != '\n' && a != '\r' &&
             a != '\f' && a != '\v') || a == 0x7f)
          return source_text_issue{error_type::err_forbidden_control, i, 1};
      }
      ++i;
      continue;
    }

    size_t length = 0;
    if (a >= 0xC2 && a <= 0xDF) length = 2;
    else if (a >= 0xE0 && a <= 0xEF) length = 3;
    else if (a >= 0xF0 && a <= 0xF4) length = 4;
    else return source_text_issue{error_type::err_invalid_utf8, i, 1};

    if (input.size() - i < length)
      return source_text_issue{error_type::err_invalid_utf8, i, 1};
    const auto b = static_cast<unsigned char>(input[i + 1]);
    if (!continuation(b)) return source_text_issue{error_type::err_invalid_utf8, i, 1};
    if ((a == 0xE0 && b < 0xA0) || (a == 0xED && b > 0x9F) ||
        (a == 0xF0 && b < 0x90) || (a == 0xF4 && b > 0x8F))
      return source_text_issue{error_type::err_invalid_utf8, i, 1};
    for (size_t j = 2; j < length; ++j)
      if (!continuation(static_cast<unsigned char>(input[i + j])))
        return source_text_issue{error_type::err_invalid_utf8, i, 1};
    if (check_controls && a == 0xc2 && b <= 0x9f)
      return source_text_issue{error_type::err_forbidden_control, i, 2};
    i += length;
  }
  return std::nullopt;
}
}  // namespace

std::optional<size_t> invalid_utf8(std::string_view input) noexcept {
  if (const auto issue = scan_source_text(input, false)) return issue->offset;
  return std::nullopt;
}

std::optional<source_text_issue> invalid_source_text(std::string_view input) noexcept {
  return scan_source_text(input, true);
}

bool is_boolean(const std::string_view &str, bool& out) {
  const bool is_true  = str == "true" || str == "TRUE";
  const bool is_false = str == "false" || str == "FALSE";
  out = is_true || is_false ? is_true : out;
  return is_true || is_false;
}

bool is_null(const std::string_view &str) {
  return str == "null" || str == "NULL";
}

static bool parse_unsigned_digits(std::string_view text, unsigned base,
                                  uint64_t limit, uint64_t& out) noexcept {
  if (text.empty()) return false;
  uint64_t value = 0;
  for (const char c : text) {
    unsigned digit;
    if (c >= '0' && c <= '9') digit = unsigned(c - '0');
    else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a') + 10;
    else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A') + 10;
    else return false;
    if (digit >= base || value > (limit - digit) / base) return false;
    value = value * base + digit;
  }
  out = value;
  return true;
}

bool is_bin_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0b" && part1 != "0B") return false;
  const auto part2 = str.substr(2);
  return parse_unsigned_digits(part2, 2, UINT64_MAX, out);
}

bool is_oct_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0o" && part1 != "0O") return false;
  const auto part2 = str.substr(2);
  return parse_unsigned_digits(part2, 8, UINT64_MAX, out);
}

bool is_hex_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0x" && part1 != "0X") return false;
  const auto part2 = str.substr(2);
  return parse_unsigned_digits(part2, 16, UINT64_MAX, out);
}

bool is_dec_number(const std::string_view &str, int64_t& out) {
  auto tmp = str;
  if (tmp.empty()) return false;
  if (tmp.front() == '+') {
    tmp.remove_prefix(1);
    if (tmp.empty() || tmp.front() == '-' || tmp.front() == '+') return false;
  }

  const bool negative = tmp.front() == '-';
  if (negative) tmp.remove_prefix(1);
  const uint64_t limit = uint64_t(INT64_MAX) + uint64_t(negative);
  uint64_t magnitude = 0;
  if (!parse_unsigned_digits(tmp, 10, limit, magnitude)) return false;
  out = negative ? (magnitude == limit ? INT64_MIN : -static_cast<int64_t>(magnitude))
                 : static_cast<int64_t>(magnitude);
  return true;
}

bool is_dec_unsigned_number(const std::string_view &str, uint64_t& out) {
  auto tmp = str;
  if (tmp.empty()) return false;
  if (tmp.front() == '+') tmp.remove_prefix(1);
  if (tmp.empty()) return false;
  return parse_unsigned_digits(tmp, 10, UINT64_MAX, out);
}

bool parse_int(std::string_view& s, int32_t& out, const size_t n) {
  if (n == 0 || s.size() < n) return false;
  uint64_t value = 0;
  if (!parse_unsigned_digits(s.substr(0, n), 10, INT32_MAX, value)) return false;
  out = static_cast<int32_t>(value);
  s.remove_prefix(n);
  return true;
}

bool is_datetime(const std::string_view &str, iso_datetime& data) {
  if (str.empty()) return false;

  data = iso_datetime{};

  auto tmp = str;

  bool has_date = false;
  bool has_time = false;

  // Date: YYYY-MM-DD.
  if (tmp.size() >= 10 && tmp[4] == '-' && tmp[7] == '-') {
    if (!parse_int(tmp, data.y, 4)) return false;
    tmp.remove_prefix(1); // '-'
    if (!parse_int(tmp, data.m, 2)) return false;
    tmp.remove_prefix(1); // '-'
    if (!parse_int(tmp, data.d, 2)) return false;
    has_date = true;

    if (tmp.empty()) return true; // date only
    if (tmp[0] == 'T' || tmp[0] == '_') tmp.remove_prefix(1);
    else return false;
  }

  // Time: HH:MM:SS.
  if (tmp.size() >= 8 && tmp[2] == ':' && tmp[5] == ':') {
    if (!parse_int(tmp, data.hh, 2)) return false;
    tmp.remove_prefix(1); // ':'
    if (!parse_int(tmp, data.mm, 2)) return false;
    tmp.remove_prefix(1); // ':'
    if (!parse_int(tmp, data.ss, 2)) return false;
    has_time = true;
  } else if (has_date) return false; // date separator must be followed by time

  // Without a date or time this is not datetime; otherwise "-3.14"/"+5" would look like timezones.
  if (!has_date && !has_time) return false;

  // Fractional seconds.
  if (!tmp.empty() && tmp[0] == '.') {
    tmp.remove_prefix(1);
    size_t i = 0;
    while (i < tmp.size() && tmp[i] >= '0' && tmp[i] <= '9') i++;
    if (i == 0) return false;
    // iso_datetime stores nanoseconds; reject fractions we cannot represent exactly.
    if (i > 9) return false;
    for (size_t j = 0; j < 9; ++j)
      data.ns = data.ns * 10 + (j < i ? tmp[j] - '0' : 0);
    tmp.remove_prefix(i);
  }

  // Timezone: Z or +-HH:MM.
  if (tmp.empty()) return true;
  if (tmp[0] == 'Z') {
      data.is_utc = true;
      data.has_timezone = true;
      return tmp.size() == 1;
  }

  if (tmp[0] == '+' || tmp[0] == '-') {
    data.has_timezone = true;
    int sign = (tmp[0] == '+') ? 1 : -1;
    tmp.remove_prefix(1);
    int tz_h = 0, tz_m = 0;
    if (!parse_int(tmp, tz_h, 2)) return false;
    if (!tmp.empty()) {
      if (tmp[0] == ':') tmp.remove_prefix(1);
      if (!parse_int(tmp, tz_m, 2)) return false;
    }
    data.tz_offset_mm = sign * (tz_h * 60 + tz_m);
  }

  return tmp.empty();
}

// Append a Unicode code point as UTF-8.
static void append_utf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.append(1, static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.append(1, static_cast<char>(0xC0 | (cp >> 6)));
    out.append(1, static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.append(1, static_cast<char>(0xE0 | (cp >> 12)));
    out.append(1, static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.append(1, static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.append(1, static_cast<char>(0xF0 | (cp >> 18)));
    out.append(1, static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.append(1, static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.append(1, static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void unescape_singlequote_string(const std::string_view& input, std::string& result) {
  result.reserve(result.size() + input.size());

  bool has_escape = false;
  for (const char c : input) {
    if (has_escape && c == '\'') { result.append(1, c); has_escape = false; continue; }
    if (has_escape && c == '\\') { result.append(1, c); has_escape = false; continue; }

    if (c == '\\') { has_escape = true; continue; }

    if (has_escape) {
      result.append(1, '\\');
      has_escape = false;
    }

    result.append(1, c);
  }

  if (has_escape) result.append(1, '\\');
}

std::optional<size_t> invalid_doublequote_escape(std::string_view input) {
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  const auto codepoint = [&](size_t start, size_t digits) -> std::optional<uint32_t> {
    if (start > input.size() || digits > input.size() - start) return std::nullopt;
    uint32_t cp = 0;
    for (size_t j = 0; j < digits; ++j) {
      const int digit = hex(input[start + j]);
      if (digit < 0) return std::nullopt;
      cp = (cp << 4) | static_cast<uint32_t>(digit);
    }
    return cp;
  };

  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != '\\') continue;
    const size_t escape = i;
    if (++i == input.size()) return escape;
    switch (input[i]) {
      case 'n': case 'r': case 't': case 'a': case 'b': case 'f': case 'v':
      case '\\': case '"': break;
      case 'u': case 'U': {
        const bool short_form = input[i] == 'u';
        const size_t digits = short_form ? 4 : 8;
        const auto cp = codepoint(i + 1, digits);
        if (!cp || *cp > 0x10FFFF || (*cp >= 0xDC00 && *cp <= 0xDFFF))
          return escape;
        i += digits;
        if (*cp >= 0xD800 && *cp <= 0xDBFF) {
          if (!short_form || i + 6 >= input.size() || input[i + 1] != '\\' ||
              input[i + 2] != 'u') return escape;
          const auto low = codepoint(i + 3, 4);
          if (!low || *low < 0xDC00 || *low > 0xDFFF) return escape;
          i += 6;
        }
        break;
      }
      default: return escape;
    }
  }
  return std::nullopt;
}

void unescape_doublequote_string(const std::string_view& input, std::string& result) {
  uint32_t high_surrogate = 0;

  for (size_t i = 0; i < input.length(); ++i) {
    if (input[i] == '\\' && i + 1 < input.length()) {
      const char type = input[i + 1];
      const int len = (type == 'u') ? 4 : (type == 'U' ? 8 : 0);

      if (len > 0 && i + 1 + len < input.length()) {
        const auto part = input.substr(i + 2, len);
        uint64_t cp = 0;
        if (parse_unsigned_digits(part, 16, UINT32_MAX, cp)) {
          i += (len + 1);

          // UTF-16 surrogate pairs.
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            high_surrogate = cp;
            continue;
          } else if (cp >= 0xDC00 && cp <= 0xDFFF && high_surrogate != 0) {
            cp = 0x10000 + ((high_surrogate - 0xD800) << 10) + (cp - 0xDC00);
            high_surrogate = 0;
          }

          append_utf8(result, cp);
          continue;
        }
      }

      switch (type) {
        case 'n':  result.append(1, '\n'); i += 1; continue; break;
        case 'r':  result.append(1, '\r'); i += 1; continue; break;
        case 't':  result.append(1, '\t'); i += 1; continue; break;
        case '\\': result.append(1, '\\'); i += 1; continue; break;
        case '\"': result.append(1, '\"'); i += 1; continue; break;
        case 'a':  result.append(1, '\a'); i += 1; continue; break;
        case 'b':  result.append(1, '\b'); i += 1; continue; break;
        case 'f':  result.append(1, '\f'); i += 1; continue; break;
        case 'v':  result.append(1, '\v'); i += 1; continue; break;
        //case '\'': result.append(1, '\''); i += 1; continue; break;
      }

    }

    result.append(1, input[i]);
  }
}

// escape_doublequote_minimal_to / escape_doublequote_full_to are templates in detail.h and write
// through a sink without temporary buffers.

std::optional<std::chrono::nanoseconds> iso_datetime_to_unix_ns(const iso_datetime& dt) {
  using namespace std::chrono;

  seconds whole{};
  if (!dt.has_timezone && !dt.is_utc) {
    std::tm local{};
    local.tm_year = (dt.m >= 1 && dt.d >= 1 ? dt.y : 1970) - 1900;
    local.tm_mon = (dt.m >= 1 ? dt.m : 1) - 1;
    local.tm_mday = dt.d >= 1 ? dt.d : 1;
    local.tm_hour = dt.hh;
    local.tm_min = dt.mm;
    local.tm_sec = dt.ss;
    local.tm_isdst = -1;
    whole = seconds{static_cast<int64_t>(std::mktime(&local))};
  } else {
    sys_days date{};   // epoch default for time-only tokens
    if (dt.m >= 1 && dt.d >= 1) {
      const year_month_day ymd{year{dt.y}, month{static_cast<unsigned>(dt.m)}, day{static_cast<unsigned>(dt.d)}};
      date = ymd;      // year_month_day -> sys_days
    }
    whole = duration_cast<seconds>(date.time_since_epoch())
          + hours{dt.hh} + minutes{dt.mm} + seconds{dt.ss}
          - minutes{dt.tz_offset_mm};
  }
  constexpr int64_t billion = 1000000000;
  constexpr int64_t min_seconds = std::numeric_limits<int64_t>::min() / billion;
  constexpr int64_t max_seconds = std::numeric_limits<int64_t>::max() / billion;
  const int64_t s = whole.count();
  if (dt.ns < 0 || dt.ns >= billion || s < min_seconds - 1 || s > max_seconds)
    return std::nullopt;
  if (s == min_seconds - 1) {
    constexpr int64_t min_fraction = billion + std::numeric_limits<int64_t>::min() % billion;
    if (dt.ns < min_fraction) return std::nullopt;
    return nanoseconds{std::numeric_limits<int64_t>::min() + dt.ns - min_fraction};
  }
  const int64_t base = s * billion;
  if (base > std::numeric_limits<int64_t>::max() - dt.ns) return std::nullopt;
  return nanoseconds{base + dt.ns};
}

iso_datetime unix_ns_to_iso_datetime(std::chrono::nanoseconds ns, bool utc) {
  using namespace std::chrono;

  iso_datetime out;
  const auto whole_seconds = floor<seconds>(ns);
  out.ns = static_cast<int32_t>((ns - whole_seconds).count());
  out.is_utc = utc;
  out.has_timezone = utc;
  if (utc) {
    const days dd = floor<days>(ns);
    auto tod = ns - dd;
    const year_month_day ymd{sys_days{dd}};
    out.y = static_cast<int32_t>(int(ymd.year()));
    out.m = static_cast<int32_t>(unsigned(ymd.month()));
    out.d = static_cast<int32_t>(unsigned(ymd.day()));
    const auto h = duration_cast<hours>(tod); tod -= h;
    const auto mi = duration_cast<minutes>(tod); tod -= mi;
    const auto s = duration_cast<seconds>(tod);
    out.hh = static_cast<int32_t>(h.count());
    out.mm = static_cast<int32_t>(mi.count());
    out.ss = static_cast<int32_t>(s.count());
  } else {
    const std::time_t t = static_cast<std::time_t>(whole_seconds.count());
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &t) != 0) return out;
#else
    if (localtime_r(&t, &local) == nullptr) return out;
#endif
    out.y = local.tm_year + 1900;
    out.m = local.tm_mon + 1;
    out.d = local.tm_mday;
    out.hh = local.tm_hour;
    out.mm = local.tm_min;
    out.ss = local.tm_sec;
  }
  return out;
}

size_t format_iso_datetime(const iso_datetime& dt, char* buf, size_t cap, char sep) {
  if (cap == 0) return 0;
  int n = std::snprintf(buf, cap, "%04d-%02u-%02u%c%02d:%02d:%02d",
                        dt.y, static_cast<unsigned>(dt.m), static_cast<unsigned>(dt.d),
                        sep, dt.hh, dt.mm, dt.ss);
  size_t len = (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
  if (dt.ns != 0 && len < cap) {
    const int digits = dt.ns % 1000000 == 0 ? 3 : dt.ns % 1000 == 0 ? 6 : 9;
    const int scaled = digits == 3 ? dt.ns / 1000000 : digits == 6 ? dt.ns / 1000 : dt.ns;
    const int m = std::snprintf(buf + len, cap - len, ".%0*d", digits, scaled);
    if (m > 0 && static_cast<size_t>(m) < cap - len) len += static_cast<size_t>(m);
  }
  if (dt.is_utc && len + 1 < cap) {
    buf[len++] = 'Z';
    buf[len] = '\0';
  } else if (dt.has_timezone && len < cap) {
    const int offset = dt.tz_offset_mm;
    const int abs_offset = offset < 0 ? -offset : offset;
    const int m = std::snprintf(buf + len, cap - len, "%c%02d:%02d",
                                offset < 0 ? '-' : '+', abs_offset / 60, abs_offset % 60);
    if (m > 0 && static_cast<size_t>(m) < cap - len) len += static_cast<size_t>(m);
  }
  return len;
}

}
