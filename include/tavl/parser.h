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

// parser - parses a byte stream into tavl-format events.
// Input is fed in chunks via flush(); the parser passes the bytes to the lexer, reads tokens and
// checks them against the format. finish() signals end of input - an eof event follows it.
// Events with their accompanying errors are read through poll_event(); errors do NOT interrupt
// parsing (criticality - error::is_critical). Operators are registered with add_operator /
// add_litteral_operator (or in bundles via add_*_default_operators). A block's row parse mode
// (parse_mode) by default follows the bracket type; override_next_block_modes replaces it, and the
// override is inherited by nested blocks. to_* convert a token into a C++ value. block_type /
// parse_mode / block_modes / block_frame are helper types of the block stack. emit/close_*/open_block
// are the internal steps of poll_event.
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
  event peek();             // non-consuming lookahead

  // --- releasing consumed input for streaming ---
  // Lower bound of still-needed bytes: current lexer token start and the oldest queued event.
  // Does not account for deserialize-level lookahead; call tavl::release_consumed(p, ctx) there.
  size_t consumed_offset() const;
  void release_before(size_t offset);   // after release, content() for discarded spans is empty

  // Block modes are inherited by nested blocks.
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
  std::optional<iso_datetime> to_datetime(const token& t) const;

  void fill_events();       // fill the event queue with at least one event when possible
  void emit(const event& ev, error err = error{});
  source_span row_span(const block_frame& f, const token& fallback) const;
  void close_row(const token& end_tok);
  void close_unmatched(block_type target, const token& closer);
  void close_block(block_type target, event_type end_ev, const token& closer);
  void open_block(const token& c, block_type bt, event_type begin_ev);
};

}
