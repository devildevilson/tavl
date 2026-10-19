#include "tavl/lexer.h"
#include "tavl/detail.h"

#include <algorithm>

namespace tavl {

using namespace tavl::detail;

detail::float_parse_result lexer::parse_float(std::string_view text, double& value) const {
  return parse_float_number(text, value, float_workspace);
}

std::string_view char_storage::resolve(const span& s) const {
  if (s.offset < offset) return {};
  const size_t final_offset = s.offset - offset;
  const size_t live_size = storage.size() - begin;
  if (final_offset >= live_size) return std::string_view();
  if (s.size == 0) return std::string_view();

  return std::string_view(storage.data() + begin + final_offset, std::min(s.size, live_size - final_offset));
}

char char_storage::peek(const size_t offset) const {
  if (offset < this->offset) return '\0';
  const size_t final_offset = offset - this->offset;
  if (final_offset >= storage.size() - begin) return '\0';
  return storage[begin + final_offset];
}

size_t char_storage::size() const {
  return offset + storage.size() - begin;
}

char_storage::span char_storage::append(const std::string_view& data) {
  // Release is O(1). Compact only when new input needs the unused prefix (or
  // the prefix has grown larger than the live tail), preserving capacity.
  if (begin && (storage.capacity() - storage.size() < data.size() ||
                begin > storage.size() / 2)) {
    storage.erase(storage.begin(), storage.begin() + begin);
    begin = 0;
  }
  storage.insert(storage.end(), data.begin(), data.end());
  return {offset, storage.size() - begin};
}

void char_storage::release_before(const size_t offset) {
  if (offset < this->offset) return;
  const size_t final_offset = std::min(offset - this->offset, storage.size() - begin);

  begin += final_offset;
  this->offset += final_offset;
  if (begin == storage.size()) {
    storage.clear();
    begin = 0;
  }
}

bool char_storage::truncate_after(size_t absolute_offset) {
  if (absolute_offset < offset || absolute_offset > size()) return false;
  storage.resize(begin + absolute_offset - offset);
  return true;
}

void char_storage::clear() {
  storage.clear();
  offset = 0;
  begin = 0;
}

size_t char_storage::buffer_size() const {
  return storage.size() - begin;
}

size_t char_storage::released_offset() const noexcept {
  return offset;
}

std::string_view char_storage::live_source() const noexcept {
  if (storage.size() == begin) return {};
  return {storage.data() + begin, storage.size() - begin};
}

token lexer::lex(const char_storage& storage) {
  // Read until a delimiter, then classify the accumulated span as a scalar token. If that fails,
  // switch to identifier mode and split it into registered operators and identifiers.

  if (state.mode == lexer_mode::standard) {
    while (state.offset < storage.size()) {
      const char prev = state.offset > state.token_span.offset ? storage.peek(state.offset-1) : '\0';
      const char cur = storage.peek(state.offset);
      if (prev == '/' && (cur == '/' || cur == '*') && state.offset-1 > state.token_span.offset) {
        // The first slash belongs to the comment, not to the preceding token. Leave it
        // unread so the next lex call can recognize the complete delimiter, even after release.
        state.offset -= 1;
        state.column -= 1;
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
      state.escaped = !state.escaped && c == '\\';

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
      return token{token_type::doublequote_string, s, false};
    }
  }

  if (state.mode == lexer_mode::singlequote_string) {
    bool string_end = false;
    char c = storage.peek(state.offset);
    while (state.offset < storage.size()) {
      string_end = !state.escaped && c == '\'';
      state.escaped = !state.escaped && c == '\\';

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
      return token{token_type::singlequote_string, s, false};
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
      return token{token_type::block_comment, s, false};
    }
  }

  if (state.mode == lexer_mode::identifier) {
    // After splitting a leading operator, the remaining span can be a complete datetime.
    // Classify it again before scanning its internal '-' and ':' characters as operators.
    const auto remaining = storage.resolve({state.token_span.offset, state.offset - state.token_span.offset});
    if (iso_datetime dt; is_datetime(remaining, dt)) {
      auto s = state.token_span;
      s.size = remaining.size();
      state.token_span = {state.offset, 0, state.line, state.column};
      state.mode = lexer_mode::standard;
      return token{token_type::datetime, s};
    }
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

      // A later operator can make the whole scalar invalid (1e+3+2). While finding
      // its boundary, skip the sign that belongs to a valid exponent prefix.
      if (found_op.size() == 1 && (c == '+' || c == '-') &&
          state.division_counter > state.token_span.offset &&
          (storage.peek(state.division_counter - 1) == 'e' ||
           storage.peek(state.division_counter - 1) == 'E')) {
        size_t end = state.division_counter + 1;
        while (end < state.offset && storage.peek(end) >= '0' && storage.peek(end) <= '9') ++end;
        if (end > state.division_counter + 1) {
          const auto prefix = storage.resolve({state.token_span.offset, end - state.token_span.offset});
          double value = 0;
          if (parse_float(prefix, value) != float_parse_result::invalid) {
            advance(c);
            continue;
          }
        }
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
  TAVL_CHECK(!op.empty() && check_valid_string_chars(op, operator_chars), "operator must be nonempty and consist of operator_chars");

  op_entry entry{std::string(op), info};
  auto itr = std::upper_bound(operators.begin(), operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();   // longest-match: longer operators first
  });

  operators.insert(itr, std::move(entry));
}

void lexer::add_litteral_operator(const std::string_view& op, op_info info) {
  // Literal operators live in identifier space, so invalid ones are programmer errors.
  TAVL_CHECK(is_valid_identificator(op), "litteral operator must be a valid identificator");

  op_entry entry{std::string(op), info};
  auto itr = std::upper_bound(litteral_operators.begin(), litteral_operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();
  });

  litteral_operators.insert(itr, std::move(entry));
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

  // An exponent sign belongs to the float, even when + or - is registered as an operator.
  // Keep a leading sign available to the operator splitter (for example, -1e-3).
  if (!str.empty() && str.front() >= '0' && str.front() <= '9' &&
      str.find_first_of("eE") != std::string_view::npos) {
    double value = 0;
    const auto result = parse_float(str, value);
    if (result == float_parse_result::valid)
      return token{token_type::number_float, s};
    if (result == float_parse_result::out_of_range)
      return token{token_type::number_float, s, true, true};
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

  if (uint64_t v = 0; is_dec_unsigned_number(str, v)) {
    return token{token_type::number_uint, s};
  }

  // All digits with a recognized sign/base prefix, but outside the corresponding integer range.
  // Keep the numeric type and span so callers can report a range error and still inspect the text.
  auto digits = str;
  bool negative = false;
  bool has_sign = false;
  if (!digits.empty() && (digits.front() == '+' || digits.front() == '-')) {
    has_sign = true;
    negative = digits.front() == '-';
    digits.remove_prefix(1);
  }
  unsigned base = 10;
  if (!has_sign && digits.size() >= 2 && digits.front() == '0') {
    switch (digits[1]) {
      case 'b': case 'B': base = 2; digits.remove_prefix(2); break;
      case 'o': case 'O': base = 8; digits.remove_prefix(2); break;
      case 'x': case 'X': base = 16; digits.remove_prefix(2); break;
      default: break;
    }
  }
  const auto digit_value = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return unsigned(c - '0');
    if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return unsigned(c - 'A') + 10;
    return 16;
  };
  if (!digits.empty() && std::all_of(digits.begin(), digits.end(),
                                    [&](char c) { return digit_value(c) < base; })) {
    return token{negative ? token_type::number_int : token_type::number_uint,
                 s, true, false, true};
  }

  double float_value = 0;
  switch (parse_float(str, float_value)) {
    case float_parse_result::valid:
      return token{token_type::number_float, s};
    case float_parse_result::out_of_range:
      return token{token_type::number_float, s, true, true};
    case float_parse_result::invalid:
      break;
  }

  if (is_valid_identificator(str)) {
    return token{token_type::identifier, s};
  }

  return token{token_type::invalid, s};
}

}
