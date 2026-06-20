// Error handling and misuse checks: parser errors plus deserialize diagnostics.

#include <doctest/doctest.h>

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <optional>

#include "util.h"

using tavl::error_type;
using tavl::event_type;

namespace {
struct rgb { int r, g, b; };
struct buf { std::array<char, 4> s; };
struct typed_fields { bool b; int i; unsigned u; double f; };
struct nested_typed_fields { std::vector<int> nums; std::optional<unsigned> maybe; std::unique_ptr<double> ptr; };

bool has_diag(const tavl::ct_context& ctx, error_type t) {
  for (const auto& d : ctx.diagnostics) if (d.error.type == t) return true;
  return false;
}

int g_check_calls = 0;   // TAVL_CHECK call count, intercepted by a test handler
}

// ---------------- parser errors ----------------

TEST_CASE("error: unclosed block -> critical err_bracket_missmatch") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ a = 1");
  CHECK(tavl_test::has_error(evs, error_type::err_bracket_missmatch));
  CHECK(tavl_test::has_critical(evs));
}

TEST_CASE("error: extra closing bracket -> critical") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "a = 1 }");
  CHECK(tavl_test::has_error(evs, error_type::err_bracket_missmatch));
}

TEST_CASE("error: empty row before a comma -> warn_empty_row (not critical)") {
  tavl::parser p;
  const auto evs = tavl_test::poll_all(p, "[1, , 2]");
  CHECK(tavl_test::has_error(evs, error_type::warn_empty_row));
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("error: object without a value -> warn_object_without_value") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ key }");
  CHECK(tavl_test::has_error(evs, error_type::warn_object_without_value));
  CHECK_FALSE(tavl_test::has_critical(evs));
}

TEST_CASE("error: operator without an identifier in an object -> warn_expected_identifier") {
  tavl::parser p;
  p.add_default_operator();
  const auto evs = tavl_test::poll_all(p, "{ = 6 }");
  CHECK(tavl_test::has_error(evs, error_type::warn_expected_identifier));
}

// ---------------- struct-fill errors ----------------

TEST_CASE("error: unfilled fields -> warn_missing_field") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "r = 1", ctx);    // g and b are absent
  CHECK(has_diag(ctx, error_type::warn_missing_field));
}

TEST_CASE("error: duplicate field -> err_duplicate_field") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "r = 1\nr = 2\ng = 3\nb = 4", ctx);
  CHECK(has_diag(ctx, error_type::err_duplicate_field));
}

TEST_CASE("error: named field after positional fill is duplicate") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "1\nr = 2\ng = 3\nb = 4", ctx);
  CHECK(has_diag(ctx, error_type::err_duplicate_field));
}

TEST_CASE("error: extra positional values -> err_too_many_values") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<rgb>(p, "1\n2\n3\n4", ctx);   // 4 values for 3 fields
  CHECK(has_diag(ctx, error_type::err_too_many_values));
}

TEST_CASE("error: too-long string in a char buffer -> err_string_too_long") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<buf>(p, "s = toolong", ctx);  // 7 chars into char[4]
  CHECK(has_diag(ctx, error_type::err_string_too_long));
}

TEST_CASE("error: primitive type mismatches produce err_expect_token diagnostics") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<typed_fields>(p, "b = 1\ni = text\nu = -1\nf = text", ctx);

  CHECK(has_diag(ctx, error_type::err_expect_token_boolean));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_int));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_uint));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_float));
}

TEST_CASE("error: nested primitive type mismatches produce err_expect_token diagnostics") {
  tavl::parser p;
  p.add_default_operator();
  tavl::ct_context ctx;
  tavl_test::deserialize_all<nested_typed_fields>(p, "nums = [1, text]\nmaybe = -1\nptr = text", ctx);

  CHECK(has_diag(ctx, error_type::err_expect_token_number_int));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_uint));
  CHECK(has_diag(ctx, error_type::err_expect_token_number_float));
}

// ---------------- TAVL_CHECK: loud configuration failure ----------------

TEST_CASE("misuse: invalid operator -> TAVL_CHECK (caught by a handler instead of abort)") {
  g_check_calls = 0;
  const auto prev = tavl::set_check_handler(
      [](const char*, const char*, int, const char*) { ++g_check_calls; });

  tavl::parser p;
  p.add_operator("abc", tavl::op_fixity::binary, 1);          // not operator_chars
  CHECK(g_check_calls == 1);

  p.add_litteral_operator("123", tavl::op_fixity::binary, 1); // not a valid identifier
  CHECK(g_check_calls == 2);

  tavl::set_check_handler(prev);   // restore default handler
}

// ---------------- memory limits ----------------

TEST_CASE("limit: nesting depth -> err_nesting_too_deep (critical)") {
  tavl::parser p;
  const std::string src(tavl::limits::max_nesting + 5, '(');   // more open brackets than the limit
  const auto evs = tavl_test::poll_all(p, src);
  CHECK(tavl_test::has_error(evs, error_type::err_nesting_too_deep));
  CHECK(tavl_test::has_critical(evs));
}

TEST_CASE("limit: row length -> err_row_too_long") {
  std::string src = "[";
  for (size_t i = 0; i < tavl::limits::max_row_tokens + 100; ++i) src += "1,";
  src += "]";

  tavl::parser p;
  p.clear(); p.flush(src); p.finish();
  tavl::event ev{}; tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);

  std::vector<tavl::node> nodes;
  tavl::ast_context ctx;
  std::tie(ev, err) = tavl::make_pair_ast(p, ctx, nodes);
  CHECK(err.type == error_type::err_row_too_long);
}
