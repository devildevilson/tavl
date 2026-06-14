#include "tavl/deserialize.h"

namespace tavl {

// RAII внешнего кадра: на входе сбрасывает курсор/stalled; на СВЕЖЕЙ сессии (frames пуст)
// чистит lookahead/диагностику.
ct_scope::ct_scope(ct_context& c) noexcept : ctx(c) {
  if (ctx.depth == 0) {
    ctx.cursor = 0;
    ctx.stalled = false;
    if (ctx.frames.empty()) { ctx.peeked.reset(); ctx.diagnostics.clear(); }
  }
  ctx.depth += 1;
}
ct_scope::~ct_scope() { ctx.depth -= 1; }

event ct_peek(parser& p, ct_context& ctx) {
  if (!ctx.peeked) {
    auto [ev, err] = p.poll_event();
    if (!err.no_error()) ctx.push_diagnostic(err);
    if (ev.type == event_type::not_enought_data) { ctx.stalled = true; return ev; }
    ctx.peeked = ev;
  }
  return *ctx.peeked;
}
event ct_next(parser& p, ct_context& ctx) {
  if (ctx.peeked) { const event e = *ctx.peeked; ctx.peeked.reset(); return e; }
  auto [ev, err] = p.poll_event();
  if (!err.no_error()) ctx.push_diagnostic(err);
  if (ev.type == event_type::not_enought_data) ctx.stalled = true;
  return ev;
}

bool ds_is_block_begin(event_type t) {
  return t == event_type::tuple_begin || t == event_type::object_begin || t == event_type::array_begin;
}
bool ds_is_block_end(event_type t) {
  return t == event_type::tuple_end || t == event_type::object_end || t == event_type::array_end;
}
event_type ds_block_end(event_type begin) {
  switch (begin) {
    case event_type::tuple_begin:  return event_type::tuple_end;
    case event_type::object_begin: return event_type::object_end;
    case event_type::array_begin:  return event_type::array_end;
    default: return event_type::invalid;
  }
}

// skip-op + детект режима. Возвращает term: блок -> *_end (входим), россыпь токенов -> row_end, иначе default_term.
static event_type ds_detect_term(parser& p, ct_context& ctx, event_type default_term) {
  event ev = ct_peek(p, ctx);
  if (ctx.stalled) return event_type::invalid;
  if (ev.type == event_type::got_row_operator) {
    ct_next(p, ctx);
    ev = ct_peek(p, ctx);
    if (ctx.stalled) return event_type::invalid;
  }
  if (ds_is_block_begin(ev.type)) { const event_type t = ds_block_end(ev.type); ct_next(p, ctx); return t; }
  if (ev.type == event_type::got_token) return event_type::row_end;
  return default_term;
}

size_t ds_enter(parser& p, ct_context& ctx, bool& resuming, event_type default_term) {
  if (ctx.cursor < ctx.frames.size()) { resuming = true; return ctx.cursor++; }
  resuming = false;
  const event_type t = ds_detect_term(p, ctx, default_term);
  if (ctx.stalled) return SIZE_MAX;
  // глубина кадров ограничена вложенностью C++ ТИПА (не вводом); превышение лимита = патологически
  // глубокий тип -> громко падаем (это не малформный ВВОД, а программерская структура)
  TAVL_CHECK(ctx.frames.size() < limits::max_nesting, "deserialize nesting exceeds limits::max_nesting");
  ctx.frames.push_back(ct_context::frame{});
  ctx.frames.back().term = t;
  ctx.cursor = ctx.frames.size();
  return ctx.frames.size() - 1;
}

void ds_skip_value(parser& p, ct_context& ctx, size_t& depth) {
  while (true) {
    const event ev = ct_peek(p, ctx);
    if (ctx.stalled) return;
    if (ev.type == event_type::eof) break;
    if (depth == 0 && ev.type == event_type::row_end) break;
    if (ds_is_block_begin(ev.type)) depth += 1;
    else if (ds_is_block_end(ev.type)) { if (depth == 0) break; depth -= 1; }
    ct_next(p, ctx);
  }
}

bool ds_skip_framed(parser& p, ct_context& ctx, ct_context::frame& fr) {
  fr.skipping = true;
  ds_skip_value(p, ctx, fr.skip_depth);
  if (ctx.stalled) return true;
  fr.skipping = false; fr.skip_depth = 0;
  return false;
}

void ds_finish_group(parser& p, ct_context& ctx, event_type term) {
  while (true) {
    const event ev = ct_peek(p, ctx);
    if (ctx.stalled) return;
    if (ev.type == event_type::eof) break;
    if (ev.type == term) { if (ds_is_block_end(term)) ct_next(p, ctx); break; }
    ct_next(p, ctx);
  }
}

}
