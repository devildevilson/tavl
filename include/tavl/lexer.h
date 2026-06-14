#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include <optional>

#include "tavl/common.h"

// lexer - токенизатор формата tavl. char_storage накапливает байты (resolve/peek по абсолютному
// смещению, release_before освобождает прочитанное). lexer::lex() выдаёт по одному токену из storage
// и возвращает token_type::not_enought_data, если данных не хватает; после finish() дотокенизирует
// хвост. Операторы регистрируются с метаданными (фиксность/приоритет): символьные add_operator,
// литеральные add_litteral_operator; operator_info отдаёт метаданные (один оператор может иметь
// несколько фиксностей). lexer_state - резюмируемое состояние между вызовами.
//
// Если накопленная последовательность не распознаётся как один токен (число/дата/идентификатор/...),
// лексер делает второй заход в режиме identifier: расщепляет её на зарегистрированные операторы +
// идентификаторы (longest-match). Если и это не помогло - выдаёт token_type::unrecognized.
namespace tavl {

struct char_storage {
public:
  struct span { size_t offset = SIZE_MAX, size = 0; };
  std::string_view resolve(const span& s) const;
  char peek(const size_t offset) const;
  size_t size() const;

  span append(const std::string_view& data);
  void release_before(const size_t offset);
  size_t buffer_size() const;   // живых байт в буфере (после release_before уменьшается)

  void clear();
private:
  size_t offset = 0;
  std::vector<char> storage;
};

struct lexer_state {
  enum lexer_mode mode = lexer_mode::standard;
  uint32_t nesting_depth = 0;
  bool escaped = false;
  bool finalizing = false;
  source_span token_span = {0, 0, 1, 1};
  size_t offset = 0;
  size_t line = 1;
  size_t column = 1;
  size_t division_counter = 0;
};

class lexer {
public:
  struct op_entry { std::string_view op; struct op_info info; };

  token lex(const char_storage& storage);
  void finish();

  void add_operator(const std::string_view& op, op_info info);
  void add_litteral_operator(const std::string_view& op, op_info info);
  void clear_operators();
  void clear();

  bool finalizing() const noexcept;
  size_t token_offset() const noexcept;   // абсолютное смещение начала текущего (ещё не выданного) токена

  std::optional<op_info> operator_info(const std::string_view& op, op_fixity fixity) const;
private:
  lexer_state state;
  std::vector<op_entry> operators;
  std::vector<op_entry> litteral_operators;

  void advance(const char c);
  size_t skip_ws(const char_storage& storage);
  token make_token(const char_storage& storage, size_t local_offset = SIZE_MAX);
};

}
