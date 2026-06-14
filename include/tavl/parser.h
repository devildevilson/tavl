#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <optional>
#include <tuple>

#include "tavl/common.h"
#include "tavl/lexer.h"

// parser - разбор потока байтов в события формата tavl.
// Вход подаётся кусками через flush(); парсер передаёт байты лексеру, читает токены и проверяет
// их на соответствие формату. finish() сообщает о конце ввода - после него придёт событие eof.
// События с сопутствующими ошибками читаются через poll_event(); ошибки НЕ прерывают разбор
// (критичность - error::is_critical). Операторы регистрируются add_operator / add_litteral_operator
// (или наборами add_*_default_operators). Режим разбора строк блока (parse_mode) по умолчанию следует
// из типа скобки; override_next_block_modes подменяет его, и подмена наследуется вложенными блоками.
// to_* преобразуют токен в C++ значение. block_type / parse_mode / block_modes / block_frame -
// вспомогательные типы стека блоков. emit/close_*/open_block - внутренние шаги poll_event.
namespace tavl {

enum class block_type {
  root,
  object,
  tuple,
  array,
};

enum class parse_mode {
  object_like,
  data_driven,
  tuple_like
};

struct block_modes {
  bool active = false;
  parse_mode object = parse_mode::object_like;
  parse_mode tuple  = parse_mode::tuple_like;
  parse_mode array  = parse_mode::data_driven;
};

struct block_frame {
  enum block_type type = block_type::root;
  uint32_t nesting_depth = 0;
  enum parse_mode row_mode = parse_mode::tuple_like;
  block_modes modes;
  token open_token;
  token identifier;
  token op;
  token last_value;

  inline void new_row() { identifier = token(); op = token(); last_value = token(); }
  inline bool has_identifier() const { return identifier.exists(); }
  inline bool has_value() const { return last_value.exists(); }
  inline bool has_row() const { return has_identifier() || op.exists() || has_value(); }
};

// наверное было бы неплохо игнорировать \n как токен конца строки - отдельная настройка
struct parser {
  struct pending_event {
    struct event event;
    struct error error;
  };

  char_storage storage;
  struct lexer lexer;
  std::vector<block_frame> stack;
  std::vector<pending_event> events;

  size_t ignore_depth = 0;
  block_modes pending_modes;

  parser() noexcept;

  void flush(const std::string_view& data);
  void finish();

  std::tuple<event, error> poll_event();
  event peek();             // не-снимающий lookahead: следующее событие без снятия с очереди

  // --- освобождение прочитанного ввода (стриминг) ---
  // нижняя граница ещё нужных байт: начало текущего токена лексера и старейшее событие в очереди.
  // НЕ учитывает lookahead уровня deserialize - там зови tavl::release_consumed(p, ctx).
  size_t consumed_offset() const;
  void release_before(size_t offset);   // освободить байты ввода до offset (после - content() для них пуст)

  // режимы блоков наследуются вложенными блоками
  void override_next_block_modes(parse_mode object, parse_mode tuple, parse_mode array);
  void clear_pending_block_modes();

  void add_default_operator();
  void add_additional_default_operators();
  void add_math_default_operators();
  void add_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc = op_assoc::left);
  void add_litteral_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc = op_assoc::left);
  void clear_operators();

  std::optional<op_info> operator_info(const std::string_view& op, op_fixity fixity) const;

  void clear() noexcept;

  std::string_view content(const source_span& span) const;

  std::string to_string(const token& t) const;
  std::optional<bool> to_boolean(const token& t) const;
  std::optional<int64_t> to_int(const token& t) const;
  std::optional<uint64_t> to_uint(const token& t) const;
  std::optional<double> to_float(const token& t) const;
  // дата? пусть это будет тот объект, пусть пользователь распарсит более аккуратно
  std::optional<iso_datetime> to_datetime(const token& t) const;

  void fill_events();       // наполнить очередь events хотя бы одним событием
  void emit(const event& ev, error err = error{});
  source_span row_span(const block_frame& f, const token& fallback) const;
  void close_row(const token& end_tok);
  void close_unmatched(block_type target, const token& closer);
  void close_block(block_type target, event_type end_ev, const token& closer);
  void open_block(const token& c, block_type bt, event_type begin_ev);
};

}
