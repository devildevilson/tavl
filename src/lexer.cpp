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
  // читаем входной сторадж, если дошли до управляющих символов
  // то разбираем токены и отправляем что есть
  // тут алгоритм вот какой:
  // получаем строчку до ' ', '\n', ',' и символов скобок
  // если эта строчка не зарезервированное слово или не дата или не число и не идентификатор
  // то пытаемся строку подразбить на пачку идентификаторов + операторы

  if (state.mode == lexer_mode::standard) {
    while (state.offset < storage.size()) {
      const char prev = storage.peek(state.offset-1);
      const char cur = storage.peek(state.offset);
      // адванс должен произойти где то рядом

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
        case '\t': // все вайтспейсы
        case '\n': // новая строка
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

      // переключаем режимы

      if (prev == '/' && cur == '/') {
        state.mode = lexer_mode::line_comment;
        break;
      }

      if (prev == '/' && cur == '*') {
        state.mode = lexer_mode::block_comment;
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

      // отправляем простые ивенты

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

      // на этом этапе мы отправили событие с токеном
      // и теперь возможно остались одни вайтспейсы
      // просто пропускаем их
      // нет остались и обычные символы
      const size_t skipped = skip_ws(storage);
      if (skipped == 0) advance(cur);
    }

    // не нашли токен или переключили режим
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

    // незаконченная строка
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

    // незаконченная строка
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

      if (cur == '\n') { // еще дополнительно нужно отправить newline токен
        auto s = state.token_span;
        s.size = state.offset - s.offset;
        state.mode = lexer_mode::standard;
        state.token_span = {state.offset, 0, state.line, state.column};
        return token{token_type::line_comment, s};
      }

      advance(cur);
    }

    // незаконченный коммент
    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::line_comment, s};
    }
  }

  if (state.mode == lexer_mode::block_comment) {
    while (state.offset < storage.size()) {
      const char prev = storage.peek(state.offset-1);
      const char cur = storage.peek(state.offset);

      if (prev == '*' && cur == '/') {
        advance(cur);

        auto s = state.token_span;
        s.size = state.offset - s.offset;
        state.mode = lexer_mode::standard;

        state.token_span = {state.offset, 0, state.line, state.column};
        return token{token_type::block_comment, s};
      }

      advance(cur);
    }

    // незаконченный коммент
    if (state.finalizing) {
      auto s = state.token_span;
      s.size = state.offset - s.offset;
      state.mode = lexer_mode::standard;

      state.token_span = {state.offset, 0, state.line, state.column};
      return token{token_type::block_comment, s};
    }
  }

  // тут попытаемся подразбить токен на идентификаторы и операторы
  if (state.mode == lexer_mode::identifier) {
    // если даже после возможного разбиения мы получили чушь
    // то только тогда возвращаем unrecognized

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

    // тут наверное должен быть make_token для идентификаторов...
    while (state.division_counter < state.offset) {
      const char c = storage.peek(state.division_counter);

      std::string_view found_op;
      for (const auto& op : operators) {
        const auto str = storage.resolve({state.division_counter, op.op.size()});
        if (str == op.op) { found_op = op.op; break; }
      }

      if (!found_op.empty() && state.division_counter > state.token_span.offset) {
        auto t = make_token(storage, state.division_counter);
        // префикс перед оператором может оказаться не валидным токеном (напр. "123abc-x") -
        // это малформный текст, не баг: отдаём unrecognized (идёт штатно через канал ошибок)
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
  // завершаем отправку данных, это означает что если например лексер не смог завершить строку
  // (не нашел кавычку) то по идее нужно сгенерить 2 токена
  state.finalizing = true;
}

void lexer::add_operator(const std::string_view& op, op_info info) {
  // конфигурационный ввод: невалидный оператор -> громко падаем (иначе разбор будет неожиданным)
  TAVL_CHECK(check_valid_string_chars(op, operator_chars), "operator must consist of operator_chars");

  const op_entry entry{op, info};
  auto itr = std::upper_bound(operators.begin(), operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();   // длинные операторы раньше (longest-match)
  });

  operators.insert(itr, entry);
}

void lexer::add_litteral_operator(const std::string_view& op, op_info info) {
  // конфигурационный ввод: литеральный оператор обязан быть валидным идентификатором -> иначе громко падаем
  TAVL_CHECK(is_valid_identificator(op), "litteral operator must be a valid identificator");

  const op_entry entry{op, info};
  auto itr = std::upper_bound(litteral_operators.begin(), litteral_operators.end(), entry, [] (const op_entry& a, const op_entry& b) {
    return a.op.size() > b.op.size();
  });

  litteral_operators.insert(itr, entry);
}

std::optional<op_info> lexer::operator_info(const std::string_view& op, op_fixity fixity) const {
  // один и тот же оператор может быть зарегистрирован с разными фиксностями (напр. '-' унарный И бинарный)
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

  // datetime - по приоритету ПЕРЕД операторами и числами: содержит '-'/':' и не должен
  // расщепляться зарегистрированным оператором '-'
  if (iso_datetime dt; is_datetime(str, dt)) {
    return token{token_type::datetime, s};
  }

  // если токен содержит зарегистрированный символьный оператор - возвращаем invalid, и лексер
  // в режиме identifier до-расщепит токен на op + остаток. Проверка ВЫШЕ чисел, поэтому при
  // зарегистрированном '-' токен '-123' станет 'op' + 'number', а без него - знаковым числом.
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

  // тут проверим идентификатор
  if (is_valid_identificator(str)) {
    return token{token_type::identifier, s};
  }

  // вот тут тоже должен возвращаться инвалид
  return token{token_type::invalid, s};
}

}
