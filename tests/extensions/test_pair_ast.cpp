// Расширение: make_pair_ast.

#include <doctest/doctest.h>

#include "util.h"

static std::string pair_ast(tavl::parser& p, std::string_view src) {
  const auto nodes = tavl_test::build_ast(p, src, tavl::make_pair_ast);
  return tavl_test::ast_str(p, nodes);
}

TEST_CASE("pair_ast: a single pair") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(pair_ast(p, "a = b") == "(pair '=' (tok 'a') (tok 'b'))");
}

TEST_CASE("pair_ast: right-associativity (rhs = the entire remainder)") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(pair_ast(p, "a = b = c") == "(pair '=' (tok 'a') (pair '=' (tok 'b') (tok 'c')))");
}

TEST_CASE("pair_ast: free operands -> row") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(pair_ast(p, "a b") == "(row 'a' (tok 'a') (tok 'b'))");
}

TEST_CASE("pair_ast: a bracket group as an operand") {
  tavl::parser p;
  p.add_default_operator();
  CHECK(pair_ast(p, "a = (b, c)") == "(pair '=' (tok 'a') (tuple '(' (tok 'b') (tok 'c')))");
}

TEST_CASE("pair_ast: child count via node_view") {
  tavl::parser p;
  p.add_default_operator();
  const auto nodes = tavl_test::build_ast(p, "a = (b, c)", tavl::make_pair_ast);
  const tavl::node_view root{nodes};

  CHECK(root.type() == tavl::node_type::pair);
  CHECK(root.size() == 2);                       // lhs 'a' и группа (b, c)
  CHECK(root.child(0).type() == tavl::node_type::token);
  const auto grp = root.child(1);
  CHECK(grp.type() == tavl::node_type::tuple);
  CHECK(grp.size() == 2);                         // b, c
  CHECK(grp.is_block());
}

// прокрутить парсер до начала строки (как делает build_ast), затем отдать управление вызывающему
static void to_row_begin(tavl::parser& p, std::string_view src) {
  p.clear(); p.flush(src); p.finish();
  tavl::event ev{}; tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);
}

TEST_CASE("pair_ast: bounded_output — hitting ast_nodes.capacity() yields err_output_capacity (atomically)") {
  tavl::parser p;
  p.add_default_operator();
  to_row_begin(p, "a = (b, c, d, e)");          // дерево из ~7 узлов

  std::vector<tavl::node> nodes;
  nodes.reserve(2);                              // заведомо мало
  const size_t cap = nodes.capacity();
  tavl::ast_context ctx;
  ctx.bounded_output = true;
  const auto [ev, err] = tavl::make_pair_ast(p, ctx, nodes);
  CHECK(err.type == tavl::error_type::err_output_capacity);
  CHECK(nodes.empty());                          // строка не добавлена целиком (атомарно)
  CHECK(nodes.capacity() == cap);                // без реаллокации
}

TEST_CASE("pair_ast: bounded_output with sufficient capacity — no error, tree is correct") {
  tavl::parser p;
  p.add_default_operator();
  to_row_begin(p, "a = (b, c)");

  std::vector<tavl::node> nodes;
  nodes.reserve(64);
  tavl::ast_context ctx;
  ctx.bounded_output = true;
  const auto [ev, err] = tavl::make_pair_ast(p, ctx, nodes);
  CHECK(err.type == tavl::error_type::no_error);
  CHECK(tavl_test::ast_str(p, nodes) == "(pair '=' (tok 'a') (tuple '(' (tok 'b') (tok 'c')))");
}
