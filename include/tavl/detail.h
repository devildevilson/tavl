#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <string>
#include <chrono>
#include <algorithm>

#include "tavl/common.h"

// Низкоуровневые утилиты разбора: классификация символов, предикаты типов,
// обработка escape-последовательностей строк. Вынесены сюда и сделаны публичными,
// чтобы их могли использовать и лексер, и парсер, и внешний код.
namespace tavl::detail {

inline constexpr std::string_view operator_chars = "!@#$%^&*-+=<>/?|~";
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
bool is_float_number(const std::string_view& str, double& val);

bool parse_int(std::string_view& s, int32_t& out, const size_t n);
bool is_datetime(const std::string_view& str, iso_datetime& data);

// обработка содержимого строкового литерала (без кавычек) в готовую строку:
// одинарные кавычки - почти без escape, двойные - стандартные escape + \u\U unicode
void unescape_singlequote_string(const std::string_view& input, std::string& result);
void unescape_doublequote_string(const std::string_view& input, std::string& result);

// escape для сериализации, БЕЗ временных буферов: каждый выходной байт отдаётся в put(char)->bool.
// put возвращает false, чтобы остановить (напр. упор в capacity); тогда escape_*_to вернёт false.
// Так serialize пишет сразу в out через bounded-aware ser_put, не создавая промежуточных строк.

// минимальный: экранирует ТОЛЬКО \ и " (строки многострочны, управляющие пишем как есть).
template <typename Put>
bool escape_doublequote_minimal_to(std::string_view input, Put&& put) {
  for (const char c : input) {
    if ((c == '\\' || c == '\"') && !put('\\')) return false;
    if (!put(c)) return false;
  }
  return true;
}

// полный: \ " + управляющие (\n \r \t \a \b \f \v, прочие <0x20 -> \u00XX) + ЮНИКОД (байты >=0x80
// декодятся из UTF-8 в \uXXXX / \UXXXXXXXX). Чистый ASCII; читается обратно unescape_doublequote_string.
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

    uint32_t cp = 0;                                  // multibyte UTF-8 -> кодовая точка
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
    if (!ok) { if (!emit_u(c)) return false; i += 1; continue; }  // битый ведущий/оборванный
    if (!emit_u(cp)) return false;
    i += static_cast<size_t>(len);
  }
  return true;
}

// конвертация iso_datetime <-> Unix-время в миллисекундах (UTC; учитывает tz_offset). Календарь -
// через std::chrono (C++20). Для chrono-десериализации/сериализации (см. deserialize.h/serialize.h).
std::chrono::milliseconds iso_datetime_to_unix_ms(const iso_datetime& dt);
iso_datetime unix_ms_to_iso_datetime(std::chrono::milliseconds ms);
// рендер iso_datetime в токен YYYY-MM-DD<sep>HH:MM:SS[.mmm] (UTC, без tz-суффикса) в буфер buf (cap байт),
// возвращает длину. Датавремя ограниченной длины -> хватает char buf[40] (без std::string).
// sep - разделитель даты и времени: '_' (дефолт формата) или 'T' (строгий ISO).
size_t format_iso_datetime(const iso_datetime& dt, char* buf, size_t cap, char sep = '_');

}
