#pragma once

#include <cstddef>
#include <cstdint>
#include <charconv>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <tuple>
#include <array>
#include <chrono>
#include <type_traits>

#include <reflect>

#include "tavl/common.h"
#include "tavl/detail.h"
#include "tavl/type_traits.h"

// --- serialize: textual representation of a C++ struct in the tavl format ---
// The mirror of deserialize: the same if constexpr dispatcher and the same ds_* traits (type_traits.h),
// but instead of reading events we write a string. Type-to-bracket mapping (as deserialize reads it):
//   named aggregate -> { field = val }   unnamed aggregate (past the depth threshold) -> ( val, val )
//   pair/tuple -> ( … )    vector/list/deque/std::array/set -> [ … ]    map -> { k = v }
//   root aggregate -> bare rows without brackets (the file's implicit global tuple).
// Primitives: bool->true/false; integral/floating -> std::vformat using the options' format string
//   (empty -> to_chars, shortest round-trip); std::string/char[N]/array<char,N>/const char* ->
//   identifier without quotes if safe, otherwise "…" with escaping; optional/unique_ptr -> null or the value.
//
// Options - the sopts struct, passed WHOLE as a non-type template parameter
//   (template <auto Opts = sopts{}>), i.e. a compile-time constant. Fields: int_fmt/float_fmt (std::vformat
//   format strings), serialize_cstr/follow_pointers/prettify/quote_all_strings/indent/names_depth/
//   wrap_at. The string fields have type ser_str (a structural fixed-capacity buffer), since std::string/
//   string_view can't be put in an NTTP. follow_pointers is compile-time ON PURPOSE: when false the *val
//   branch isn't instantiated (otherwise void*/function pointers wouldn't compile). Call: tavl::serialize<sopts{ .prettify = false }>(val, out).
//
// Extending with a custom type T: the recursion calls `serialize<Opts>(child, out, depth)` (with an explicit
//   NTTP Opts), so the overload must be a TEMPLATE over auto Opts and live in T's namespace (found
//   by ADL; more specialized than the primary -> wins partial ordering):
//     template <auto Opts> bool serialize(const T& v, std::string& out, size_t depth) { … }
//   Returns true on a complete write (false - bounded mode hit out.capacity()); the easiest is to
//   return the result of `tavl::ser_string<Opts>(...)` / `ser_put<Opts>(...)` (they respect bounded and escaping).
//   depth may be ignored. Example - enum color in main.cpp.
//   Usage: std::string out; tavl::serialize(value, out);

namespace tavl {

// Structural string type for NTTP options: public fixed-capacity buffer + length. std::string_view
// and std::string are not structural types, so option strings live here.
template <size_t Cap = 32>
struct ser_str {
  char data[Cap]{};
  size_t size = 0;
  constexpr ser_str() = default;
  constexpr ser_str(std::string_view s) { size = s.size() < Cap ? s.size() : Cap; for (size_t i = 0; i < size; ++i) data[i] = s[i]; }
  template <size_t N> constexpr ser_str(const char (&s)[N]) : ser_str(std::string_view(s, N ? N - 1 : 0)) {}
  constexpr std::string_view view() const { return std::string_view(data, size); }
  constexpr bool empty() const { return size == 0; }
};

struct sopts {
  ser_str<> int_fmt   = "{}";       // integer format (std::format spec); empty -> to_chars
  ser_str<> float_fmt = "{}";       // float format; empty -> to_chars (shortest round-trip)
  bool serialize_cstr   = false;   // serialize std::string_view / const char* / char* (asymmetric)
  bool follow_pointers  = false;   // dereference raw pointers (ASYMMETRIC - not read back)

  bool prettify = true;             // line breaks + indentation; false - compact, on a single line
  bool quote_all_strings = false;   // always wrap strings in "" with minimal escaping (otherwise - only when needed)
  bool full_escape = false;         // ALL strings -> "" with full escape (control chars + unicode \u/\U): pure ASCII for network transport
  bool iso_datetime = false;        // datetime: date/time separator - false -> '_' (format default), true -> 'T' (strict ISO)

  ser_str<> indent = "  ";          // indent for one nesting level (only when prettify)
  size_t names_depth = SIZE_MAX;    // STRICTLY up to which depth (number of brackets) to write field names; 0 -> nowhere (not even at root), SIZE_MAX -> everywhere
  size_t wrap_at = 0;               // >0: in sequences ([]/set) wrap every wrap_at elements onto a new line (prettify)
  bool bounded = false;             // capacity-aware: do not grow past out.capacity(), stop at the boundary (serialize -> false)
};

// serialize returns true when the value was written completely. In bounded mode it returns false
// if out.capacity() would be exceeded. SIZE_MAX depth marks the root call, whose aggregate is
// rendered as bare rows.
template <auto Opts = sopts{}, typename T>
bool serialize(const T& val, std::string& out, size_t depth = SIZE_MAX);  // forward

// ---------------- helpers ----------------

template <class> inline constexpr bool ser_always_false = false;

// Whether a string must be quoted; implemented in detail.cpp with the token predicates.
bool ser_string_needs_quote(std::string_view s);

// Single write point. With Opts.bounded, never grow past capacity: if the append would not fit,
// write nothing and return false. Without bounded mode, always write.
template <auto Opts>
bool ser_put(std::string& out, std::string_view s) {
  if constexpr (Opts.bounded) { if (out.size() + s.size() > out.capacity()) return false; }
  out.append(s);
  return true;
}
template <auto Opts>
bool ser_put(std::string& out, char c) {
  if constexpr (Opts.bounded) { if (out.size() + 1 > out.capacity()) return false; }
  out.push_back(c);
  return true;
}

template <auto Opts>
bool ser_indent(std::string& out, size_t level) {
  if constexpr (!Opts.prettify) return true;
  else { for (size_t i = 0; i < level; ++i) if (!ser_put<Opts>(out, Opts.indent.view())) return false; return true; }
}

template <auto Opts, class I> bool ser_int(I v, std::string& out) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return ser_put<Opts>(out, std::string_view(buf, r.ptr));
}

template <auto Opts> bool ser_float(double v, std::string& out) {
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return ser_put<Opts>(out, std::string_view(buf, r.ptr));
}

template <auto Opts>
bool ser_string(std::string_view s, std::string& out) {
  if (!Opts.full_escape && !Opts.quote_all_strings && !ser_string_needs_quote(s))
    return ser_put<Opts>(out, s);   // unquoted valid identifier

  // Escape directly into out through bounded-aware ser_put; no temporary string.
  const auto put = [&out](char c) { return ser_put<Opts>(out, c); };
  if (!ser_put<Opts>(out, '\"')) return false;
  bool ok;
  if constexpr (Opts.full_escape) ok = detail::escape_doublequote_full_to(s, put);
  else                            ok = detail::escape_doublequote_minimal_to(s, put);
  if (!ok) return false;
  return ser_put<Opts>(out, '\"');
}

enum class ser_kind {
  named,   // struct/map: field rows when prettified
  group,   // tuple/pair/positional aggregate
  seq,     // []/set, optionally wrapped by wrap_at
};

// Write directly into out without temporary render buffers; ser_multiline decides layout up front.
template <auto Opts>
bool ser_open(std::string& out, char open, bool multiline) {
  if (!ser_put<Opts>(out, open)) return false;
  if (multiline) return ser_put<Opts>(out, '\n');
  return true;
}

template <auto Opts>
bool ser_sep(std::string& out, size_t depth, ser_kind kind, bool multiline, size_t i) {
  if (!multiline) { if (i) return ser_put<Opts>(out, Opts.prettify ? std::string_view(", ") : std::string_view(",")); return true; }
  const size_t per_line = (kind == ser_kind::seq && Opts.wrap_at > 0) ? Opts.wrap_at : 1;
  if (i % per_line == 0) { if (i && !ser_put<Opts>(out, '\n')) return false; return ser_indent<Opts>(out, depth + 1); }
  return ser_put<Opts>(out, ", ");
}

template <auto Opts>
bool ser_close(std::string& out, size_t depth, char close, bool multiline) {
  if (multiline) { if (!ser_put<Opts>(out, '\n') || !ser_indent<Opts>(out, depth)) return false; }
  return ser_put<Opts>(out, close);
}

// Structural multiline predicate, no rendering. Branch order mirrors serialize(). A literal newline
// inside a string does not force multiline layout; newlines inside quoted strings are valid.
template <auto Opts, typename T>
bool ser_multiline(const T& val, size_t depth) {
  if constexpr (!Opts.prettify) return false;
  else {
    const bool root = (depth == SIZE_MAX);
    const size_t d = root ? 0 : depth;
    if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) return false;
    else if constexpr (ds_is_optional<T>::value)   return val.has_value() && ser_multiline<Opts>(*val, depth);
    else if constexpr (ds_is_unique_ptr<T>::value) return bool(val) && ser_multiline<Opts>(*val, depth);
    else if constexpr (ds_is_pair<T>::value)
      return ser_multiline<Opts>(val.first, d + 1) || ser_multiline<Opts>(val.second, d + 1);
    else if constexpr (ds_is_tuple<T>::value) {
      bool m = false;
      std::apply([&](const auto&... xs) { ((m = m || ser_multiline<Opts>(xs, d + 1)), ...); }, val);
      return m;
    }
    else if constexpr (ds_is_char_array<T>::value) return false;
    else if constexpr (ds_is_std_array<T>::value || ds_has_push_back<T>::value || ds_has_key_type<T>::value) {
      if constexpr (ds_is_map<T>::value) return !val.empty();      // map -> named
      else {                                                       // seq ([]/set)
        if (val.empty()) return false;
        if (Opts.wrap_at > 0 && val.size() > Opts.wrap_at) return true;
        for (const auto& x : val) if (ser_multiline<Opts>(x, d + 1)) return true;
        return false;
      }
    }
    else if constexpr (std::is_aggregate_v<T>) {
      size_t fc = 0; reflect::for_each([&](auto) { ++fc; }, val);
      if (fc == 0) return false;
      const size_t field_level = root ? 0 : d + 1;
      if (field_level < Opts.names_depth) return true;             // named non-empty
      bool m = false;                                              // group layout follows children
      reflect::for_each([&](auto I) { m = m || ser_multiline<Opts>(reflect::get<I>(val), root ? 0 : d + 1); }, val);
      return m;
    }
    else return false;
  }
}

// ---------------- main dispatcher; branch order mirrors deserialize ----------------

template <auto Opts, typename T>
bool serialize(const T& val, std::string& out, size_t depth) {
  [[maybe_unused]] const bool root = (depth == SIZE_MAX);
  [[maybe_unused]] const size_t d = root ? 0 : depth;
  [[maybe_unused]] const std::string_view eq = Opts.prettify ? " = " : "=";

  if constexpr (std::is_same_v<T, bool>) {
    return ser_put<Opts>(out, val ? std::string_view("true") : std::string_view("false"));
  }
  else if constexpr (std::is_integral_v<T>) {                 // char is rendered as a number, mirroring to_int
    using FT = std::conditional_t<(sizeof(T) == 1),
                 std::conditional_t<std::is_signed_v<T>, int, unsigned int>, T>;
    FT fv = static_cast<FT>(val);
    if constexpr (Opts.int_fmt.empty()) return ser_int<Opts>(fv, out);
    else return ser_put<Opts>(out, std::format(Opts.int_fmt.view(), fv));   // Opts is an NTTP, so format is compile-time data
  }
  else if constexpr (std::is_floating_point_v<T>) {
    double fv = static_cast<double>(val);
    if constexpr (Opts.float_fmt.empty()) return ser_float<Opts>(fv, out);
    else return ser_put<Opts>(out, std::format(Opts.float_fmt.view(), fv));
  }
  else if constexpr (ds_is_time_point<T>::value || ds_is_duration<T>::value) {
    using namespace std::chrono;
    milliseconds ms{};
    if constexpr (ds_is_time_point<T>::value) ms = duration_cast<milliseconds>(val.time_since_epoch());
    else                                      ms = duration_cast<milliseconds>(val);
    char buf[40];   // datetime output is bounded; avoid std::string
    const size_t n = detail::format_iso_datetime(detail::unix_ms_to_iso_datetime(ms), buf, sizeof(buf), Opts.iso_datetime ? 'T' : '_');
    return ser_put<Opts>(out, std::string_view(buf, n));
  }
  else if constexpr (std::is_same_v<T, std::string>) {
    return ser_string<Opts>(val, out);
  }
  else if constexpr (std::is_same_v<T, std::string_view>) {   // gated by serialize_cstr (asymmetric: deserialize won't read it back)
    if constexpr (Opts.serialize_cstr) return ser_string<Opts>(val, out);
    else static_assert(ser_always_false<T>, "tavl::serialize: std::string_view disabled (asymmetric) - set serialize_cstr=true to allow");
  }
  else if constexpr (ds_is_char_array<T>::value) {            // char[N] / std::array<char,N>, up to '\0'
    const char* data; size_t n;
    if constexpr (std::is_array_v<T>) { data = val; n = std::extent_v<T>; }
    else                              { data = val.data(); n = val.size(); }
    size_t len = 0; while (len < n && data[len] != '\0') ++len;
    return ser_string<Opts>(std::string_view(data, len), out);
  }
  else if constexpr (std::is_pointer_v<T> &&
                     std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>, char>) {
    if constexpr (Opts.serialize_cstr) {
      return val ? ser_string<Opts>(std::string_view(val), out) : ser_put<Opts>(out, "null");
    } else {
      static_assert(ser_always_false<T>, "tavl::serialize: const char* disabled - set serialize_cstr=true on options");
    }
  }
  else if constexpr (std::is_pointer_v<T>) {                  // gated by follow_pointers (asymmetric)
    if constexpr (Opts.follow_pointers) {
      return val ? serialize<Opts>(*val, out, depth) : ser_put<Opts>(out, "null");
    } else {
      static_assert(ser_always_false<T>, "tavl::serialize: raw pointer disabled (asymmetric) - set follow_pointers=true or provide an overload");
    }
  }
  else if constexpr (ds_is_optional<T>::value) {
    return val.has_value() ? serialize<Opts>(*val, out, depth) : ser_put<Opts>(out, "null");   // transparent wrapper: same depth
  }
  else if constexpr (ds_is_unique_ptr<T>::value) {
    return val ? serialize<Opts>(*val, out, depth) : ser_put<Opts>(out, "null");
  }
  else if constexpr (ds_is_pair<T>::value) {
    const bool ml = ser_multiline<Opts>(val, d);
    return ser_open<Opts>(out, '(', ml)
        && ser_sep<Opts>(out, d, ser_kind::group, ml, 0) && serialize<Opts>(val.first,  out, d + 1)
        && ser_sep<Opts>(out, d, ser_kind::group, ml, 1) && serialize<Opts>(val.second, out, d + 1)
        && ser_close<Opts>(out, d, ')', ml);
  }
  else if constexpr (ds_is_tuple<T>::value) {
    const bool ml = ser_multiline<Opts>(val, d);
    bool ok = ser_open<Opts>(out, '(', ml);
    size_t i = 0;
    std::apply([&](const auto&... xs) {
      ((ok = ok && ser_sep<Opts>(out, d, ser_kind::group, ml, i) && serialize<Opts>(xs, out, d + 1), ++i), ...);
    }, val);
    return ok && ser_close<Opts>(out, d, ')', ml);
  }
  else if constexpr (ds_is_std_array<T>::value) {             // before is_aggregate: std::array is also an aggregate
    const bool ml = ser_multiline<Opts>(val, d);
    bool ok = ser_open<Opts>(out, '[', ml);
    size_t i = 0;
    for (const auto& x : val) { if (!ok) break; ok = ser_sep<Opts>(out, d, ser_kind::seq, ml, i) && serialize<Opts>(x, out, d + 1); ++i; }
    return ok && ser_close<Opts>(out, d, ']', ml);
  }
  else if constexpr (ds_has_push_back<T>::value) {            // vector / list / deque
    const bool ml = ser_multiline<Opts>(val, d);
    bool ok = ser_open<Opts>(out, '[', ml);
    size_t i = 0;
    for (const auto& x : val) { if (!ok) break; ok = ser_sep<Opts>(out, d, ser_kind::seq, ml, i) && serialize<Opts>(x, out, d + 1); ++i; }
    return ok && ser_close<Opts>(out, d, ']', ml);
  }
  else if constexpr (ds_is_map<T>::value) {                   // key = value entries
    const bool ml = ser_multiline<Opts>(val, d);
    bool ok = ser_open<Opts>(out, '{', ml);
    size_t i = 0;
    for (const auto& kv : val) {
      if (!ok) break;
      ok = ser_sep<Opts>(out, d, ser_kind::named, ml, i)
        && serialize<Opts>(kv.first, out, d + 1) && ser_put<Opts>(out, eq) && serialize<Opts>(kv.second, out, d + 1);
      ++i;
    }
    return ok && ser_close<Opts>(out, d, '}', ml);
  }
  else if constexpr (ds_has_key_type<T>::value) {             // set / multiset; map handled above
    const bool ml = ser_multiline<Opts>(val, d);
    bool ok = ser_open<Opts>(out, '[', ml);
    size_t i = 0;
    for (const auto& x : val) { if (!ok) break; ok = ser_sep<Opts>(out, d, ser_kind::seq, ml, i) && serialize<Opts>(x, out, d + 1); ++i; }
    return ok && ser_close<Opts>(out, d, ']', ml);
  }
  else if constexpr (std::is_aggregate_v<T>) {
    const size_t field_level = root ? 0 : d + 1;
    const bool names = field_level < Opts.names_depth;        // strict: names_depth=0 disables names even at root
    const size_t cd = root ? 0 : d + 1;
    bool ok = true;

    if (root) {                                               // root: bare rows, always separated by '\n'
      size_t i = 0;
      reflect::for_each([&](auto I) {
        if (!ok) return;
        if (i) ok = ser_put<Opts>(out, '\n');
        if (ok && names) ok = ser_put<Opts>(out, reflect::member_name<I>(val)) && ser_put<Opts>(out, eq);
        if (ok) ok = serialize<Opts>(reflect::get<I>(val), out, cd);
        ++i;
      }, val);
      return ok;
    } else {
      const bool ml = ser_multiline<Opts>(val, d);
      const char open  = names ? '{' : '(';
      const char close = names ? '}' : ')';
      const ser_kind kind = names ? ser_kind::named : ser_kind::group;
      ok = ser_open<Opts>(out, open, ml);
      size_t i = 0;
      reflect::for_each([&](auto I) {
        if (!ok) return;
        ok = ser_sep<Opts>(out, d, kind, ml, i);
        if (ok && names) ok = ser_put<Opts>(out, reflect::member_name<I>(val)) && ser_put<Opts>(out, eq);
        if (ok) ok = serialize<Opts>(reflect::get<I>(val), out, cd);
        ++i;
      }, val);
      return ok && ser_close<Opts>(out, d, close, ml);
    }
  }
  else {
    static_assert(ser_always_false<T>, "tavl::serialize: unsupported type - provide a serialize() overload for T");
  }
}

}
