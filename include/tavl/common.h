#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Базовые типы tavl, общие для всех слоёв: enum'ы token_type/event_type/error_type (генерируются
// из X-макросов TAVL_*_TYPE_LIST - новый вид добавляется ОДНОЙ строкой в макрос, to_string и
// предикаты подхватят), плюс token/event/error/source_span/iso_datetime, метаданные операторов
// (op_fixity/op_assoc/op_info) и узел AST node (+node_view в ext.h).
// ВАЖНО: порядок error_type значим - всё начиная с err_bracket_missmatch критично (error::is_critical),
// warning'и идут до этой границы.

#define TAVL_TOKEN_TYPE_LIST \
  X(invalid) \
  X(unrecognized) \
  /* types */ \
  X(null) \
  X(boolean) \
  X(identifier) \
  X(number_int) \
  X(number_uint) \
  X(number_float) \
  X(op) \
  X(datetime) \
  X(doublequote_string) \
  X(singlequote_string) \
  /* core symbols */ \
  X(comma) \
  X(newline) \
  X(open_paren) \
  X(close_paren) \
  X(open_brace) \
  X(close_brace) \
  X(open_bracket) \
  X(close_bracket) \
  /* comments */ \
  X(line_comment) \
  X(block_comment) \
  /* special */ \
  X(not_enought_data) \
  X(eof) \


#define TAVL_EVENT_TYPE_LIST \
  X(invalid) \
  /* block begin */ \
  X(object_begin) \
  X(tuple_begin) \
  X(array_begin) \
  X(row_begin) \
  X(pair_begin) \
  /* block end */ \
  X(object_end) \
  X(tuple_end) \
  X(array_end) \
  X(row_end) \
  X(pair_end) \
  /* core symbols */ \
  X(got_row_identifier) \
  X(got_row_operator) \
  X(got_token) \
  /* special */ \
  X(got_comment) \
  X(empty_row) \
  X(not_enought_data) \
  X(eof) \


// порядок важен: всё начиная с err_bracket_missmatch считается критическим
// (см. error::is_critical). Новые warning'и добавлять до этой границы,
// новые критические — после.
#define TAVL_ERROR_TYPE_LIST \
  X(no_error) \
  X(expected_identifier) \
  /* warning */ \
  X(warn_expected_identifier) \
  X(warn_object_without_value) \
  X(warn_empty_row) \
  X(warn_missing_field) \
  /* critical */ \
  X(err_bracket_missmatch) \
  X(err_misplaced_operator) \
  X(err_nesting_too_deep) \
  X(err_row_too_long) \
  X(err_output_capacity) \
  X(err_expect_token_boolean) \
  X(err_expect_token_number_uint) \
  X(err_expect_token_number_int) \
  X(err_expect_token_number_float) \
  /* deserialize */ \
  X(err_duplicate_field) \
  X(err_too_many_values) \
  X(err_string_too_long) \


// критические ошибки - это отсутствие закрывающих скобок
// остальные ошибки можно игнорировать
// ошибки будут еще при заполнении си структуры

// будут ли ошибки при превращении этого дела в строку? вряд ли
// нужно ли городить сериализатор с апихой похожей на парсер? что то я сомневаюсь

namespace tavl {

enum class token_type {
#define X(name) name,
  TAVL_TOKEN_TYPE_LIST
#undef X

  count
};

enum class event_type {
#define X(name) name,
  TAVL_EVENT_TYPE_LIST
#undef X

  count

  // ошибки + расширения
};

enum class error_type {
#define X(name) name,
  TAVL_ERROR_TYPE_LIST
#undef X

  count
};

std::string_view to_string(const enum token_type type) noexcept;
std::string_view to_string(const enum event_type type) noexcept;
std::string_view to_string(const enum error_type type) noexcept;

// Громкая проверка инвариантов / конфигурационного ввода, НЕЗАВИСИМАЯ от NDEBUG (в отличие от assert).
// Малформный ТЕКСТ так не проверяют - он идёт через error/event. TAVL_CHECK для: ошибок конфигурации
// (битый оператор в add_operator) и «этого не может быть» (недостижимые ветки). По умолчанию печатает
// в stderr и std::abort() (библиотека -fno-exceptions). Встраивающий может подменить хендлер
// (например, чтобы залогировать или - в тестах - перехватить без abort).
using check_handler = void(*)(const char* expr, const char* file, int line, const char* msg);
check_handler set_check_handler(check_handler h);   // возвращает предыдущий; nullptr -> дефолт (stderr+abort)
void fail_check(const char* expr, const char* file, int line, const char* msg);

#define TAVL_CHECK(cond, msg) ((cond) ? (void)0 : ::tavl::fail_check(#cond, __FILE__, __LINE__, (msg)))

// Жёсткие лимиты внутренних буферов (compile-time). При достижении - элемент не добавляется,
// сигналим ошибкой (вход-зависимые: err_nesting_too_deep / err_row_too_long) либо обрезаем
// (диагностики). >256 полей агрегата - ошибка КОМПИЛЯЦИИ (static_assert в deserialize).
namespace limits {
inline constexpr size_t max_nesting     = 64;    // parser::stack / ct_context::frames - глубина блоков
inline constexpr size_t max_row_tokens  = 4096;  // ast_context op_stack/rpn_output/support - токенов в строке
inline constexpr size_t max_fields      = 256;   // полей агрегата (ct_context::frame::seen - bitset)
inline constexpr size_t max_diagnostics = 256;   // ct_context::diagnostics - дальше не пишем
}

enum class lexer_mode {
  standard,

  identifier,

  doublequote_string,
  singlequote_string,

  line_comment,
  block_comment,
};

struct source_span {
  size_t offset = SIZE_MAX;
  size_t size = 0;
  size_t line = 0;
  size_t column = 0;
};

struct token {
  enum token_type type = token_type::invalid;
  source_span span;

  inline bool exists() const noexcept { return type != token_type::invalid; }
  inline bool valid() const noexcept { return exists() && type != token_type::unrecognized; }
};

struct event {
  enum event_type type = event_type::invalid;
  struct token token;
};

struct error {
  enum error_type type = error_type::no_error;
  source_span span;

  inline bool no_error() const noexcept { return type == error_type::no_error; }
  // критические ошибки (несовпадение скобок, неожиданный тип токена) -
  // повод пропустить текущую строку; остальные не прерывают разбор
  inline bool is_critical() const noexcept { return type >= error_type::err_bracket_missmatch; }
};

// перевод из и в unix timestamp
struct iso_datetime {
  int32_t y=0, m=0, d=0, hh=0, mm=0, ss=0, ms=0, tz_offset_mm=0;
  bool is_utc = false;
};

// фиксность оператора (для построения матдерева)
enum class op_fixity {
  prefix,    // унарный правоассоциативный префикс: -x, !x
  binary,    // бинарный инфикс: a + b
  postfix,   // унарный левоассоциативный постфикс: x!
};

// ассоциативность (значима для бинарных: a-b-c -> (a-b)-c слева, a=b=c -> a=(b=c) справа).
// Унарные определяются фиксностью (prefix=правая, postfix=левая).
enum class op_assoc {
  left,
  right,
};

// метаданные зарегистрированного оператора (нужны make_math_ast)
struct op_info {
  op_fixity fixity = op_fixity::binary;
  int precedence = 0;
  op_assoc assoc = op_assoc::left;
};

// --- AST: узел дерева (строится расширениями поверх потока событий, см. ext.h) ---
enum class node_type {
  invalid,

  object,
  tuple,
  array,
  row,
  pair,
  token
};

// Узел плоского AST. child_count - ФУТПРИНТ: число слотов под всех потомков в плоском массиве
// (поддерево узла занимает 1 + child_count слотов). Прямые дети идут подряд начиная со слота +1,
// шаг до следующего ребёнка = child.child_count + 1. Удобный обход - через node_view (ext.h).
struct node {
  enum node_type type = node_type::invalid;
  struct token token;   // для pair - оператор; для блоков - открывающая скобка; для token - сам токен
  size_t child_count = 0;
};

}