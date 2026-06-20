#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <utility>
#include <tuple>
#include <array>
#include <deque>
#include <any>
#include <bitset>
#include <chrono>
#include <type_traits>

#include <reflect>

#include "tavl/common.h"
#include "tavl/detail.h"
#include "tavl/parser.h"
#include "tavl/type_traits.h"

// --- deserialize: filling a C++ struct from the parser's event stream ---
// Recursive descent. The field name = the row identifier, the '=' operator is ignored. Supported:
// primitives (bool / integral with mutual casting / float) / std::string / char[N] and std::array<char,N> /
// optional / unique_ptr / pair / tuple / std::array / vector-list-deque / map / set / aggregates.
//
// RESUME: on not_enought_data we set ctx.stalled and unwind outward (after each ct_peek/ct_next and
// nested deserialize - `if (ctx.stalled) return;`). The caller tops up the data and calls again - we resume
// along the frame stack ctx.frames (std::deque - references to a frame and its carry stay stable across a
// recursive push). The partial value lives AT ITS DESTINATION (field/slot/array[i]/back()), a map key /
// set element - in frame::carry (std::any), the depth of an unfinished skip - in frame::skip_depth.
// Containers are NOT held locally - everything lives in ctx/the frame.
//   ct_context ctx; do { p.flush(chunk); deserialize(p, ctx, val); } while (ctx.stalled);
// Parser and deserialization errors (missing/duplicate fields, overflow, too-long string) - in ctx.diagnostics.
//
// Extending with a custom type T: the recursion calls deserialize(p, ctx, child) UNqualified,
// so the overload is NON-template, in T's namespace (found by ADL; an exact match beats the template):
//     void deserialize(tavl::parser& p, tavl::ct_context& ctx, T& val) { ... }
// Inside, read via ct_peek/ct_next and honor the resume contract (after each one - if (ctx.stalled) return;).

namespace tavl {

struct ct_context {
  struct frame {
    event_type term = event_type::eof;  // level terminator: block-end, row_end, or eof
    size_t pos = 0;                     // current sub-element index, or number of completed slots
    size_t next = 0;                    // positional counter / array index
    bool in_element = false;            // a sub-element has started but not finished
    bool positional = false;            // current aggregate sub-element came from positional input
    bool skipping = false;              // resumable ds_skip_value is in progress
    size_t skip_depth = 0;              // bracket depth for an unfinished skip
    std::any carry;                     // partial map key / set element across resume
    size_t field_count = 0;             // aggregate field count; >max_fields is a compile error
    std::bitset<limits::max_fields> seen;// filled aggregate fields, fixed-size instead of vector
  };

  struct diagnostic { struct error error; std::string_view field; };

  std::optional<event> peeked;          // one-event lookahead; not_enought_data is never cached
  std::deque<frame> frames;             // level stack used for resume
  size_t cursor = 0;                    // re-entry cursor through frames, reset by the outer scope
  bool stalled = false;                 // not_enought_data was hit; unwind outward
  std::vector<diagnostic> diagnostics;
  std::string strbuf;                   // reusable string buffer for char arrays
  size_t depth = 0;                     // recursion depth, used to detect the outer call

  // Diagnostics are capped to keep memory bounded.
  void push_diagnostic(struct error e, std::string_view field = {}) {
    if (diagnostics.size() < limits::max_diagnostics) diagnostics.push_back({e, field});
  }
};

// RAII outer scope: resets cursor/stalled on entry; for a fresh session also clears lookahead and diagnostics.
struct ct_scope {
  ct_context& ctx;
  explicit ct_scope(ct_context& c) noexcept;
  ~ct_scope();
};

template <typename T> void deserialize(parser& p, ct_context& ctx, T& val);   // forward

// --- event stream helpers (definitions in deserialize.cpp) ---
// Lookahead wrappers. not_enought_data is not cached (it must be re-polled after flush) and sets
// ctx.stalled. Parser errors attached to events are collected once in ctx.diagnostics.
event ct_peek(parser& p, ct_context& ctx);
event ct_next(parser& p, ct_context& ctx);

bool ds_is_block_begin(event_type t);
bool ds_is_block_end(event_type t);
event_type ds_block_end(event_type begin);

// Enter a loop level: resume adopts an existing frame; fresh input detects term and pushes a frame.
// SIZE_MAX with ctx.stalled means the caller should return.
size_t ds_enter(parser& p, ct_context& ctx, bool& resuming, event_type default_term);

// Skip one value, including nested brackets, up to but not consuming row_end. depth is in/out and
// stored in a frame so the skip can resume in the middle of a nested block.
void ds_skip_value(parser& p, ct_context& ctx, size_t& depth);

// Frame-backed resumable value skip. true means the caller should return.
bool ds_skip_framed(parser& p, ct_context& ctx, ct_context::frame& fr);

// Finish a group through its terminator. Block ends are consumed; row_end is left to the parent.
void ds_finish_group(parser& p, ct_context& ctx, event_type term);

// Read one pair/tuple slot: skip row delimiters and operator separators, then read the value.
template <typename E>
void ds_read_slot(parser& p, ct_context& ctx, event_type term, E& slot) {
  while (true) {
    const event ev = ct_peek(p, ctx);
    if (ctx.stalled) return;
    if (ev.type == event_type::eof || ev.type == term) return;
    if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
        ev.type == event_type::empty_row || ev.type == event_type::got_comment ||
        ev.type == event_type::got_row_operator ||
        (ev.type == event_type::got_token && ev.token.type == token_type::op)) {
      ct_next(p, ctx);
      continue;
    }
    break;
  }
  deserialize(p, ctx, slot);
}

template <typename T>
void deserialize(parser& p, ct_context& ctx, T& val) {
  ct_scope _scope(ctx);

  if constexpr (std::is_same_v<T, bool>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if (const auto v = p.to_boolean(ev.token)) val = *v;
      else ctx.push_diagnostic(error{error_type::err_expect_token_boolean, ev.token.span});
    }
  }
  else if constexpr (std::is_integral_v<T>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if constexpr (std::is_signed_v<T>) {
        if (const auto v = p.to_int(ev.token)) val = static_cast<T>(*v);   // to_int accepts int and uint hex/oct/bin
        else ctx.push_diagnostic(error{error_type::err_expect_token_number_int, ev.token.span});
      } else {
        if (const auto v = p.to_uint(ev.token)) val = static_cast<T>(*v);
        else ctx.push_diagnostic(error{error_type::err_expect_token_number_uint, ev.token.span});
      }
    }
  }
  else if constexpr (std::is_floating_point_v<T>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if (const auto v = p.to_float(ev.token)) val = static_cast<T>(*v);
      else ctx.push_diagnostic(error{error_type::err_expect_token_number_float, ev.token.span});
    }
  }
  else if constexpr (ds_is_time_point<T>::value || ds_is_duration<T>::value) {
    // datetime token -> Unix milliseconds (UTC). time_point is built from epoch duration; duration
    // receives the raw epoch-millisecond value without calendar semantics.
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if (const auto dt = p.to_datetime(ev.token)) {
        const auto ms = detail::iso_datetime_to_unix_ms(*dt);
        if constexpr (ds_is_time_point<T>::value) val = T{std::chrono::duration_cast<typename T::duration>(ms)};
        else                                      val = std::chrono::duration_cast<T>(ms);
      }
    }
  }
  else if constexpr (std::is_same_v<T, std::string>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      val = p.to_string(ev.token);
    }
  }
  else if constexpr (ds_is_char_array<T>::value) {         // std::array<char,N> / char[N] string buffer
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      ctx.strbuf = p.to_string(ev.token);                  // context buffer avoids local containers
      char* data; size_t n;
      if constexpr (std::is_array_v<T>) { data = val; n = std::extent_v<T>; }
      else                              { data = val.data(); n = val.size(); }
      const size_t len = ctx.strbuf.size();
      for (size_t i = 0; i < n; ++i) data[i] = (i < len) ? ctx.strbuf[i] : char(0);   // copy + zero-fill tail
      if (len > n) ctx.push_diagnostic(error{error_type::err_string_too_long, ev.token.span});
    }
  }
  else if constexpr (ds_is_optional<T>::value) {
    if (ctx.cursor < ctx.frames.size()) {                  // resume: continue filling the contained value
      ctx.cursor++;
      if (val.has_value()) { deserialize(p, ctx, *val); if (ctx.stalled) return; }
      ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
    } else {
      event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
      if (ev.type == event_type::got_token && ev.token.type == token_type::null) { ct_next(p, ctx); val.reset(); }
      else {
        ctx.frames.push_back(ct_context::frame{}); ctx.cursor = ctx.frames.size();
        val.emplace();
        deserialize(p, ctx, *val);
        if (ctx.stalled) return;
        ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
      }
    }
  }
  else if constexpr (ds_is_unique_ptr<T>::value) {
    using U = typename T::element_type;
    if (ctx.cursor < ctx.frames.size()) {                  // resume
      ctx.cursor++;
      if (val) { deserialize(p, ctx, *val); if (ctx.stalled) return; }
      ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
    } else {
      event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
      if (ev.type == event_type::got_token && ev.token.type == token_type::null) { ct_next(p, ctx); val.reset(); }
      else {
        ctx.frames.push_back(ct_context::frame{}); ctx.cursor = ctx.frames.size();
        val = std::make_unique<U>();
        deserialize(p, ctx, *val);
        if (ctx.stalled) return;
        ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
      }
    }
  }
  else if constexpr (ds_is_pair<T>::value) {
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.pos == 0) { ds_read_slot(p, ctx, fr.term, val.first);  if (ctx.stalled) return; fr.pos = 1; }
    if (fr.pos == 1) { ds_read_slot(p, ctx, fr.term, val.second); if (ctx.stalled) return; fr.pos = 2; }
    ds_finish_group(p, ctx, fr.term); if (ctx.stalled) return;
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_is_tuple<T>::value) {
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    size_t idx = 0;
    std::apply([&](auto&... slots) {
      ([&]{
        if (ctx.stalled || idx < fr.pos) { idx += 1; return; }   // already stopped or this slot is done
        ds_read_slot(p, ctx, fr.term, slots);
        if (!ctx.stalled) fr.pos = idx + 1;                      // on stall, leave pos so resume rereads the slot
        idx += 1;
      }(), ...);
    }, val);
    if (ctx.stalled) return;
    ds_finish_group(p, ctx, fr.term); if (ctx.stalled) return;
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_is_std_array<T>::value) {
    using E = typename T::value_type;
    bool resuming;
    const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;

    auto& fr = ctx.frames[fi];
    if (fr.in_element) {
      if (fr.pos < val.size()) { deserialize(p, ctx, val[fr.pos]); }
      else { E tmp{}; deserialize(p, ctx, tmp); }
      if (ctx.stalled) return;
      fr.in_element = false;
      fr.next = fr.pos + 1;
    }

    while (true) {
      const event ev = ct_peek(p, ctx);
      if (ctx.stalled) return;

      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment)
        { ct_next(p, ctx); continue; }

      fr.pos = fr.next;
      fr.in_element = true;
      if (fr.pos < val.size()) { deserialize(p, ctx, val[fr.pos]); }
      else { E tmp{}; deserialize(p, ctx, tmp); }

      if (ctx.stalled) return;
      fr.in_element = false;
      fr.next = fr.pos + 1;
    }
    if (fr.next != val.size()) ctx.push_diagnostic(error{error_type::err_too_many_values, {}});  // size mismatch
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_has_push_back<T>::value) {         // vector / list / deque
    using E = typename T::value_type;
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.in_element) { deserialize(p, ctx, val.back()); if (ctx.stalled) return; fr.in_element = false; }
    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }
      val.push_back(E{});
      fr.in_element = true;
      deserialize(p, ctx, val.back());
      if (ctx.stalled) return;
      fr.in_element = false;
    }
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_is_map<T>::value) {                // map / unordered_map: key=value entries in {} or ()
    using K = typename T::key_type;
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.in_element) {                                   // finish the value for the key saved in carry
      deserialize(p, ctx, val[*std::any_cast<K>(&fr.carry)]); if (ctx.stalled) return;
      fr.in_element = false; fr.carry.reset();
    } else if (fr.skipping) { if (ds_skip_framed(p, ctx, fr)) return; }
    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }
      if (ev.type == event_type::got_row_identifier) {     // keyed entry
        fr.carry = K{};                                    // build key in carry, not a local container
        deserialize(p, ctx, *std::any_cast<K>(&fr.carry)); if (ctx.stalled) return;  // scalar key is idempotent
        fr.in_element = true;                              // key is fixed; val[key] creates the entry
        deserialize(p, ctx, val[*std::any_cast<K>(&fr.carry)]); if (ctx.stalled) return;
        fr.in_element = false; fr.carry.reset();
      } else {
        if (ds_skip_framed(p, ctx, fr)) return;            // bare entry without a key is not a pair
      }
    }
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_has_key_type<T>::value) {          // set / multiset / unordered_set; map handled above
    using E = typename T::value_type;
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.in_element) {                                   // finish the partial element held in carry
      deserialize(p, ctx, *std::any_cast<E>(&fr.carry)); if (ctx.stalled) return;
      val.insert(std::move(*std::any_cast<E>(&fr.carry)));
      fr.in_element = false; fr.carry.reset();
    }
    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }
      fr.carry = E{}; fr.in_element = true;                // build the element in carry so it survives unwinding
      deserialize(p, ctx, *std::any_cast<E>(&fr.carry)); if (ctx.stalled) return;
      val.insert(std::move(*std::any_cast<E>(&fr.carry)));
      fr.in_element = false; fr.carry.reset();
    }
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (std::is_aggregate_v<T>) {
    static_assert(reflect::size<T>() <= limits::max_fields, "tavl::deserialize: aggregate has more than limits::max_fields members");
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::eof);   // root reads until eof
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (!resuming) { size_t fc = 0; reflect::for_each([&](auto) { fc += 1; }, val); fr.field_count = fc; fr.seen.reset(); }

    if (fr.in_element) {                                   // finish sub-field fr.pos
      const size_t cur = fr.pos; const bool was_pos = fr.positional;
      size_t counter = 0;
      reflect::for_each([&](auto I) { if (counter == cur) deserialize(p, ctx, reflect::get<I>(val)); counter += 1; }, val);
      if (ctx.stalled) return;
      fr.in_element = false;
      if (!was_pos) { if (ds_skip_framed(p, ctx, fr)) return; }   // named: skip extra tokens until row_end
    } else if (fr.skipping) { if (ds_skip_framed(p, ctx, fr)) return; }

    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }

      if (ev.type == event_type::got_row_identifier) {     // named: row id -> field
        const auto name = p.content(ev.token.span);
        const auto name_span = ev.token.span;
        ct_next(p, ctx);
        size_t found = SIZE_MAX, counter = 0;
        std::string_view fname{};   // static member_name; unlike parser-buffer name, it stays valid
        reflect::for_each([&](auto I) {
          if (found == SIZE_MAX && name == reflect::member_name<I>(val)) { found = counter; fname = reflect::member_name<I>(val); }
          counter += 1;
        }, val);
        if (found != SIZE_MAX && found < fr.field_count && fr.seen[found]) {
          // The field was already filled, either positionally or by name.
          ctx.push_diagnostic(error{error_type::err_duplicate_field, name_span}, fname);
          if (ds_skip_framed(p, ctx, fr)) return;
        } else if (found != SIZE_MAX) {
          fr.seen.set(found);
          fr.pos = found; fr.in_element = true; fr.positional = false;
          counter = 0;
          reflect::for_each([&](auto I) { if (counter == found) deserialize(p, ctx, reflect::get<I>(val)); counter += 1; }, val);
          if (ctx.stalled) return;
          fr.in_element = false;
          if (ds_skip_framed(p, ctx, fr)) return;          // skip extra tokens until row_end
        } else {
          if (ds_skip_framed(p, ctx, fr)) return;          // unknown field
        }
      } else {                                             // positional -> first unfilled field
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < fr.field_count; ++i) if (!fr.seen[i]) { idx = i; break; }
        if (idx == SIZE_MAX) {                             // no fields left
          ctx.push_diagnostic(error{error_type::err_too_many_values, ev.token.span});
          if (ds_skip_framed(p, ctx, fr)) return;
        } else {
          fr.seen.set(idx);
          fr.pos = idx; fr.in_element = true; fr.positional = true;
          size_t counter = 0;
          reflect::for_each([&](auto I) { if (counter == idx) deserialize(p, ctx, reflect::get<I>(val)); counter += 1; }, val);
          if (ctx.stalled) return;
          fr.in_element = false;
        }
      }
    }

    // Warn about missing required fields; optional and unique_ptr may be absent.
    size_t ci = 0;
    reflect::for_each([&](auto I) {
      using FT = std::remove_reference_t<decltype(reflect::get<I>(val))>;
      if (ci < fr.field_count && !fr.seen[ci] && !ds_is_optional<FT>::value && !ds_is_unique_ptr<FT>::value) {
        ctx.push_diagnostic(error{error_type::warn_missing_field, {}}, reflect::member_name<I>(val));
      }
      ci += 1;
    }, val);
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else {
    // Unsupported primitive/container/aggregate category (enum or custom type). Users can provide
    // an ADL deserialize overload; otherwise we skip one value.
    size_t d = 0;
    ds_skip_value(p, ctx, d);
  }
}

// Read one top-level T from the current stream position (skipping row markers). Returns false at eof.
// The same loop reads one instance from a single-record file or N instances from a top-level list;
// the list shape is only a convention, not a separate schema. Use parser::peek() to classify up front:
// first content event being a block begin means list. Comments, including //--- separators, are skipped;
// split documents before iterating. Assumes all input for the current document has been flushed.
template <typename T>
bool deserialize_next(parser& p, ct_context& ctx, T& out) {
  for (;;) {
    const event ev = p.peek();
    if (ev.type == event_type::eof || ev.type == event_type::not_enought_data) return false;
    if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
        ev.type == event_type::empty_row || ev.type == event_type::got_comment) {
      p.poll_event();   // skip top-level marker
      continue;
    }
    break;              // first content event
  }
  deserialize(p, ctx, out);
  return true;
}

// Release already-consumed input while streaming. This considers both parser lookahead and the
// deserialize-level cached event. After release, content() for discarded spans is empty, but their
// values have already been copied into the output object.
inline void release_consumed(parser& p, ct_context& ctx) {
  size_t boundary = p.consumed_offset();
  if (ctx.peeked) {
    const size_t o = ctx.peeked->token.span.offset;
    if (o != SIZE_MAX && o < boundary) boundary = o;
  }
  p.release_before(boundary);
}

}
