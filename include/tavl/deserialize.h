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
    event_type term = event_type::eof;  // терминатор уровня (block-end / row_end / eof)
    size_t pos = 0;                      // индекс под-элемента в обработке (поле/слот) либо число прочитанных слотов
    size_t next = 0;                     // позиционный счётчик (array index)
    bool in_element = false;             // под-элемент начат, но не дочитан (стоп пришёлся внутрь него)
    bool positional = false;             // текущий под-элемент агрегата - позиционный
    bool skipping = false;               // идёт пропуск значения (ds_skip_value), прерванный стопом
    size_t skip_depth = 0;               // глубина скобок незавершённого пропуска (для резюма)
    std::any carry;                      // частичный ключ (map) / элемент (set) при резюме внутри записи
    size_t field_count = 0;              // число полей агрегата (>256 - ошибка компиляции, см. static_assert)
    std::bitset<limits::max_fields> seen;// отметки заполненных полей (фикс. размер вместо vector)
  };

  struct diagnostic { struct error error; std::string_view field; };

  std::optional<event> peeked;          // 1-событийный lookahead (не кэшируем not_enought_data!)
  std::deque<frame> frames;             // стек кадров уровней (резюм)
  size_t cursor = 0;                    // курсор повторного спуска по frames (сброс в 0 на внешнем входе)
  bool stalled = false;                 // встретили not_enought_data - разматываемся наружу
  std::vector<diagnostic> diagnostics;
  std::string strbuf;                   // переиспользуемый буфер строк (char-массивы) - не плодим локальные
  size_t depth = 0;                     // глубина рекурсии (для распознавания внешнего кадра)

  // диагностика с жёстким лимитом: дальше limits::max_diagnostics не пишем (обрезаем)
  void push_diagnostic(struct error e, std::string_view field = {}) {
    if (diagnostics.size() < limits::max_diagnostics) diagnostics.push_back({e, field});
  }
};

// RAII внешнего кадра: на входе сбрасывает курсор/stalled; на СВЕЖЕЙ сессии (frames пуст) чистит lookahead/диагностику.
struct ct_scope {
  ct_context& ctx;
  explicit ct_scope(ct_context& c) noexcept;
  ~ct_scope();
};

template <typename T> void deserialize(parser& p, ct_context& ctx, T& val);   // forward

// --- хелперы потока событий (определения в deserialize.cpp) ---
// lookahead-обёртки. not_enought_data НЕ кэшируем (переполлим после flush) и помечаем ctx.stalled.
// Ошибки парсера, приходящие с событием, собираем в ctx.diagnostics (1 раз на событие - при полле).
event ct_peek(parser& p, ct_context& ctx);
event ct_next(parser& p, ct_context& ctx);

bool ds_is_block_begin(event_type t);
bool ds_is_block_end(event_type t);
event_type ds_block_end(event_type begin);

// вход в loop-уровень: резюм -> усыновляем кадр; свежий -> detect term + push. SIZE_MAX + ctx.stalled -> вызыватель return.
size_t ds_enter(parser& p, ct_context& ctx, bool& resuming, event_type default_term);

// пропуск ОДНОГО значения (с вложенными скобками) до row_end текущего элемента, не съедая row_end.
// depth - глубина скобок (in/out): хранится в кадре, чтобы пропуск переживал стоп посреди вложенного блока.
void ds_skip_value(parser& p, ct_context& ctx, size_t& depth);

// пропуск значения с резюмируемым состоянием в кадре. true = стоп (вызыватель должен return).
bool ds_skip_framed(parser& p, ct_context& ctx, ct_context::frame& fr);

// дочитывает группу до терминатора (съедая закрывающую скобку, если term - *_end; row_end оставляем родителю)
void ds_finish_group(parser& p, ct_context& ctx, event_type term);

// читает ОДИН слот (pair/tuple): пропускает разделители (делимитеры строк + операторы '='/',') и читает значение
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
    }
  }
  else if constexpr (std::is_integral_v<T>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if (const auto v = p.to_int(ev.token)) val = static_cast<T>(*v);   // to_int принимает int+uint(hex/oct/bin)
    }
  }
  else if constexpr (std::is_floating_point_v<T>) {
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      if (const auto v = p.to_float(ev.token)) val = static_cast<T>(*v);
    }
  }
  else if constexpr (ds_is_time_point<T>::value || ds_is_duration<T>::value) {
    // datetime-токен -> Unix-мс (UTC); time_point строим из длительности от эпохи,
    // duration получает «сырые» миллисекунды от эпохи (как и просили - без календарной семантики)
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
  else if constexpr (ds_is_char_array<T>::value) {         // std::array<char,N> / char[N] - строковый буфер
    event ev = ct_peek(p, ctx); if (ctx.stalled) return;
    if (ev.type == event_type::got_row_operator) { ct_next(p, ctx); ev = ct_peek(p, ctx); if (ctx.stalled) return; }
    if (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) {
      ct_next(p, ctx);
      ctx.strbuf = p.to_string(ev.token);                  // строка в контекстный буфер (без локального контейнера)
      char* data; size_t n;
      if constexpr (std::is_array_v<T>) { data = val; n = std::extent_v<T>; }
      else                              { data = val.data(); n = val.size(); }
      const size_t len = ctx.strbuf.size();
      for (size_t i = 0; i < n; ++i) data[i] = (i < len) ? ctx.strbuf[i] : char(0);   // копируем + zero-fill хвост
      if (len > n) ctx.push_diagnostic(error{error_type::err_string_too_long, ev.token.span});
    }
  }
  else if constexpr (ds_is_optional<T>::value) {
    if (ctx.cursor < ctx.frames.size()) {                  // РЕЗЮМ: продолжаем заполнять внутреннее
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
    if (ctx.cursor < ctx.frames.size()) {                  // РЕЗЮМ
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
        if (ctx.stalled || idx < fr.pos) { idx += 1; return; }   // стоп выше / слот уже прочитан
        ds_read_slot(p, ctx, fr.term, slots);
        if (!ctx.stalled) fr.pos = idx + 1;                      // при стопе pos не двигаем -> резюм перечитает слот
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
    if (fr.next != val.size()) ctx.push_diagnostic(error{error_type::err_too_many_values, {}});  // размер не совпал
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
  else if constexpr (ds_is_map<T>::value) {                // map / unordered_map: записи key=value в {} или ()
    using K = typename T::key_type;
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.in_element) {                                   // дочитать value для сохранённого ключа (в carry)
      deserialize(p, ctx, val[*std::any_cast<K>(&fr.carry)]); if (ctx.stalled) return;
      fr.in_element = false; fr.carry.reset();
    } else if (fr.skipping) { if (ds_skip_framed(p, ctx, fr)) return; }
    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }
      if (ev.type == event_type::got_row_identifier) {     // запись с ключом
        fr.carry = K{};                                    // ключ строим В carry (без локального контейнера)
        deserialize(p, ctx, *std::any_cast<K>(&fr.carry)); if (ctx.stalled) return;  // ключ (скаляр - идемпотентен)
        fr.in_element = true;                              // ключ зафиксирован -> читаем value (val[key] создаст запись)
        deserialize(p, ctx, val[*std::any_cast<K>(&fr.carry)]); if (ctx.stalled) return;
        fr.in_element = false; fr.carry.reset();
      } else {
        if (ds_skip_framed(p, ctx, fr)) return;            // bare-запись без ключа - не пара
      }
    }
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (ds_has_key_type<T>::value) {          // set / multiset / unordered_set (map уже выше)
    using E = typename T::value_type;
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::row_end);
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (fr.in_element) {                                   // дочитать частичный элемент из carry
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
      fr.carry = E{}; fr.in_element = true;                // элемент строим В carry (переживает разматывание)
      deserialize(p, ctx, *std::any_cast<E>(&fr.carry)); if (ctx.stalled) return;
      val.insert(std::move(*std::any_cast<E>(&fr.carry)));
      fr.in_element = false; fr.carry.reset();
    }
    ctx.frames.pop_back(); ctx.cursor = ctx.frames.size();
  }
  else if constexpr (std::is_aggregate_v<T>) {
    static_assert(reflect::size<T>() <= limits::max_fields, "tavl::deserialize: aggregate has more than limits::max_fields members");
    bool resuming; const size_t fi = ds_enter(p, ctx, resuming, event_type::eof);   // корень -> до eof
    if (ctx.stalled) return;
    auto& fr = ctx.frames[fi];
    if (!resuming) { size_t fc = 0; reflect::for_each([&](auto) { fc += 1; }, val); fr.field_count = fc; fr.seen.reset(); }

    if (fr.in_element) {                                   // дочитать под-поле fr.pos
      const size_t cur = fr.pos; const bool was_pos = fr.positional;
      size_t counter = 0;
      reflect::for_each([&](auto I) { if (counter == cur) deserialize(p, ctx, reflect::get<I>(val)); counter += 1; }, val);
      if (ctx.stalled) return;
      fr.in_element = false;
      if (!was_pos) { if (ds_skip_framed(p, ctx, fr)) return; }   // named: съесть лишние токены до row_end
    } else if (fr.skipping) { if (ds_skip_framed(p, ctx, fr)) return; }

    while (true) {
      const event ev = ct_peek(p, ctx); if (ctx.stalled) return;
      if (ev.type == event_type::eof) break;
      if (ev.type == fr.term) { if (ds_is_block_end(fr.term)) ct_next(p, ctx); break; }
      if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
          ev.type == event_type::empty_row || ev.type == event_type::got_comment) { ct_next(p, ctx); continue; }

      if (ev.type == event_type::got_row_identifier) {     // named: имя -> поле
        const auto name = p.content(ev.token.span);
        const auto name_span = ev.token.span;
        ct_next(p, ctx);
        size_t found = SIZE_MAX, counter = 0;
        std::string_view fname{};   // статичное имя поля (member_name) - в отличие от name (буфер парсера) не висит
        reflect::for_each([&](auto I) {
          if (found == SIZE_MAX && name == reflect::member_name<I>(val)) { found = counter; fname = reflect::member_name<I>(val); }
          counter += 1;
        }, val);
        if (found != SIZE_MAX && found < fr.field_count && fr.seen[found]) {
          // поле уже заполнено (по индексу/повторно по имени) - явная ошибка пользователя
          ctx.push_diagnostic(error{error_type::err_duplicate_field, name_span}, fname);
          if (ds_skip_framed(p, ctx, fr)) return;
        } else if (found != SIZE_MAX) {
          fr.seen.set(found);
          fr.pos = found; fr.in_element = true; fr.positional = false;
          counter = 0;
          reflect::for_each([&](auto I) { if (counter == found) deserialize(p, ctx, reflect::get<I>(val)); counter += 1; }, val);
          if (ctx.stalled) return;
          fr.in_element = false;
          if (ds_skip_framed(p, ctx, fr)) return;          // съесть лишние токены до row_end
        } else {
          if (ds_skip_framed(p, ctx, fr)) return;          // поле не найдено - пропускаем
        }
      } else {                                             // positional -> в наименьшее НЕзаполненное поле
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < fr.field_count; ++i) if (!fr.seen[i]) { idx = i; break; }
        if (idx == SIZE_MAX) {                             // полей не осталось - лишнее значение
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

    // warning о незаполненных полях (optional/unique_ptr - отсутствие норм)
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
    // не распознан как примитив/контейнер/агрегат (enum, кастомные типы) -
    // пользователь задаёт через перегрузку deserialize; иначе пропускаем значение
    size_t d = 0;
    ds_skip_value(p, ctx, d);
  }
}

// Читает ОДИН верхнеуровневый экземпляр T из текущей позиции (пропустив маркеры строк), true; eof -> false.
// В цикле: одиночный файл -> 1 экземпляр; файл-список ((...),(...),...) -> N. Это просто инструмент -
// «лист» НЕ стандартизируется: каждый верхнеуровневый блок/строка трактуется как отдельный экземпляр.
// Классифицировать заранее можно через parser::peek() (первое содержательное событие: блок-begin -> список).
// Маркеры-комментарии (в т.ч. //--- разделитель документов) пропускаются - документы разделять ДО итерации.
// Предполагает полный флаш входа; диагностики каждого экземпляра - в ctx (перетираются на следующем вызове).
template <typename T>
bool deserialize_next(parser& p, ct_context& ctx, T& out) {
  for (;;) {
    const event ev = p.peek();
    if (ev.type == event_type::eof || ev.type == event_type::not_enought_data) return false;
    if (ev.type == event_type::row_begin || ev.type == event_type::row_end ||
        ev.type == event_type::empty_row || ev.type == event_type::got_comment) {
      p.poll_event();   // пропускаем маркер верхнего уровня
      continue;
    }
    break;              // на содержательном событии
  }
  deserialize(p, ctx, out);
  return true;
}

// Освободить уже прочитанный ввод (стриминг): для сетевых пачек - после каждого deserialize_next
// сбрасываем разобранные байты, чтобы буфер не рос с общим объёмом потока. Учитывает и lookahead
// парсера (consumed_offset), и буферизованное событие deserialize (ctx.peeked) - его байты не трогаем.
// После вызова content() для отброшенных спанов пуст, но их значения уже извлечены в out.
inline void release_consumed(parser& p, ct_context& ctx) {
  size_t boundary = p.consumed_offset();
  if (ctx.peeked) {
    const size_t o = ctx.peeked->token.span.offset;
    if (o != SIZE_MAX && o < boundary) boundary = o;
  }
  p.release_before(boundary);
}

}
