#include "tavl/detail.h"

#include <charconv>
#include <limits>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

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

// единственная нешаблонная функция сериализатора (объявлена в serialize.h); живёт здесь, рядом
// с detail-предикатами, которые использует
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

bool is_boolean(const std::string_view &str, bool& out) {
  const bool is_true  = str == "true" || str == "TRUE";
  const bool is_false = str == "false" || str == "FALSE";
  out = is_true || is_false ? is_true : out;
  return is_true || is_false;
}

bool is_null(const std::string_view &str) {
  return str == "null" || str == "NULL";
}

bool is_bin_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0b" && part1 != "0B") return false;
  const auto part2 = str.substr(2);
  const auto [ptr, ec] = std::from_chars(part2.data(), part2.data() + part2.size(), out, 2);
  return ec == std::errc() && ptr == (part2.data() + part2.size());
}

bool is_oct_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0o" && part1 != "0O") return false;
  const auto part2 = str.substr(2);
  const auto [ptr, ec] = std::from_chars(part2.data(), part2.data() + part2.size(), out, 8);
  return ec == std::errc() && ptr == (part2.data() + part2.size());
}

bool is_hex_number(const std::string_view &str, uint64_t& out) {
  const auto part1 = str.substr(0, 2);
  if (part1 != "0x" && part1 != "0X") return false;
  const auto part2 = str.substr(2);
  const auto [ptr, ec] = std::from_chars(part2.data(), part2.data() + part2.size(), out, 16);
  return ec == std::errc() && ptr == (part2.data() + part2.size());
}

bool is_dec_number(const std::string_view &str, int64_t& out) {
  auto tmp = str;
  int64_t sign = 1;
  if (tmp[0] == '-' || tmp[0] == '+') {
    if (tmp[0] == '-') sign = -1;
    tmp = tmp.substr(1);
  }

  const auto [ptr, ec] = std::from_chars(tmp.data(), tmp.data() + tmp.size(), out);
  out = sign * out;
  return ec == std::errc() && ptr == (tmp.data() + tmp.size());
}

static constexpr size_t helper_buffer_size = 1024;

bool is_float_number(const std::string_view &str, double &val) {
  if (str.empty()) return false;

  auto tmp = str;
  double sign = 1.0;
  if (tmp[0] == '-' || tmp[0] == '+') {
    if (tmp[0] == '-') sign = -1.0;
    tmp.remove_prefix(1);
  }

  if (tmp.empty()) return false;

  if (tmp == "nan" || tmp == "NAN" || tmp == "NaN") {
    val = sign * std::numeric_limits<double>::quiet_NaN();
    return true;
  }

  if (tmp == "inf" || tmp == "INF") {
    val = sign * std::numeric_limits<double>::infinity();
    return true;
  }

  if (tmp.back() == 'f' || tmp.back() == 'F' || tmp.back() == 'd' || tmp.back() == 'D') {
    tmp.remove_suffix(1);
    if (tmp.empty()) return false;   // был один суффикс ("d"/"f"/...) - это идентификатор, не число
  }

  while (!tmp.empty() && tmp[0] == '0') tmp.remove_prefix(1);
  if (tmp.empty()) return false;     // одни нули - это целое (ловится is_dec_number раньше), не float

  if (tmp.size() >= helper_buffer_size-1) return false;

  if (tmp[0] == '.') {
    char buffer[helper_buffer_size];

    const size_t size = std::min(tmp.size(), helper_buffer_size-1);
    buffer[0] = '0';
    std::copy(tmp.begin(), tmp.end(), &buffer[1]);

    const auto [ptr, ec] = std::from_chars(buffer, buffer+size+1, val);
    val = sign * val;
    return ec == std::errc() && ptr == (buffer + size + 1);
  }

  auto [ptr, ec] = std::from_chars(tmp.data(), tmp.data() + tmp.size(), val);
  val = sign * val;
  return ec == std::errc() && ptr == (tmp.data() + tmp.size());
}

bool parse_int(std::string_view& s, int32_t& out, const size_t n) {
  if (s.size() < n) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + n, out);
  if (ec != std::errc{}) return false;
  s.remove_prefix(n);
  return true;
}

bool is_datetime(const std::string_view &str, iso_datetime& data) {
  if (str.empty()) return false;

  auto tmp = str;

  bool has_date = false;
  bool has_time = false;

  // дата (YYYY-MM-DD)
  if (tmp.size() >= 10 && tmp[4] == '-' && tmp[7] == '-') {
    if (!parse_int(tmp, data.y, 4)) return false;
    tmp.remove_prefix(1); // '-'
    if (!parse_int(tmp, data.m, 2)) return false;
    tmp.remove_prefix(1); // '-'
    if (!parse_int(tmp, data.d, 2)) return false;
    has_date = true;

    if (tmp.empty()) return true; // Только дата
    if (tmp[0] == 'T' || tmp[0] == '_') tmp.remove_prefix(1);
    else return false;
  }

  // время (HH:MM:SS)
  if (tmp.size() >= 8 && tmp[2] == ':' && tmp[5] == ':') {
    if (!parse_int(tmp, data.hh, 2)) return false;
    tmp.remove_prefix(1); // ':'
    if (!parse_int(tmp, data.mm, 2)) return false;
    tmp.remove_prefix(1); // ':'
    if (!parse_int(tmp, data.ss, 2)) return false;
    has_time = true;
  } else if (has_date) return false; // Ошибка, если после T нет времени

  // ни даты, ни времени - не datetime (иначе "-3.14"/"+5" уехали бы в ветку часового пояса)
  if (!has_date && !has_time) return false;

  // миллисекунды (.XXXX)
  if (!tmp.empty() && tmp[0] == '.') {
    tmp.remove_prefix(1);
    size_t i = 0;
    while (i < tmp.size() && std::isdigit(tmp[i])) i++;
    if (i == 0) return false;
    std::from_chars(tmp.data(), tmp.data() + i, data.ms); // Читаем сколько есть
    tmp.remove_prefix(i);
  }

  // часовой пояс (Z или +-HH:MM)
  if (tmp.empty()) return true;
  if (tmp[0] == 'Z') {
      data.is_utc = true;
      return tmp.size() == 1;
  }

  if (tmp[0] == '+' || tmp[0] == '-') {
    int sign = (tmp[0] == '+') ? 1 : -1;
    tmp.remove_prefix(1);
    int tz_h = 0, tz_m = 0;
    if (!parse_int(tmp, tz_h, 2)) return false;
    if (!tmp.empty() && tmp[0] == ':') tmp.remove_prefix(1);
    if (!tmp.empty() && std::isdigit(tmp[0])) parse_int(tmp, tz_m, 2);
    data.tz_offset_mm = sign * (tz_h * 60 + tz_m);
  }

  return tmp.empty();
}

// запись кодовой точки в UTF-8
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

void unescape_doublequote_string(const std::string_view& input, std::string& result) {
  uint32_t high_surrogate = 0;

  for (size_t i = 0; i < input.length(); ++i) {
    if (input[i] == '\\' && i + 1 < input.length()) {
      const char type = input[i + 1];
      const int len = (type == 'u') ? 4 : (type == 'U' ? 8 : 0);

      if (len > 0 && i + 1 + len < input.length()) {
        const auto part = input.substr(i + 2, len);
        uint64_t cp = 0;
        const auto [ptr, ec] = std::from_chars(part.data(), part.data()+part.size(), cp, 16);
        if (ec == std::errc() && ptr == part.data()+part.size()) {
          i += (len + 1);

          // обработка суррогатных пар (UTF-16)
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

      // числа?
    }

    result.append(1, input[i]);
  }
}

// escape_doublequote_minimal_to / escape_doublequote_full_to - теперь шаблоны в detail.h
// (пишут через sink без промежуточных буферов).

std::chrono::milliseconds iso_datetime_to_unix_ms(const iso_datetime& dt) {
  using namespace std::chrono;

  sys_days date{};   // эпоха по умолчанию, если в токене не было даты (только время)
  if (dt.m >= 1 && dt.d >= 1) {
    const year_month_day ymd{year{dt.y}, month{static_cast<unsigned>(dt.m)}, day{static_cast<unsigned>(dt.d)}};
    date = ymd;      // year_month_day -> sys_days (неявная конверсия)
  }

  // местное время -> UTC: вычитаем смещение пояса (tz_offset_mm в минутах)
  return duration_cast<milliseconds>(date.time_since_epoch())
       + hours{dt.hh} + minutes{dt.mm} + seconds{dt.ss} + milliseconds{dt.ms}
       - minutes{dt.tz_offset_mm};
}

iso_datetime unix_ms_to_iso_datetime(std::chrono::milliseconds ms) {
  using namespace std::chrono;

  const days dd = floor<days>(ms);
  milliseconds tod = ms - dd;                 // время суток в [0, сутки)
  const year_month_day ymd{sys_days{dd}};

  iso_datetime out;
  out.y = static_cast<int32_t>(int(ymd.year()));
  out.m = static_cast<int32_t>(unsigned(ymd.month()));
  out.d = static_cast<int32_t>(unsigned(ymd.day()));

  const auto h  = duration_cast<hours>(tod);   tod -= h;
  const auto mi = duration_cast<minutes>(tod); tod -= mi;
  const auto s  = duration_cast<seconds>(tod); tod -= s;
  out.hh = static_cast<int32_t>(h.count());
  out.mm = static_cast<int32_t>(mi.count());
  out.ss = static_cast<int32_t>(s.count());
  out.ms = static_cast<int32_t>(tod.count());
  out.is_utc = true;
  return out;
}

size_t format_iso_datetime(const iso_datetime& dt, char* buf, size_t cap, char sep) {
  if (cap == 0) return 0;
  int n = std::snprintf(buf, cap, "%04d-%02u-%02u%c%02d:%02d:%02d",
                        dt.y, static_cast<unsigned>(dt.m), static_cast<unsigned>(dt.d),
                        sep, dt.hh, dt.mm, dt.ss);
  size_t len = (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
  if (dt.ms != 0 && len < cap) {
    const int m = std::snprintf(buf + len, cap - len, ".%03d", dt.ms);
    if (m > 0 && static_cast<size_t>(m) < cap - len) len += static_cast<size_t>(m);
  }
  return len;
}

}
