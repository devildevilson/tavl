// Extension: make_math_ast plus node_view traversal.

#include <doctest/doctest.h>

#include "util.h"

static std::string math_ast(tavl::parser& p, std::string_view src) {
  const auto nodes = tavl_test::build_ast(p, src, tavl::make_math_ast);
  return tavl_test::ast_str(p, nodes);
}

TEST_CASE("math_ast: * has higher precedence than +") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a + b * c") == "(pair '+' (tok 'a') (pair '*' (tok 'b') (tok 'c')))");
}

TEST_CASE("math_ast: left-associativity of equal precedence") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a * b + c") == "(pair '+' (pair '*' (tok 'a') (tok 'b')) (tok 'c'))");
  CHECK(math_ast(p, "a - b - c") == "(pair '-' (pair '-' (tok 'a') (tok 'b')) (tok 'c'))");
}

TEST_CASE("math_ast: brackets override precedence") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a * (b + c)") == "(pair '*' (tok 'a') (pair '+' (tok 'b') (tok 'c')))");
}

TEST_CASE("math_ast: unary prefix -> a node with a single child") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "-a") == "(pair '-' (tok 'a'))");
  CHECK(math_ast(p, "-a * b") == "(pair '*' (pair '-' (tok 'a')) (tok 'b'))");

  const auto nodes = tavl_test::build_ast(p, "-a", tavl::make_math_ast);
  const tavl::node_view root{nodes};
  CHECK(root.size() == 1);          // exactly one child: the operand
  CHECK(root.footprint() == 1);
}

TEST_CASE("math_ast: malformed expressions report err_misplaced_operator") {
  tavl::parser p;
  p.add_math_default_operators();

  for (const std::string_view src : {"a +", "= a", "a * * b", "a + (b *)", "f(a +)", "a ! b"}) {
    const auto result = tavl_test::build_ast_result(p, src, tavl::make_math_ast);
    CHECK(result.error.type == tavl::error_type::err_misplaced_operator);
    CHECK(result.nodes.empty());
  }
}

TEST_CASE("math_ast: byte-by-byte streaming yields the same AST as all at once") {
  tavl::parser whole;
  whole.add_math_default_operators();
  const auto ref = math_ast(whole, "a + b * (c + d)");

  tavl::parser stream;
  stream.add_math_default_operators();
  const auto result = tavl_test::build_ast_streamed(stream, "a + b * (c + d)", tavl::make_math_ast);
  CHECK(result.error.type == tavl::error_type::no_error);
  CHECK(tavl_test::ast_str(stream, result.nodes) == ref);
}

TEST_CASE("math_ast: right-associative assignment") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a = b = c") == "(pair '=' (tok 'a') (pair '=' (tok 'b') (tok 'c')))");
}

TEST_CASE("math_ast: custom postfix operator binds to one operand") {
  tavl::parser p;
  p.add_math_default_operators();
  p.add_operator("!", tavl::op_fixity::postfix, 17);

  CHECK(math_ast(p, "a! + b") == "(pair '+' (pair '!' (tok 'a')) (tok 'b'))");
}

TEST_CASE("math_ast: call syntax uses an empty operator pair") {
  tavl::parser p;
  p.add_math_default_operators();

  CHECK(math_ast(p, "f(a, b + c)") ==
        "(pair (tok 'f') (tuple '(' (tok 'a') (pair '+' (tok 'b') (tok 'c'))))");
}

TEST_CASE("math_ast: bounded_output — hitting ast_nodes.capacity() yields err_output_capacity") {
  tavl::parser p;
  p.add_math_default_operators();
  p.clear(); p.flush("a + b * c"); p.finish();

  tavl::event ev{}; tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);

  std::vector<tavl::node> nodes;
  nodes.reserve(2);
  const size_t cap = nodes.capacity();
  tavl::ast_context ctx;
  ctx.bounded_output = true;
  std::tie(ev, err) = tavl::make_math_ast(p, ctx, nodes);
  CHECK(err.type == tavl::error_type::err_output_capacity);
  CHECK(nodes.empty());
  CHECK(nodes.capacity() == cap);
}

TEST_CASE("math_ast: bounded_output with sufficient capacity — no error") {
  tavl::parser p;
  p.add_math_default_operators();
  p.clear(); p.flush("a + b * c"); p.finish();

  tavl::event ev{}; tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);

  std::vector<tavl::node> nodes;
  nodes.reserve(16);
  tavl::ast_context ctx;
  ctx.bounded_output = true;
  std::tie(ev, err) = tavl::make_math_ast(p, ctx, nodes);
  CHECK(err.type == tavl::error_type::no_error);
  CHECK(tavl_test::ast_str(p, nodes) == "(pair '+' (tok 'a') (pair '*' (tok 'b') (tok 'c')))");
}

TEST_CASE("node_view: AST traversal (size/footprint/is_block/for_each/child/next_child_index)") {
  tavl::parser p;
  p.add_math_default_operators();
  const auto nodes = tavl_test::build_ast(p, "a + b * c", tavl::make_math_ast);
  const tavl::node_view root{nodes};

  CHECK(root.type() == tavl::node_type::pair);   // '+'
  CHECK(root.is_block());
  CHECK(root.size() == 2);          // direct children: a and (b * c)
  CHECK(root.footprint() == 4);     // descendant slots: a(1) + (b*c)(3)

  const auto lhs = root.child(0);   // 'a'
  CHECK(lhs.type() == tavl::node_type::token);
  CHECK_FALSE(lhs.is_block());
  CHECK(lhs.size() == 0);

  const auto rhs = root.child(1);   // pair '*'
  CHECK(rhs.type() == tavl::node_type::pair);
  CHECK(rhs.size() == 2);
  CHECK(rhs.footprint() == 2);

  size_t n = 0;
  root.for_each([&](tavl::node_view) { ++n; });
  CHECK(n == 2);

  // Direct child starts at [1]; 'a' occupies one slot, so the next child starts at [2].
  CHECK(tavl::next_child_index(root.nodes, 1) == 2);
}

TEST_CASE("node_view: an empty span is safe (defaults invalid/token{})") {
  const tavl::node_view nv{};   // empty span
  CHECK(nv.empty());
  CHECK(nv.type() == tavl::node_type::invalid);
  CHECK(nv.token().type == tavl::token_type::invalid);
  CHECK(nv.footprint() == 0);
  CHECK_FALSE(nv.is_block());
  CHECK(nv.size() == 0);
  CHECK(nv.descendants().empty());
  size_t n = 0;
  nv.for_each([&](tavl::node_view) { ++n; });
  CHECK(n == 0);
}
