#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Core tavl types shared by every layer: the token_type/event_type/error_type enums (generated
// from the TAVL_*_TYPE_LIST X-macros - a new kind is added with a SINGLE line in the macro, and
// to_string and the predicates pick it up), plus token/event/error/source_span/iso_datetime, the
// operator metadata (op_fixity/op_assoc/op_info) and the AST node type (+node_view in ext.h).
// IMPORTANT: error_type ordering matters - everything from err_bracket_missmatch onward is critical
// (error::is_critical); warnings come before that boundary.

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


// Ordering matters: error::is_critical() treats err_bracket_missmatch and everything after it
// as critical. Add new warnings before that boundary and new critical errors after it.
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

// Loud invariant/configuration check, independent from NDEBUG. Malformed input text is reported
// through event/error instead; TAVL_CHECK is for invalid configuration (for example, a bad
// operator in add_operator) and unreachable internal paths. The default handler prints to stderr
// and aborts because the library is built without exceptions; embedders may replace it.
using check_handler = void(*)(const char* expr, const char* file, int line, const char* msg);
check_handler set_check_handler(check_handler h);   // returns the previous handler; nullptr restores default
void fail_check(const char* expr, const char* file, int line, const char* msg);

#define TAVL_CHECK(cond, msg) ((cond) ? (void)0 : ::tavl::fail_check(#cond, __FILE__, __LINE__, (msg)))

// Hard compile-time limits for internal buffers. On input-dependent limits we report an error
// instead of growing; diagnostics are truncated. Aggregates with too many fields fail to compile.
namespace limits {
inline constexpr size_t max_nesting     = 64;    // parser::stack / ct_context::frames block depth
inline constexpr size_t max_row_tokens  = 4096;  // ast_context op_stack/rpn_output/support tokens per row
inline constexpr size_t max_fields      = 256;   // aggregate fields tracked by ct_context::frame::seen
inline constexpr size_t max_diagnostics = 256;   // ct_context::diagnostics truncation point
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
  // Critical errors usually mean the current row/value should be skipped; warnings do not stop parsing.
  inline bool is_critical() const noexcept { return type >= error_type::err_bracket_missmatch; }
};

// Calendar token payload, converted to/from Unix milliseconds by detail helpers.
struct iso_datetime {
  int32_t y=0, m=0, d=0, hh=0, mm=0, ss=0, ms=0, tz_offset_mm=0;
  bool is_utc = false;
};

// Operator fixity used by make_math_ast.
enum class op_fixity {
  prefix,    // unary right-associative prefix: -x, !x
  binary,    // binary infix: a + b
  postfix,   // unary left-associative postfix: x!
};

// Associativity for binary operators. Unary associativity is implied by fixity.
enum class op_assoc {
  left,
  right,
};

// Metadata for a registered operator; make_math_ast reads it from the parser.
struct op_info {
  op_fixity fixity = op_fixity::binary;
  int precedence = 0;
  op_assoc assoc = op_assoc::left;
};

// --- AST node type, built by extensions over the event stream (see ext.h) ---
enum class node_type {
  invalid,

  object,
  tuple,
  array,
  row,
  pair,
  token
};

// Flat AST node. child_count is the subtree footprint: the number of descendant slots in the
// flat array. A subtree occupies 1 + child_count nodes. Direct children start at slot +1 and the
// next child is reached by child.child_count + 1. node_view in ext.h provides traversal helpers.
struct node {
  enum node_type type = node_type::invalid;
  struct token token;   // pair: operator; blocks: opening bracket; token: the token itself
  size_t child_count = 0;
};

}
