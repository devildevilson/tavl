#pragma once

#include <cstddef>
#include <vector>
#include <tuple>
#include <span>

#include "tavl/common.h"
#include "tavl/parser.h"

// Расширения поверх потока событий парсера. Три построителя AST (плоский tavl::node):
//   make_pair_ast - все операторы равны, бинарны, правоассоциативны; свободные операнды -> row;
//                   скобки -> tuple/object/array.
//   make_tag_ast  - XML/HCL-подобные теги: идентификатор строки = тег, данные после оператора =
//                   атрибуты (k=v -> пара атрибут-значение).
//   make_math_ast - матвыражения с приоритетом/фиксностью/ассоциативностью операторов (Pratt).
// Каждый построитель разбирает ОДНУ строку за вызов и иммунен к not_enought_data: всё состояние
// текущей строки в ast_context, на нехватке данных возвращаемся и продолжаем при следующем вызове.
// ignore_to_row_end пропускает остаток строки (для отдачи строки стороннему разбору).
//
// node_view - обход построенного дерева: представление поддерева как span, где nodes[0] - корень,
// дальше плоско его потомки. Прямой ребёнок начинается со слота +1, шаг до следующего = footprint+1
// (child_count корня = футпринт, число слотов под всех потомков). Пустой span безопасен:
// root() -> дефолтный node{} (type=invalid, token{}), size()=0.
namespace tavl {

struct ast_context {
  struct event_args { struct event event; size_t counter = 0; };

  // состояние разбора ТЕКУЩЕЙ строки - сбрасывается по её завершении
  size_t nest_counter = 0;
  bool expect_operand = true;
  bool got_start = false;
  bool had_error = false;
  bool row_too_long = false;     // op_stack уперся в limits::max_row_tokens
  bool bounded_output = false;   // capacity-aware: уважать ast_nodes.capacity() (выставляет вызывающий)
  bool output_full = false;      // строка не влезла в ast_nodes.capacity() -> err_output_capacity
  source_span row_start{};

  std::vector<event_args> op_stack;
  std::vector<event_args> rpn_output;
  std::vector<event_args> support;
};

std::tuple<event, error> ignore_to_row_end(parser& p);

// Разделитель документов в одном потоке: строка-комментарий ровно "//---" (визуальный, как YAML ---,
// но это КОММЕНТ - поэтому "//---" внутри строки не спутается, и сам по себе он безвреден для формата).
// is_document_separator проверяет токен события got_comment; при true - конец документа: вызвать finish(),
// дочитать события до eof, затем clear() и продолжить кормить остаток потока новому документу.
inline constexpr std::string_view document_separator = "//---";
bool is_document_separator(const parser& p, const token& t);

std::tuple<event, error> make_pair_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes);
std::tuple<event, error> make_tag_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes);
std::tuple<event, error> make_math_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes);

struct node_view {
  std::span<const node> nodes;   // nodes[0] - корень поддерева; nodes[1..] - потомки (плоско)

  const node& root() const;
  node_type type() const;
  const struct token& token() const;
  size_t footprint() const;
  bool empty() const;
  bool is_block() const;
  std::span<const node> descendants() const;

  struct iterator {
    std::span<const node> nodes;
    size_t i = 0;
    node_view operator*() const;
    iterator& operator++();
    bool operator==(const iterator& o) const;
    bool operator!=(const iterator& o) const;
  };
  iterator begin() const;
  iterator end() const;

  size_t size() const;                // число ПРЯМЫХ детей (не футпринт)
  node_view child(size_t k) const;

  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (node_view c : *this) fn(c);
  }
};

size_t next_child_index(std::span<const node> sub, size_t index);

}
