#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <string>
#include <vector>
#include <optional>

#include "tavl/common.h"
#include "tavl/detail.h"

// lexer - the tokenizer for the tavl format. char_storage accumulates bytes (resolve/peek by
// absolute offset, release_before discards what has been read while retaining capacity).
// lexer::lex() yields one token at a time
// from storage and returns token_type::not_enought_data when there isn't enough data; after finish()
// it tokenizes the trailing tail. Operators are registered with metadata (fixity/precedence):
// symbolic via add_operator, literal via add_litteral_operator; operator_info returns the metadata
// (a single operator may have several fixities). lexer_state - the resumable state between calls.
//
// If the accumulated sequence isn't recognized as a single token (number/date/identifier/...),
// the lexer makes a second pass in identifier mode: it splits the sequence into registered operators +
// identifiers (longest-match). If that doesn't help either - it yields token_type::unrecognized.
namespace tavl {

struct char_storage {
public:
  struct span { size_t offset = SIZE_MAX, size = 0; };
  std::string_view resolve(const span& s) const;
  char peek(const size_t offset) const;
  size_t size() const;

  span append(const std::string_view& data);
  void release_before(const size_t offset);
  bool truncate_after(size_t absolute_offset); // keep bytes before this absolute offset
  size_t buffer_size() const;   // live bytes after release_before
  size_t released_offset() const noexcept; // absolute offset of the first live byte
  std::string_view live_source() const noexcept; // view invalidated by append/release/clear

  void clear();
private:
  size_t offset = 0;
  size_t begin = 0;  // released prefix retained until append needs to compact
  std::vector<char> storage;
};

struct lexer_state {
  enum lexer_mode mode = lexer_mode::standard;
  uint32_t nesting_depth = 0;     // open block-comment levels; block comments nest
  bool escaped = false;
  bool comment_delim = false;     // char at offset-1 was already used as a /* or */ half (no reuse)
  bool finalizing = false;
  source_span token_span = {0, 0, 1, 1};
  size_t offset = 0;
  size_t line = 1;
  size_t column = 1;
  size_t division_counter = 0;
};

class lexer {
public:
  struct op_entry { std::string op; struct op_info info; };

  token lex(const char_storage& storage);
  void finish();

  // Registration copies the spelling; callers need not retain its storage.
  void add_operator(const std::string_view& op, op_info info);
  void add_litteral_operator(const std::string_view& op, op_info info);
  void clear_operators();
  void clear();

  bool finalizing() const noexcept;
  size_t token_offset() const noexcept;   // absolute offset of the current not-yet-emitted token

  std::optional<op_info> operator_info(const std::string_view& op, op_fixity fixity) const;
  detail::float_parse_result parse_float(std::string_view text, double& value) const;
private:
  lexer_state state;
  mutable detail::float_parse_workspace float_workspace;
  std::vector<op_entry> operators;
  std::vector<op_entry> litteral_operators;

  void advance(const char c);
  size_t skip_ws(const char_storage& storage);
  token make_token(const char_storage& storage, size_t local_offset = SIZE_MAX);
};

}
