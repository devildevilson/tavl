// Расширение: make_tag_ast.

#include <doctest/doctest.h>

#include "util.h"

static std::string tag_ast(tavl::parser& p, std::string_view src) {
  const auto nodes = tavl_test::build_ast(p, src, tavl::make_tag_ast);
  return tavl_test::ast_str(p, nodes);
}

TEST_CASE("tag_ast: a tag with a value via an operator") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(tag_ast(p, "name = value") == "(pair '=' (tok 'name') (row 'name' (tok 'value')))");
}

TEST_CASE("tag_ast: a tag with multiple data items (synthetic operator)") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(tag_ast(p, "widget a b") == "(pair (tok 'widget') (row 'widget' (tok 'a') (tok 'b')))");
}

TEST_CASE("tag_ast: a single token — a data row") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(tag_ast(p, "lonely") == "(tok 'lonely')");
}

TEST_CASE("tag_ast: child count via node_view") {
  tavl::parser p;
  p.add_default_operator();
  const auto nodes = tavl_test::build_ast(p, "name = value", tavl::make_tag_ast);
  const tavl::node_view root{nodes};

  CHECK(root.type() == tavl::node_type::pair);   // пара тег-значение
  CHECK(root.size() == 2);                        // тег 'name' и row(данные)
  CHECK(root.child(0).type() == tavl::node_type::token);
  const auto data = root.child(1);
  CHECK(data.type() == tavl::node_type::row);     // rhs тега ВСЕГДА row
  CHECK(data.size() == 1);                         // один элемент данных 'value'
}
