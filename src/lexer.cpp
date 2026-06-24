#include "tavl/lexer.h"
#include "tavl/detail.h"

#include <algorithm>

namespace tavl {

using namespace tavl::detail;

std::string_view char_storage::resolve(const span& s) const {
  const size_t final_offset = s.offset < offset ? 0 : s.offset - offset;
  if (final_offset >= storage.size()) return std::string_view();
  if (s.size == 0) return std::string_view();

  if (final_offset + s.size >= storage.size()) {
    return std::string_view(storage.data() + final_offset, storage.data() + storage.size());
  }

  return std::string_view(storage.data() + final_offset, storage.data() + (final_offset + s.size));
}

char char_storage::peek(const size_t offset) const {
  if (offset < this->offset) return '\0';
  const size_t final_offset = offset - this->offset;
  if (final_offset >= storage.size()) return '\0';
  return storage[final_offset];
}

size_t char_storage::size() const {
  return offset + storage.size();
}

char_storage::span char_storage::append(const std::string_view& data) {
  storage.insert(storage.end(), data.begin(), data.end());
  return {offset, storage.size()};
}

void char_storage::release_before(const size_t offset) {
  if (offset < this->offset) return;
  const size_t final_offset = std::min(offset - this->offset, storage.size());

  storage.erase(storage.begin(), storage.begin() + final_offset);
  this->offset = offset;
}

void char_storage::clear() {
  storage.clear();
  offset = 0;
}

size_t char_storage::buffer_size() const {
  return storage.size();
}

token lexer::lex(const char_storage& storage) {
  // Read until a delimiter, then classify the accumulated span as a scalar token. If that fails,
  // switch to identifier mode and split it into registered operators and identifiers.

  if (state.mode == lexer_mode::standard) {
    while (state.offset < storage.size()) {
      const char prev = storage.peek(state.offset-1);
      const char cur = storage.peek(state.offset);
      if (prev == '/' && cur == '/' && state.offset-1 > state.token_span.offset) {
        auto t = make_token(storage);
        if (!t.valid()) {
          state.mode = lexer_mode::identifier;
          break;
        }

        state.token_span = {state.offset, 0, state.line, state.column};
        return t;
      }

      if (prev == '/' && cur == '*' && state.offset-1 > state.token_span.offset) {
        auto t = make_token(storage);
        if (!t.valid()) {
          state.mode = lexer_mode::identifier;
          break;
        }

        state.token_span = {state.offset, 0, state.line, state.column};
        return t;
      }

      switch (cur) {
        case ' ':
        case '\r':
        case '\f':
        case '\v':
        case '\t':
        case '\n':
        case ',':
        case '{':
        case '(':
        case '[':
        case '}':
        case ')':
        case ']':
        case '"':
        case '\'':
          if (state.offset > state.token_span.offset) {
            auto t = make_token(storage);
            if (!t.valid()) {
              state.mode = lexer_mode::identifier;
              break;
            }

            state.token_span = {state.offset, 0, state.line, state.column};
            return t;
          }
          break;
      }

      if (state.mode != lexer_mode::standard) break;

      if (prev == '/' && cur == '/') {
        state.mode = lexer_mode::line_comment;
        break;
      }

      if (prev == '/' && cur == '*') {
        state.mode = lexer_mode::block_comment;
        state.nesting_depth = 1;
        advance(cur);                 // consume the opening '*'
        state.comment_delim = true;   // '*' already used as the open's second half
        break;
      }

      if (cur == '"') {
        advance(cur);
        state.mode = lexer_mode::doublequote_string;
        break;
      }

      if (cur == '\'') {
        advance(cur);
        state.mode = lexer_mode::singlequote_string;
        break;
      }

      switch (cur) {
        case '{': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::open_brace, s};
        }

        case '(': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::open_paren, s};
        }

        case '[': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::open_bracket, s};
        }

        case '}': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::close_brace, s};
        }

        case ')': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::close_paren, s};
        }

        case ']': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::close_bracket, s};
        }

        case ',': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::comma, s};
        }

        case '\n': {
          auto s = state.token_span;
          s.size = 1;
          advance(cur);
          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::newline, s};
        }
      }

      const size_t skipped = skip_ws(storage);
      if (skipped == 0) advance(cur);
    }
  }

  if (state.mode == lexer_mode::standard && state.finalizing && state.offset > state.token_span.offset) {
    auto t = make_token(storage);
    if (t.valid()) {
      state.token_span = {state.offset, 0, state.line, state.column};
      return t;
    }

    state.mode = lexer_mode::identifier;
  }

  if (state.mode == lexer_mode::doublequote_string) {
    bool string_end = false;
    char c = storage.peek(state.offset);
    while (state.offset < storage.size()) {
      string_end = !state.escaped && c == '"';
      state.escaped = c == '\\';

      if (string_end) {
        state.escaped = false;
        advance(c);

        auto s = state.token_span;
        s.size = state.offset - s.offset;
        state.mode = lexer_mode::standard;

        state.token_span = {state.offset, 0, state.line, state.column};
        return token{token_type::doublequote_string, s};
      }

      advance(c);
      c = storage.peek(state.offset);
    }

    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::doublequote_string, s};
    }
  }

  if (state.mode == lexer_mode::singlequote_string) {
    bool string_end = false;
    char c = storage.peek(state.offset);
    while (state.offset < storage.size()) {
      string_end = !state.escaped && c == '\'';
      state.escaped = c == '\\';

      if (string_end) {
        state.escaped = false;
        advance(c);

        auto s = state.token_span;
        s.size = state.offset - s.offset;
        state.mode = lexer_mode::standard;

        state.token_span = {state.offset, 0, state.line, state.column};
        return token{token_type::singlequote_string, s};
      }

      advance(c);
      c = storage.peek(state.offset);
    }

    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::singlequote_string, s};
    }
  }

  if (state.mode == lexer_mode::line_comment) {
    while (state.offset < storage.size()) {
      const char cur = storage.peek(state.offset);

      if (cur == '\n') {
        auto s = state.token_span;
        s.size = state.offset - s.offset;
        state.mode = lexer_mode::standard;
        state.token_span = {state.offset, 0, state.line, state.column};
        return token{token_type::line_comment, s};
      }

      advance(cur);
    }

    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::line_comment, s};
    }
  }

  if (state.mode == lexer_mode::block_comment) {
    // Block comments nest: a /* inside an open comment opens another level, and the comment only
    // ends when the matching */ for the outermost /* is seen. comment_delim marks that the previous
    // char was already used as a /* or */ half, so it can't be reused for an overlapping delimiter
    // (e.g. the '*' in /*/ must not pair both ways). It is persisted in state to stay correct across
    // streaming chunk boundaries.
    while (state.offset < storage.size()) {
      const char prev = state.comment_delim ? '\0' : storage.peek(state.offset-1);
      const char cur = storage.peek(state.offset);

      if (prev == '/' && cur == '*') {
        state.nesting_depth += 1;
        advance(cur);
        state.comment_delim = true;
        continue;
      }

      if (prev == '*' && cur == '/') {
        advance(cur);
        state.nesting_depth -= 1;
        state.comment_delim = true;

        if (state.nesting_depth == 0) {
          auto s = state.token_span;
          s.size = state.offset - s.offset;
          state.mode = lexer_mode::standard;
          state.comment_delim = false;

          state.token_span = {state.offset, 0, state.line, state.column};
          return token{token_type::block_comment, s};
        }

        continue;
      }

      advance(cur);
      state.comment_delim = false;
    }

    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;
      state.comment_delim = false;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::block_comment, s};
    }
  }

  if (state.mode == lexer_mode::identifier) {
    size_t line = state.token_span.line;
    size_t column = state.token_span.column;
    const auto advance = [this, &line, &column] (const char c) {
      state.division_counter += 1;
      if (c == '\n') {
        line += 1;
        column = 1;
      } else {
        column += 1;
      }
    };

    state.division_counter = state.token_span.offset;

    while (state.division_counter < state.offset) {
      const char c = storage.peek(state.division_counter);

      std::string_view found_op;
      for (const auto& op : operators) {
        const auto str = storage.resolve({state.division_counter, op.op.size()});
        if (str == op.op) { found_op = op.op; break; }
      }

      if (!found_op.empty() && state.division_counter > state.token_span.offset) {
        auto t = make_token(storage, state.division_counter);
        // The prefix before an operator may still be malformed (for example "123abc-x"). That is
        // bad input, not an internal error, so report it through the normal unrecognized-token path.
        if (!t.valid()) t.type = token_type::unrecognized;
        state.token_span = {state.division_counter, 0, line, column};
        return t;
      }

      if (!found_op.empty() && state.division_counter == state.token_span.offset) {
        for (const char c : found_op) advance(c);

        auto s = state.token_span;
        s.size = found_op.size();
        state.token_span = {state.division_counter, 0, line, column};
        if (state.division_counter == state.offset) {
          state.mode = lexer_mode::standard;
        }

        return token{token_type::op, s};
      }

      advance(c);
    }

    auto t = make_token(storage);
    if (t.type == token_type::invalid) {
      t.type = token_type::unrecognized;
    }

    state.token_span = {state.offset, 0, state.line, state.column};
    state.mode = lexer_mode::standard;
    return t;
  }

  if (state.finalizing && state.offset >= storage.size()) {
    return token{token_type::eof, {state.offset, 0, state.line, state.column}};
  }

  auto s = state.token_span;
  s.size = storage.size() - s.offset;
  return token{token_type::not_enought_data, s};
}

void lexer::finish() {
  state.finalizing = true;
}

void lexer::add_operator(const std::string_view& op, op_info info) {
  // Configuration input: invalid operators are programmer errors, not parse errors.
  TAVL_CHECK(check_valid_string_chars(op, operator_chars), "operator must consist of operator_chars");

  const op_entry entry{op, info};
  auto itr = std::upper_bound(operators.begin(), operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();   // longest-match: longer operators first
  });

  operators.insert(itr, entry);
}

void lexer::add_litteral_operator(const std::string_view& op, op_info info) {
  // Literal operators live in identifier space, so invalid ones are programmer errors.
  TAVL_CHECK(is_valid_identificator(op), "litteral operator must be a valid identificator");

  const op_entry entry{op, info};
  auto itr = std::upper_bound(litteral_operators.begin(), litteral_operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();
  });

  litteral_operators.insert(itr, entry);
}

std::optional<op_info> lexer::operator_info(const std::string_view& op, op_fixity fixity) const {
  // The same spelling may be registered with several fixities, for example unary and binary '-'.
  for (const auto& e : operators)          if (e.op == op && e.info.fixity == fixity) return e.info;
  for (const auto& e : litteral_operators) if (e.op == op && e.info.fixity == fixity) return e.info;
  return std::nullopt;
}

void lexer::clear_operators() {
  operators.clear();
  litteral_operators.clear();
}

void lexer::clear() {
  state = lexer_state();
}

bool lexer::finalizing() const noexcept {
  return state.finalizing;
}

size_t lexer::token_offset() const noexcept {
  return state.token_span.offset;
}

void lexer::advance(const char c) {
  state.offset += 1;
  if (c == '\n') {
    state.line += 1;
    state.column = 1;
  } else {
    state.column += 1;
  }
}

size_t lexer::skip_ws(const char_storage& storage) {
  size_t counter = 0;
  auto c = storage.peek(state.offset);
  while (is_whitespace_except_newline(c)) {
    advance(c);
    state.token_span = {state.offset, 0, state.line, state.column};
    c = storage.peek(state.offset);
    counter += 1;
  }

  return counter;
}

token lexer::make_token(const char_storage& storage, size_t local_offset) {
  local_offset = local_offset == SIZE_MAX ? state.offset : local_offset;

  auto s = state.token_span;
  s.size = local_offset - s.offset;

  const auto str = storage.resolve({s.offset, s.size});

  for (const auto &op : litteral_operators) {
    if (str == op.op) return token{token_type::op, s};
  }

  // Datetime has priority over operators and numbers: it contains '-' and ':' but should not be
  // split by a registered '-' operator.
  if (iso_datetime dt; is_datetime(str, dt)) {
    return token{token_type::datetime, s};
  }

  // If a token contains a registered symbolic operator, return invalid so identifier mode can split
  // it into operator + remainder. This check is intentionally above number parsing: with '-' registered,
  // '-123' becomes op + number; without it, it remains a signed number.
  for (const auto &op : operators) {
    if (str.find(op.op) != std::string_view::npos) return token{token_type::invalid, s};
  }

  if (is_null(str)) {
    return token{token_type::null, s};
  }

  if (bool v = false; is_boolean(str, v)) {
    return token{token_type::boolean, s};
  }

  if (uint64_t v = 0; is_bin_number(str, v)) {
    return token{token_type::number_uint, s};
  }

  if (uint64_t v = 0; is_oct_number(str, v)) {
    return token{token_type::number_uint, s};
  }

  if (uint64_t v = 0; is_hex_number(str, v)) {
    return token{token_type::number_uint, s};
  }

  if (int64_t v = 0; is_dec_number(str, v)) {
    return token{token_type::number_int, s};
  }

  if (double v = 0; is_float_number(str, v)) {
    return token{token_type::number_float, s};
  }

  if (is_valid_identificator(str)) {
    return token{token_type::identifier, s};
  }

  return token{token_type::invalid, s};
}

}
