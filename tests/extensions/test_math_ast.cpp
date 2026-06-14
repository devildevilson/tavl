// Расширение: make_math_ast + обход node_view.

#include <doctest/doctest.h>

#include "util.h"

static std::string math_ast(tavl::parser& p, std::string_view src) {
  const auto nodes = tavl_test::build_ast(p, src, tavl::make_math_ast);
  return tavl_test::ast_str(p, nodes);
}

TEST_CASE("math_ast: приоритет * выше +") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a + b * c") == "(pair '+' (tok 'a') (pair '*' (tok 'b') (tok 'c')))");
}

TEST_CASE("math_ast: левоассоциативность одинакового приоритета") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a * b + c") == "(pair '+' (pair '*' (tok 'a') (tok 'b')) (tok 'c'))");
}

TEST_CASE("math_ast: скобки переопределяют приоритет") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "a * (b + c)") == "(pair '*' (tok 'a') (pair '+' (tok 'b') (tok 'c')))");
}

TEST_CASE("math_ast: унарный префикс -> узел с одним ребёнком") {
  tavl::parser p;
  p.add_math_default_operators();
  CHECK(math_ast(p, "-a") == "(pair '-' (tok 'a'))");

  const auto nodes = tavl_test::build_ast(p, "-a", tavl::make_math_ast);
  const tavl::node_view root{nodes};
  CHECK(root.size() == 1);          // ровно один ребёнок (операнд)
  CHECK(root.footprint() == 1);
}

TEST_CASE("node_view: обход AST (size/footprint/is_block/for_each/child/next_child_index)") {
  tavl::parser p;
  p.add_math_default_operators();
  const auto nodes = tavl_test::build_ast(p, "a + b * c", tavl::make_math_ast);
  const tavl::node_view root{nodes};

  CHECK(root.type() == tavl::node_type::pair);   // '+'
  CHECK(root.is_block());
  CHECK(root.size() == 2);          // прямых детей: a и (b * c)
  CHECK(root.footprint() == 4);     // слотов под потомков: a(1) + (b*c)(3)

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

  // прямой ребёнок начинается в [1]; 'a' занимает 1 слот -> следующий ребёнок в [2]
  CHECK(tavl::next_child_index(root.nodes, 1) == 2);
}

TEST_CASE("node_view: пустой span безопасен (дефолты invalid/token{})") {
  const tavl::node_view nv{};   // пустой span
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
