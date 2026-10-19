#include "tavl/parser.h"
#include "tavl/detail.h"

#include <limits>

namespace tavl {

using namespace tavl::detail;

std::string_view parser::content(const source_span& span) const {
  return storage.resolve({span.offset, span.size});
}

// C++-like precedence: higher number binds tighter. The ordinary profile only
// registers assignment; comparisons and math operators are opt-in.
void parser::add_default_operator() {
  add_operator("=", op_fixity::binary, 1, op_assoc::right);   // right-associative assignment
}

void parser::add_additional_default_operators() {
  add_operator(">",  op_fixity::binary, 10);  // relational (C++ <, >, <=, >=)
  add_operator("<",  op_fixity::binary, 10);
  add_operator(">=", op_fixity::binary, 10);
  add_operator("<=", op_fixity::binary, 10);
  add_operator("<>", op_fixity::binary, 9);   // equality/inequality, weaker than relational
  add_operator("==", op_fixity::binary, 9);
}

void parser::add_math_default_operators() {
  clear_operators();
  add_default_operator();
  add_additional_default_operators();

  // The binary C++ operators below are left-associative by default; assignment is right-associative.
  // Unary prefix operators get their direction from fixity.
  add_operator("||",  op_fixity::binary, 4);
  add_operator("&&",  op_fixity::binary, 5);
  add_operator("|",   op_fixity::binary, 6);
  add_operator("^",   op_fixity::binary, 7);
  add_operator("&",   op_fixity::binary, 8);

  //add_operator("<=>", op_fixity::binary, 11);
  add_operator("<<",  op_fixity::binary, 12);
  add_operator(">>",  op_fixity::binary, 12);
  add_operator("+",   op_fixity::binary, 13);
  add_operator("-",   op_fixity::binary, 13);
  add_operator("*",   op_fixity::binary, 14);
  add_operator("/",   op_fixity::binary, 14);
  add_operator("%",   op_fixity::binary, 14);

  add_operator("-",   op_fixity::prefix, 16);
  add_operator("+",   op_fixity::prefix, 16);
  add_operator("!",   op_fixity::prefix, 16);
  add_operator("~",   op_fixity::prefix, 16);
  add_operator("++",  op_fixity::prefix, 16);
  add_operator("--",  op_fixity::prefix, 16);
}

void parser::add_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc) {
  lexer.add_operator(op, op_info{fixity, precedence, assoc});
}

void parser::add_litteral_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc) {
  lexer.add_litteral_operator(op, op_info{fixity, precedence, assoc});
}

std::optional<op_info> parser::operator_info(const std::string_view& op, op_fixity fixity) const {
  return lexer.operator_info(op, fixity);
}

parser::parser() noexcept {
  clear();
}

void parser::clear_operators() {
  lexer.clear_operators();
}

void parser::clear() noexcept {
  storage.clear();
  lexer.clear();

  stack.clear();
  stack.emplace_back();
  events.clear();
  event_cursor = 0;

  ignore_depth = 0;
  pending_modes = block_modes{};
}

void parser::override_next_block_modes(parse_mode object, parse_mode tuple, parse_mode array) {
  pending_modes = block_modes{true, object, tuple, array};
}

void parser::clear_pending_block_modes() {
  pending_modes = block_modes{};
}

void parser::flush(const std::string_view& data) {
  storage.append(data);
}

size_t parser::consumed_offset() const {
  size_t b = lexer.token_offset();
  for (size_t i = event_cursor; i < events.size(); ++i) {
    const auto& pe = events[i];
    const size_t o = pe.event.token.span.offset;
    if (o != SIZE_MAX && o < b) b = o;
  }
  // The current row can retain its identifier/operator until a value or row end arrives.
  // Ancestor rows have already emitted these tokens when their child block was opened.
  if (!stack.empty() && !stack.back().has_value()) {
    const auto& f = stack.back();
    if (f.identifier.exists()) b = std::min(b, f.identifier.span.offset);
    if (f.op.exists()) b = std::min(b, f.op.span.offset);
  }
  return b;
}

void parser::release_before(size_t offset) {
  storage.release_before(offset);
}

static event_type end_event_for(const block_type type) noexcept {
  switch (type) {
    case block_type::object: return event_type::object_end;
    case block_type::array:  return event_type::array_end;
    case block_type::tuple:  return event_type::tuple_end;
    default: break;
  }

  return event_type::invalid;
}

// Queue an event with its attached error, if any.
void parser::emit(const event& ev, error err) {
  if (ev.token.float_out_of_range &&
      (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier) &&
      !err.is_critical())
    err = error{error_type::err_float_out_of_range, ev.token.span};
  if (!ev.token.terminated &&
      (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier ||
       ev.type == event_type::got_comment)) {
    const error_type t = ev.token.type == token_type::block_comment
      ? error_type::err_unterminated_block_comment
      : error_type::err_unterminated_string;
    err = error{t, ev.token.span};
  }
  if (ev.token.type == token_type::doublequote_string && ev.token.terminated &&
      (ev.type == event_type::got_token || ev.type == event_type::got_row_identifier)) {
    const auto raw = content(ev.token.span);
    if (raw.size() >= 2) {
      const auto body = raw.substr(1, raw.size() - 2);
      if (const auto bad = invalid_doublequote_escape(body)) {
        source_span span{ev.token.span.offset + 1 + *bad, 1,
                         ev.token.span.line, ev.token.span.column};
        for (size_t i = 0; i < 1 + *bad; ++i) {
          if (raw[i] == '\n') { ++span.line; span.column = 1; }
          else ++span.column;
        }
        err = error{error_type::err_invalid_escape, span};
      }
    }
  }
  if ((ev.type == event_type::got_token || ev.type == event_type::got_row_identifier ||
       ev.type == event_type::got_row_operator || ev.type == event_type::got_comment) &&
      !err.is_critical()) {
    const auto raw = content(ev.token.span);
    if (const auto issue = invalid_source_text(raw)) {
      source_span span{ev.token.span.offset + issue->offset, issue->size,
                       ev.token.span.line, ev.token.span.column};
      for (size_t i = 0; i < issue->offset; ++i) {
        if (raw[i] == '\n') { ++span.line; span.column = 1; }
        else ++span.column;
      }
      err = error{issue->type, span};
    }
  }
  events.push_back({ev, err});
}

// Diagnostic span for a row: identifier first, then operator, then fallback token.
source_span parser::row_span(const block_frame& f, const token& fallback) const {
  if (f.has_identifier()) return f.identifier.span;
  if (f.op.exists())      return f.op.span;
  return fallback.span;
}

// Emit the delayed id/operator for the current row and close it with row_end.
// Object-like rows without values are reported as warnings.
void parser::close_row(const token& end_tok) {
  auto& f = stack.back();
  if (!f.has_row()) return;

  error row_err{};
  if (f.op.exists() && f.row_mode != parse_mode::data_driven && !f.has_value() &&
      operator_info(content(f.op.span), op_fixity::binary)) {
    row_err = error{error_type::err_missing_value, row_span(f, end_tok)};
  } else if (f.row_mode == parse_mode::object_like && !f.has_value()) {
    row_err = error{error_type::warn_object_without_value, row_span(f, end_tok)};
  }

  if (!f.has_value()) {
    if (f.row_mode == parse_mode::object_like ||
        (f.row_mode == parse_mode::tuple_like && f.op.exists())) {
      // In an object, a lone id/operator is still the row key/operator.
      if (f.has_identifier()) emit(event{event_type::got_row_identifier, f.identifier});
      if (f.op.exists())      emit(event{event_type::got_row_operator, f.op});
    } else if (f.row_mode == parse_mode::tuple_like) {
      // In a tuple, a lone id/operator without a value is just data.
      if (f.has_identifier()) emit(event{event_type::got_token, f.identifier});
      if (f.op.exists())      emit(event{event_type::got_token, f.op});
    }
  }

  emit(event{event_type::row_end, end_tok}, row_err);
  f.new_row();   // make close_row idempotent
}

// Force-close unmatched nested blocks until target is found; each discarded block emits *_end with
// err_bracket_missmatch pointing at its opening bracket.
void parser::close_unmatched(block_type target, const token& closer) {
  while (stack.size() > 1 && stack.back().type != target) {
    close_row(closer);
    const auto f = stack.back();
    const auto err = error{error_type::err_bracket_missmatch, f.open_token.span};
    emit(event{end_event_for(f.type), f.open_token}, err);
    stack.pop_back();
  }
}

// Close a target block with closer. If no matching opener exists, closer itself is reported as the
// extra closing bracket.
void parser::close_block(block_type target, event_type end_ev, const token& closer) {
  close_row(closer);
  close_unmatched(target, closer);
  if (stack.size() <= 1) {
    const auto err = error{error_type::err_bracket_missmatch, closer.span};
    emit(event{end_ev, closer}, err);
  } else {
    stack.pop_back();
    emit(event{end_ev, closer});
  }
  // A closed block counts as a value in the parent row.
  stack.back().last_value = closer;
}

// Open a block of type bt. Shared logic for (){}[]: flush delayed parent id/operator, resolve
// parse_mode (including overrides), then push a new stack frame.
void parser::open_block(const token& c, block_type bt, event_type begin_ev) {
  // Hard nesting limit: emit a critical error and do not push a frame.
  if (stack.size() >= limits::max_nesting) {
    emit(event{begin_ev, c}, error{error_type::err_nesting_too_deep, c.span});
    return;
  }

  error id_err{};
  if (stack.back().leading_binary_operator) {
    id_err = error{error_type::err_expected_identifier, stack.back().last_value.span};
    stack.back().leading_binary_operator = false;
  }
  // At block start, object-like rows must already have a key.
  if (stack.back().row_mode == parse_mode::object_like &&
      !stack.back().has_value() && !stack.back().has_identifier()) {
    if (id_err.no_error()) id_err = error{error_type::warn_expected_identifier, c.span};
  }

  if (!stack.back().has_row()) emit(event{event_type::row_begin, c});

  // The block becomes the row value; emit any delayed id/operator before it.
  if ((stack.back().row_mode == parse_mode::object_like ||
       stack.back().row_mode == parse_mode::tuple_like) && !stack.back().has_value()) {
    if (stack.back().has_identifier()) emit(event{event_type::got_row_identifier, stack.back().identifier});
    if (stack.back().op.exists()) emit(event{event_type::got_row_operator, stack.back().op});
  }

  // Block mode: pending override applies to this block and its descendants, otherwise inherit the
  // parent's mode map, otherwise use the bracket default. The override is consumed by the next block.
  const block_modes eff = pending_modes.active ? pending_modes : stack.back().modes;
  pending_modes = block_modes{};
  parse_mode pm = (bt == block_type::object) ? parse_mode::object_like
                : (bt == block_type::array)  ? parse_mode::data_driven
                :                              parse_mode::tuple_like;
  if (eff.active) pm = (bt == block_type::object) ? eff.object
                     : (bt == block_type::array)  ? eff.array
                     :                              eff.tuple;

  stack.emplace_back();
  stack.back().type = bt;
  stack.back().row_mode = pm;
  stack.back().modes = eff;
  stack.back().open_token = c;
  emit(event{begin_ev, c}, id_err);
}

// Fill the event queue: tokenize input and dispatch tokens by type, keeping row state in the top
// stack frame until at least one event is available.
void parser::fill_events() {
  if (stack.empty() && events.empty()) {
    emit(event{event_type::eof, token{}});
  }

  while (events.empty()) {
    const auto c = lexer.lex(storage);

    // Do not cache not_enought_data in the queue; otherwise peek() would stick on it and never
    // re-lex after more input arrives. Leave the queue empty and synthesize it in poll_event/peek.
    if (c.type == token_type::not_enought_data) break;

    switch (c.type) {
        case token_type::close_paren:   close_block(block_type::tuple,  event_type::tuple_end,  c); break;
        case token_type::close_brace:   close_block(block_type::object, event_type::object_end, c); break;
        case token_type::close_bracket: close_block(block_type::array,  event_type::array_end,  c); break;

        case token_type::comma:
          // Empty row before a comma: synthesize empty_row and warn.
          if (!stack.back().has_row()) {
            emit(event{event_type::empty_row, c}, error{error_type::warn_empty_row, c.span});
            break;
          }
          close_row(c);
          break;

        case token_type::newline:
          close_row(c);
          break;

        case token_type::open_paren:   open_block(c, block_type::tuple,  event_type::tuple_begin);  break;
        case token_type::open_brace:   open_block(c, block_type::object, event_type::object_begin); break;
        case token_type::open_bracket: open_block(c, block_type::array,  event_type::array_begin);  break;

        // An operator after the first non-operator token becomes the delayed row operator.
        case token_type::op: {
          if (!stack.back().has_row()) {
            emit(event{event_type::row_begin, c});
          }

          if (
            (stack.back().row_mode == parse_mode::tuple_like ||
            stack.back().row_mode == parse_mode::object_like) && 
            stack.back().has_identifier() && 
            !stack.back().has_value() &&
            !stack.back().op.exists()
          ) {
            stack.back().op = c;
            break;
          }

          error id_err{};
          if (stack.back().leading_binary_operator) {
            id_err = error{error_type::err_expected_identifier, stack.back().last_value.span};
            stack.back().leading_binary_operator = false;
          }
          if (
            stack.back().row_mode == parse_mode::object_like &&
            !stack.back().has_value() &&
            !stack.back().has_identifier()
          ) {
            id_err = error{error_type::err_expected_identifier, c.span};
          }

          if (stack.back().row_mode == parse_mode::tuple_like &&
              !stack.back().has_identifier() && !stack.back().has_value() &&
              operator_info(content(c.span), op_fixity::binary) &&
              !operator_info(content(c.span), op_fixity::prefix)) {
            stack.back().leading_binary_operator = true;
          }

          // Flush delayed identifier/operator before emitting data.
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) &&
            !stack.back().has_value()
          ) {
            if (stack.back().has_identifier()) {
              emit(event{event_type::got_row_identifier, stack.back().identifier});
            }

            if (stack.back().op.exists()) {
              emit(event{event_type::got_row_operator, stack.back().op});
            }
          }

          stack.back().last_value = c;
          emit(event{event_type::got_token, c}, id_err);
          break;
        }
        
        case token_type::unrecognized:
        case token_type::null:
        case token_type::boolean:
        case token_type::number_int:
        case token_type::number_uint:
        case token_type::number_float:
        case token_type::datetime:
        case token_type::identifier:
        case token_type::doublequote_string:
        case token_type::singlequote_string: {
          if (!stack.back().has_row()) {
            emit(event{event_type::row_begin, c});
          }

          // The first non-operator token in a row becomes an identifier, but only before any value
          // was emitted. If a row starts with an operator (for example unary "-a"), that operator is
          // already data, so following tokens are data too.
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) &&
            !stack.back().has_identifier() &&
            !stack.back().has_value()
          ) {
            stack.back().identifier = c;
            break;
          }

          // Flush delayed identifier/operator before emitting data.
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) && 
            !stack.back().has_value()
          ) {
            if (stack.back().has_identifier()) {
              emit(event{event_type::got_row_identifier, stack.back().identifier});
            }

            if (stack.back().op.exists()) {
              emit(event{event_type::got_row_operator, stack.back().op});
            }
          }

          error value_err{};
          if (stack.back().leading_binary_operator) {
            value_err = error{error_type::err_expected_identifier, stack.back().last_value.span};
            stack.back().leading_binary_operator = false;
          }
          stack.back().last_value = c;
          emit(event{event_type::got_token, c}, value_err);
          break;
        }

        case token_type::eof:
          // End of input: close the pending row and every unclosed block, reporting each block at
          // its opening bracket.
          close_row(c);
          while (stack.size() > 1) {
            const auto f = stack.back();
            const auto err = error{error_type::err_bracket_missmatch, f.open_token.span};
            emit(event{end_event_for(f.type), f.open_token}, err);
            stack.pop_back();
            close_row(c);
          }
          stack.clear();
          emit({event_type::eof, c});
          break;

        case token_type::line_comment:
        case token_type::block_comment:
          emit({event_type::got_comment, c});
          break;

        // Handled before the switch; kept for -Wswitch.
        case token_type::not_enought_data: break;

        // The lexer should not emit invalid (bad text is unrecognized); count is a sentinel.
        case token_type::invalid:
        case token_type::count: TAVL_CHECK(false, "lexer returned invalid/count token"); break;
    }
  }
}

// Pop the next queued event and its attached error.
std::tuple<event, error> parser::poll_event() {
  fill_events();
  if (!events.empty()) {
    const auto pe = events[event_cursor++];
    if (event_cursor == events.size()) {
      events.clear();
      event_cursor = 0;
    }
    return {pe.event, pe.error};
  }
  return {event{event_type::not_enought_data, {}}, error{}};
}

// Non-consuming lookahead, used for stream classification.
event parser::peek() {
  fill_events();
  if (!events.empty()) return events[event_cursor].event;
  return event{event_type::not_enought_data, {}};
}

// Mark the input as complete. The parser may still emit several events afterwards: pending rows,
// synthetic block endings, diagnostics, then eof.
void parser::finish() {
  lexer.finish();
}
std::string parser::to_string(const token& t) const {
  std::string result;
  to_string(t, result);
  return result;
}

void parser::to_string(const token& t, std::string& output) const {
  output.clear();
  if (!t.exists()) return;

  auto cont = content(t.span);
  if (cont.empty()) return;
  if (t.type != token_type::doublequote_string && t.type != token_type::singlequote_string) {
    output.assign(cont);
    return;
  }

  output.reserve(cont.size());

  if (t.type == token_type::singlequote_string) {
    cont.remove_prefix(1);
    if (!cont.empty() && cont.back() == '\'') cont.remove_suffix(1);

    unescape_singlequote_string(cont, output);
    return;
  }

  // doublequote_string: standard escapes plus \u/\U Unicode.
  cont.remove_prefix(1);
  if (!cont.empty() && cont.back() == '\"') cont.remove_suffix(1);

  // Preserve the raw body for recovery; the event already carries err_invalid_escape.
  if (invalid_doublequote_escape(cont)) { output.assign(cont); return; }

  unescape_doublequote_string(cont, output);
}

std::optional<bool> parser::to_boolean(const token& t) const {
  if (t.type != token_type::boolean) return std::nullopt;

  auto cont = content(t.span);

  bool b = false;
  if (is_boolean(cont, b)) return b;
  return std::nullopt;
}

std::optional<int64_t> parser::to_int(const token& t) const {
  if (t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;

  auto cont = content(t.span);

  if (t.type == token_type::number_uint) {
    const auto v = to_uint(t);
    if (v && *v <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return static_cast<int64_t>(*v);
    return std::nullopt;
  }

  int64_t v2 = 0;
  if (is_dec_number(cont, v2)) return v2;
  return std::nullopt;
}

std::optional<uint64_t> parser::to_uint(const token& t) const {
  if (t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;
  auto cont = content(t.span);
  if (cont.empty() || cont[0] == '-') return std::nullopt;

  if (t.type == token_type::number_uint) {
    uint64_t v = 0;
    if (is_bin_number(cont, v)) return std::make_optional(v);
    if (is_oct_number(cont, v)) return std::make_optional(v);
    if (is_hex_number(cont, v)) return std::make_optional(v);
  }

  uint64_t v2 = 0;
  if (is_dec_unsigned_number(cont, v2)) return v2;
  return std::nullopt;
}

std::optional<double> parser::to_float(const token& t) const {
  if (t.type != token_type::number_float && t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;
  if (t.float_out_of_range) return std::nullopt;

  auto cont = content(t.span);

  if (t.type == token_type::number_int) {
    if (const auto v = to_int(t)) {
      const bool negative = *v < 0;
      const uint64_t magnitude = negative
        ? uint64_t{0} - static_cast<uint64_t>(*v) : static_cast<uint64_t>(*v);
      return integer_to_binary64(magnitude, negative);
    }
    return std::nullopt;
  }

  if (t.type == token_type::number_uint) {
    if (const auto v = to_uint(t)) return integer_to_binary64(*v);
    return std::nullopt;
  }

  double v = 0;
  if (lexer.parse_float(cont, v) == float_parse_result::valid) return v;
  return std::nullopt;
}

std::optional<iso_datetime> parser::to_datetime(const token& t) const {
  if (t.type != token_type::datetime) return std::nullopt;

  auto cont = content(t.span);
  iso_datetime dt;
  if (is_datetime(cont, dt)) return dt;
  return std::nullopt;
}

}
