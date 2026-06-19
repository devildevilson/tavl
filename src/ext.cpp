#include "tavl/ext.h"

namespace tavl {

static bool is_begin_block(const enum event_type type) {
  return type == event_type::array_begin ||
    type == event_type::object_begin ||
    type == event_type::tuple_begin ||
    type == event_type::row_begin ||
    type == event_type::pair_begin;
}

static bool is_end_block(const enum event_type type) {
  return type == event_type::array_end ||
    type == event_type::object_end ||
    type == event_type::tuple_end ||
   // type == event_type::row_end ||
    type == event_type::pair_end;
}

// Пропускаем остаток текущей строки (например, после ошибки в ней):
// читаем события, считая вложенность блоков, пока не встретим row_end на нулевой
// вложенности. Счётчик вложенности живёт в парсере (p.ignore_depth), чтобы пережить
// возврат not_enought_data и продолжиться при следующем вызове.
// Возвращает row_end / not_enought_data / eof (ошибку при этом не отдаём).
std::tuple<event, error> ignore_to_row_end(parser& p) {
  while (true) {
    const auto [ev, err] = p.poll_event();

    switch (ev.type) {
      case event_type::row_begin:
        p.ignore_depth += 1;
        break;

      case event_type::row_end:
        if (p.ignore_depth == 0) return {ev, error{}};
        p.ignore_depth -= 1;
        break;

      case event_type::not_enought_data:
      case event_type::eof:
        return {ev, error{}};

      default: break;
    }
  }

  return {};
}

// разделитель документов: токен-комментарий, чьё содержимое (без хвостовых пробелов) == "//---"
bool is_document_separator(const parser& p, const token& t) {
  if (t.type != token_type::line_comment) return false;
  auto s = p.content(t.span);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s == document_separator;
}

// нельзя просто делать реверс иначе поменяются местами вещи внутри пары
static void reverse_rpn(ast_context& ctx, std::vector<node>& ast_nodes) {
  ctx.support.insert(ctx.support.begin(), ctx.rpn_output.begin(), ctx.rpn_output.end());
  ctx.rpn_output.clear();

  for (size_t i = 0; i < ctx.support.size(); ++i) {
    const auto& v = ctx.support[i];
    if (v.counter == 0) { ctx.op_stack.push_back(v); continue; }

    ctx.rpn_output.push_back(v);
    const size_t stack_place = ctx.op_stack.size() - v.counter;
    for (size_t j = stack_place; j < ctx.op_stack.size(); ++j) {
      ctx.rpn_output.push_back(ctx.op_stack[j]);
    }

    ctx.op_stack.resize(stack_place);
    ctx.op_stack.insert(ctx.op_stack.begin() + stack_place, ctx.rpn_output.begin(), ctx.rpn_output.end());
    ctx.rpn_output.clear();
  }

  ctx.support.clear();

  // capacity-aware: строка добавляется ЦЕЛИКОМ (узлов = op_stack.size()) либо не добавляется -
  // если не влезает в ast_nodes.capacity(), помечаем output_full и не пишем (без реаллокации)
  if (ctx.bounded_output && ast_nodes.size() + ctx.op_stack.size() > ast_nodes.capacity()) {
    ctx.output_full = true;
    ctx.op_stack.clear();
    return;
  }

  for (const auto &v : ctx.op_stack) {
    if (v.event.token.type == token_type::op || v.event.type == event_type::got_row_operator) {
      ast_nodes.emplace_back(node{node_type::pair, v.event.token, v.counter});
    }

    else if (v.event.type == event_type::tuple_begin) {
      ast_nodes.emplace_back(node{node_type::tuple, v.event.token, v.counter});
    }

    else if (v.event.type == event_type::object_begin) {
      ast_nodes.emplace_back(node{node_type::object, v.event.token, v.counter});
    }

    else if (v.event.type == event_type::array_begin) {
      ast_nodes.emplace_back(node{node_type::array, v.event.token, v.counter});
    }

    else if (v.event.type == event_type::row_begin) {
      ast_nodes.emplace_back(node{node_type::row, v.event.token, v.counter});
    }

    else {
      ast_nodes.emplace_back(node{node_type::token, v.event.token});
    }
  }

  ctx.op_stack.clear();
}



// Курсор i идёт по ctx.op_stack (плоский поток с маркерами скобок/строк), дописывая
// postfix в ctx.rpn_output и возвращая footprint узла (= слотов в плоском rpn = 1 + потомки).
// Грамматика (все операторы равны): оператор бинарный, правоассоц., без приоритета,
// rhs = ВЕСЬ остаток текущей строки (рекурсивно), lhs = операнд прямо перед ним.
// Свободные операнды (+ результаты) собираются в node_type::row, если их >1.
// footprint считаем из counter'ов (без отдельного вектора).
static size_t ast_reduce_row(ast_context& ctx, size_t& i, const token& row_tok);

// Сворачивает скобочную группу (i указывает на *_begin): дети - строки внутри, узел -
// tuple/object/array. Возвращает footprint, потребляет до парного *_end включительно.
static size_t ast_reduce_bracket(ast_context& ctx, size_t& i, [[maybe_unused]] const token& row_tok) {
  const auto open = ctx.op_stack[i];
  i += 1;                                                  // съели *_begin
  size_t sum = 0;                                          // footprint детей-строк
  while (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_begin) {
    i += 1;                                                // съели row_begin
    sum += ast_reduce_row(ctx, i, open.event.token);
    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_end) i += 1;
  }
  if (i < ctx.op_stack.size() && is_end_block(ctx.op_stack[i].event.type)) i += 1;  // съели *_end
  ctx.rpn_output.push_back(ast_context::event_args{open.event, sum});   // узел tuple/object/array
  return 1 + sum;
}

static size_t ast_reduce_row(ast_context& ctx, size_t& i, const token& row_tok) {
  size_t row_sum = 0, row_n = 0;
  while (i < ctx.op_stack.size()) {
    const auto type = ctx.op_stack[i].event.type;
    if (type == event_type::row_end || is_end_block(type)) break;     // конец строки/группы

    size_t operand_fp;
    if (is_begin_block(type) && type != event_type::row_begin) {      // ( { [ -> подгруппа
      operand_fp = ast_reduce_bracket(ctx, i, row_tok);
    } else {
      ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});  // лист
      operand_fp = 1;
      i += 1;
    }

    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.token.type == token_type::op) {
      const auto op = ctx.op_stack[i];
      i += 1;                                                         // съели оператор
      const size_t rhs_fp = ast_reduce_row(ctx, i, row_tok);          // rhs = остаток строки
      const size_t count = operand_fp + rhs_fp;
      ctx.rpn_output.push_back(ast_context::event_args{op.event, count});
      row_sum += 1 + count; row_n += 1;
      break;                                                          // остаток поглощён
    }
    row_sum += operand_fp; row_n += 1;                                // свободный операнд
  }

  if (row_n <= 1) return row_sum;   // один элемент (или пусто) - без обёртки row
  ctx.rpn_output.push_back(ast_context::event_args{event{event_type::row_begin, row_tok}, row_sum});
  return 1 + row_sum;
}

// Накопление ОДНОЙ строки в ctx.op_stack (операнды/операторы/маркеры скобок и вложенных
// строк). Всё состояние - в ctx, поэтому иммунно к not_enought_data. Детектит неуместный
// оператор (нет lhs / висящий) на ЛЮБОМ уровне: expect_operand сбрасывается на границах
// строк/скобок; при ошибке -> ctx.had_error и режим пропуска до своего row_end.
// Возвращает not_enought_data (строка не дочитана, состояние в ctx) либо терминатор строки
// (row_end вне скобок / eof). Общий для make_pair_ast и make_tag_ast (различается лишь свёртка).
static event ast_collect_row(parser& p, ast_context& ctx, bool detect_misplaced = true) {
  // detect_misplaced=false для матразбора: префиксные операторы валидны на позиции операнда,
  // поэтому проверку «неуместный оператор» отключаем, просто буферизуем (ошибки ловит Pratt).
  event ev; error err;
  std::tie(ev, err) = p.poll_event();
  while (true) {
    if (ev.type == event_type::not_enought_data) return ev;

    if ((ev.type == event_type::row_end && ctx.nest_counter == 0) || ev.type == event_type::eof) {
      if (detect_misplaced && !ctx.had_error && !ctx.op_stack.empty() &&
          ctx.op_stack.back().event.token.type == token_type::op) {
        ctx.had_error = true;   // висящий оператор в конце строки
      }
      return ev;
    }

    // жёсткий лимит токенов строки: дальше не копим - уходим в режим пропуска до row_end
    if (!ctx.had_error && ctx.op_stack.size() >= limits::max_row_tokens) {
      ctx.had_error = true;
      ctx.row_too_long = true;
    }

    if (ctx.had_error) {
      // режим пропуска: следим только за глубиной скобок, чтобы поймать наш row_end
      if (ev.type == event_type::tuple_begin || ev.type == event_type::object_begin ||
          ev.type == event_type::array_begin) ctx.nest_counter += 1;
      else if (ev.type == event_type::tuple_end || ev.type == event_type::object_end ||
               ev.type == event_type::array_end) ctx.nest_counter -= 1;
      std::tie(ev, err) = p.poll_event();
      continue;
    }

    if (ev.type == event_type::tuple_begin || ev.type == event_type::object_begin ||
        ev.type == event_type::array_begin) {
      if (!ctx.got_start) { ctx.row_start = ev.token.span; ctx.got_start = true; }
      ctx.nest_counter += 1;
      ctx.expect_operand = true;
      ctx.op_stack.push_back(ast_context::event_args{ev, 0});
    }

    else if (ev.type == event_type::tuple_end || ev.type == event_type::object_end ||
             ev.type == event_type::array_end) {
      ctx.nest_counter -= 1;
      ctx.expect_operand = false;    // скобка-группа = операнд родителя
      ctx.op_stack.push_back(ast_context::event_args{ev, 0});
    }

    else if (ev.type == event_type::row_begin) {
      if (ctx.nest_counter == 0) {
        if (!ctx.got_start) { ctx.row_start = ev.token.span; ctx.got_start = true; }  // метка строки, не операнд
      } else {
        ctx.expect_operand = true;   // новая под-строка в скобках
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else if (ev.type == event_type::row_end) {
      if (detect_misplaced && !ctx.op_stack.empty() && ctx.op_stack.back().event.token.type == token_type::op) {
        ctx.had_error = true;        // висящий оператор в под-строке
      } else {
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else if (ev.type == event_type::empty_row || ev.type == event_type::got_comment) {
      // в AST не участвуют
    }

    else if (ev.token.type == token_type::op) {     // оператор
      if (detect_misplaced && ctx.expect_operand) ctx.had_error = true; // нет lhs -> неуместный оператор
      else {
        ctx.expect_operand = true;
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else {                                          // операнд (токен)
      if (!ctx.got_start) { ctx.row_start = ev.token.span; ctx.got_start = true; }
      ctx.expect_operand = false;
      ctx.op_stack.push_back(ast_context::event_args{ev, 0});
    }

    std::tie(ev, err) = p.poll_event();
  }
}

// Сброс per-row состояния ctx (после завершения разбора строки).
static void ast_reset_row(ast_context& ctx) {
  ctx.nest_counter = 0;
  ctx.expect_operand = true;
  ctx.got_start = false;
  ctx.had_error = false;
  ctx.row_too_long = false;
  ctx.output_full = false;
  // bounded_output НЕ сбрасываем - это настройка вызывающего, живёт между строками
}

// Ошибка строки со span на весь её размах. Тип: err_row_too_long при превышении лимита токенов,
// иначе err_misplaced_operator (неуместный/висящий оператор).
static error ast_row_error(const ast_context& ctx, const event& end_ev) {
  const size_t s = ctx.row_start.offset;
  const size_t e = end_ev.token.span.offset;
  const error_type t = ctx.row_too_long ? error_type::err_row_too_long : error_type::err_misplaced_operator;
  return error{t, source_span{s, e > s ? e - s : 0, ctx.row_start.line, ctx.row_start.column}};
}

std::tuple<event, error> make_pair_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes) {
  // Простое AST: все операторы равны, бинарные, правоассоц., без приоритета; rhs = весь остаток;
  // свободные операнды -> row; скобки -> tuple/object/array. Иммунно к not_enought_data,
  // ошибки на любом уровне (см. ast_collect_row).
  const event ev = ast_collect_row(p, ctx);
  if (ev.type == event_type::not_enought_data) return {ev, error{}};

  std::tuple<event, error> result{ev, error{}};
  if (ctx.had_error) {
    result = {ev, ast_row_error(ctx, ev)};
    ctx.op_stack.clear();
  } else if (!ctx.op_stack.empty()) {
    size_t i = 0;
    ast_reduce_row(ctx, i, ctx.op_stack.front().event.token);
    ctx.op_stack.clear();
    reverse_rpn(ctx, ast_nodes);
  }
  if (ctx.output_full) result = {ev, error{error_type::err_output_capacity, ctx.row_start}};
  ast_reset_row(ctx);
  return result;
}

// --- make_tag_ast: XML/HCL-подобные теги ---
// Тег/данные для КАЖДОЙ строки решает диспетчер ast2_reduce_row ПО ТИПУ ПЕРВОГО ИВИНТА
// основного парсера (поведение следует из block_frame::type, не хардкодится тут):
//   got_row_identifier -> ТЕГ-строка: p(тег, r(данные)), rhs ВСЕГДА row (оператор = got_row_operator
//     либо синтетический token{}); иначе -> ДАННЫЕ-строка.
// ДАННЫЕ: одиночный токен -> токен; k=v -> атрибут-пара p(k, value), value = один элемент (правоассоц.).
// Отсюда: object всегда теги; array всегда данные; tuple - тег при значении (id=v), иначе данные (v).
static size_t ast2_reduce_value(ast_context& ctx, size_t& i, const token& row_tok);
static size_t ast2_reduce_data_row(ast_context& ctx, size_t& i, const token& row_tok);
static size_t ast2_reduce_tag_row(ast_context& ctx, size_t& i, const token& row_tok);
static size_t ast2_reduce_row(ast_context& ctx, size_t& i, const token& row_tok);

static size_t ast2_reduce_bracket(ast_context& ctx, size_t& i, [[maybe_unused]] const token& row_tok) {
  const auto open = ctx.op_stack[i];
  i += 1;
  size_t sum = 0;
  while (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_begin) {
    i += 1;
    sum += ast2_reduce_row(ctx, i, open.event.token);   // тег/данные - по первому ивенту строки
    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_end) i += 1;
  }
  if (i < ctx.op_stack.size() && is_end_block(ctx.op_stack[i].event.type)) i += 1;
  ctx.rpn_output.push_back(ast_context::event_args{open.event, sum});   // tuple/object/array
  return 1 + sum;
}

static size_t ast2_reduce_value(ast_context& ctx, size_t& i, const token& row_tok) {
  size_t primary_fp;
  const auto type = ctx.op_stack[i].event.type;
  if (is_begin_block(type) && type != event_type::row_begin) {
    primary_fp = ast2_reduce_bracket(ctx, i, row_tok);
  } else {
    ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});   // лист
    primary_fp = 1;
    i += 1;
  }

  if (i < ctx.op_stack.size() && ctx.op_stack[i].event.token.type == token_type::op) {
    const auto op = ctx.op_stack[i];
    i += 1;
    const size_t rhs_fp = ast2_reduce_value(ctx, i, row_tok);   // правоассоц., rhs = один элемент
    const size_t count = primary_fp + rhs_fp;
    ctx.rpn_output.push_back(ast_context::event_args{op.event, count});   // атрибут-пара
    return 1 + count;
  }
  return primary_fp;
}

static size_t ast2_reduce_data_row(ast_context& ctx, size_t& i, const token& row_tok) {
  size_t row_sum = 0, row_n = 0;
  while (i < ctx.op_stack.size()) {
    const auto type = ctx.op_stack[i].event.type;
    if (type == event_type::row_end || is_end_block(type)) break;
    row_sum += ast2_reduce_value(ctx, i, row_tok);
    row_n += 1;
  }
  if (row_n <= 1) return row_sum;
  ctx.rpn_output.push_back(ast_context::event_args{event{event_type::row_begin, row_tok}, row_sum});
  return 1 + row_sum;
}

static size_t ast2_reduce_tag_row(ast_context& ctx, size_t& i, const token& row_tok) {
  // тег = got_row_identifier (диспетчер вызвал нас именно по этому ивенту)
  ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});
  i += 1;
  const size_t tag_fp = 1;

  // оператор пары тега = got_row_operator (если есть), иначе синтетический token{}
  event op_ev{event_type::got_row_operator, token{}};
  if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::got_row_operator) {
    op_ev = ctx.op_stack[i].event;
    i += 1;
  }

  // rhs = данные остатка, ВСЕГДА обёрнуты в row (так ТЕГ отличим от ДАННЫХ)
  size_t row_sum = 0;
  while (i < ctx.op_stack.size()) {
    const auto type = ctx.op_stack[i].event.type;
    if (type == event_type::row_end || is_end_block(type)) break;
    row_sum += ast2_reduce_value(ctx, i, row_tok);
  }
  ctx.rpn_output.push_back(ast_context::event_args{event{event_type::row_begin, row_tok}, row_sum});
  const size_t rhs_fp = 1 + row_sum;

  const size_t count = tag_fp + rhs_fp;
  ctx.rpn_output.push_back(ast_context::event_args{op_ev, count});   // пара тега
  return 1 + count;
}

// Диспетчер строки: первый ивент got_row_identifier -> ТЕГ, иначе -> ДАННЫЕ.
// Так тег/данные следуют из block_frame::type через типы ивентов основного парсера
// (object всегда got_row_identifier; tuple - только при наличии значения; array - никогда).
static size_t ast2_reduce_row(ast_context& ctx, size_t& i, const token& row_tok) {
  if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::got_row_identifier)
    return ast2_reduce_tag_row(ctx, i, row_tok);
  return ast2_reduce_data_row(ctx, i, row_tok);
}

std::tuple<event, error> make_tag_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes) {
  const event ev = ast_collect_row(p, ctx);
  if (ev.type == event_type::not_enought_data) return {ev, error{}};

  std::tuple<event, error> result{ev, error{}};
  if (ctx.had_error) {
    result = {ev, ast_row_error(ctx, ev)};
    ctx.op_stack.clear();
  } else if (!ctx.op_stack.empty()) {
    size_t i = 0;
    ast2_reduce_row(ctx, i, ctx.op_stack.front().event.token);
    ctx.op_stack.clear();
    reverse_rpn(ctx, ast_nodes);
  }
  if (ctx.output_full) result = {ev, error{error_type::err_output_capacity, ctx.row_start}};
  ast_reset_row(ctx);
  return result;
}

// --- make_math_ast: матвыражения с приоритетом ---
// Pratt/precedence-climbing над буфером op_stack (block_type игнорируем - важны токены, операторы,
// скобки). Дерево = прямая польская p OP (lhs, rhs) -> reverse_rpn. Операторы только
// зарегистрированные, по фиксности (operator_info(str, fixity)). Скобки единообразны: содержимое =
// аргументы через запятую, 1 -> сам аргумент, >1 -> tuple/object/array. Токен/группа перед скобкой
// -> вызов функции pC(func, args) (узел-пара с token{}). Унарный узел = пара с одним ребёнком.
// Бинарные левоассоциативны (next_min = prec+1), префиксные правоассоциативны.
static size_t math_parse_expr(parser& p, ast_context& ctx, size_t& i, int min_prec);
static size_t math_parse_bracket(parser& p, ast_context& ctx, size_t& i);

// оператор ли op_stack[i] с данной фиксностью? (метаданные из parser::operator_info)
static bool math_op_at(const parser& p, const ast_context& ctx, size_t i, op_fixity fixity, op_info& out) {
  if (i >= ctx.op_stack.size()) return false;
  const auto& tok = ctx.op_stack[i].event.token;
  if (tok.type != token_type::op) return false;
  const auto info = p.operator_info(p.content(tok.span), fixity);
  if (!info) return false;
  out = *info;
  return true;
}

static bool math_bracket_at(const ast_context& ctx, size_t i) {
  if (i >= ctx.op_stack.size()) return false;
  const auto t = ctx.op_stack[i].event.type;
  return is_begin_block(t) && t != event_type::row_begin;   // () {} [] - единообразно
}

static bool math_expr_end_at(const ast_context& ctx, size_t i) {
  if (i >= ctx.op_stack.size()) return true;
  const auto t = ctx.op_stack[i].event.type;
  return t == event_type::row_end || is_end_block(t);
}

// primary: группа в скобках либо лист-токен
static size_t math_parse_primary(parser& p, ast_context& ctx, size_t& i) {
  if (math_expr_end_at(ctx, i)) {
    ctx.had_error = true;
    return 0;
  }
  if (math_bracket_at(ctx, i)) return math_parse_bracket(p, ctx, i);

  if (ctx.op_stack[i].event.token.type == token_type::op) {
    ctx.had_error = true;
    return 0;
  }

  ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});   // лист
  i += 1;
  return 1;
}

// постфикс + вызов функции (скобка сразу после primary)
static size_t math_parse_postfix(parser& p, ast_context& ctx, size_t& i) {
  size_t fp = math_parse_primary(p, ctx, i);
  if (fp == 0) return 0;
  while (i < ctx.op_stack.size()) {
    op_info info;
    if (math_op_at(p, ctx, i, op_fixity::postfix, info)) {
      const auto op = ctx.op_stack[i].event; i += 1;
      ctx.rpn_output.push_back(ast_context::event_args{op, fp});   // унарный постфикс: 1 ребёнок
      fp = 1 + fp;
    } else if (math_bracket_at(ctx, i)) {
      const size_t args_fp = math_parse_bracket(p, ctx, i);        // вызов функции
      const size_t count = fp + args_fp;
      ctx.rpn_output.push_back(ast_context::event_args{event{event_type::got_row_operator, token{}}, count}); // pC, token{}
      fp = 1 + count;
    } else break;
  }
  return fp;
}

// префиксные унарные (правоассоц.)
static size_t math_parse_prefix(parser& p, ast_context& ctx, size_t& i) {
  op_info info;
  if (math_op_at(p, ctx, i, op_fixity::prefix, info)) {
    const auto op = ctx.op_stack[i].event; i += 1;
    const size_t operand_fp = math_parse_prefix(p, ctx, i);
    if (operand_fp == 0) {
      ctx.had_error = true;
      return 0;
    }
    ctx.rpn_output.push_back(ast_context::event_args{op, operand_fp});   // унарный префикс: 1 ребёнок
    return 1 + operand_fp;
  }
  return math_parse_postfix(p, ctx, i);
}

// бинарные по приоритету (левоассоц.)
static size_t math_parse_expr(parser& p, ast_context& ctx, size_t& i, int min_prec) {
  size_t lhs = math_parse_prefix(p, ctx, i);
  if (lhs == 0) return 0;
  while (true) {
    op_info info;
    if (!math_op_at(p, ctx, i, op_fixity::binary, info)) break;
    if (info.precedence < min_prec) break;
    const auto op = ctx.op_stack[i].event; i += 1;
    // левоассоц.: следующий уровень строже (prec+1); правоассоц.: тот же уровень (prec)
    const int next_min = (info.assoc == op_assoc::right) ? info.precedence : info.precedence + 1;
    const size_t rhs = math_parse_expr(p, ctx, i, next_min);
    if (rhs == 0) {
      ctx.had_error = true;
      return 0;
    }
    const size_t count = lhs + rhs;
    ctx.rpn_output.push_back(ast_context::event_args{op, count});
    lhs = 1 + count;
  }
  return lhs;
}

// скобка: аргументы через запятую (row_begin/row_end), 1 -> сам аргумент, >1/0 -> узел группы
static size_t math_parse_bracket(parser& p, ast_context& ctx, size_t& i) {
  const auto open = ctx.op_stack[i].event; i += 1;   // съели *_begin
  size_t sum = 0, n = 0;
  while (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_begin) {
    i += 1;                                           // съели row_begin
    sum += math_parse_expr(p, ctx, i, 0);
    if (ctx.had_error) return 0;
    n += 1;
    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_end) i += 1;
  }
  if (i < ctx.op_stack.size() && is_end_block(ctx.op_stack[i].event.type)) i += 1;   // съели *_end
  if (n == 1) return sum;                            // один аргумент - без обёртки
  ctx.rpn_output.push_back(ast_context::event_args{open, sum});   // tuple/object/array
  return 1 + sum;
}

std::tuple<event, error> make_math_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes) {
  const event ev = ast_collect_row(p, ctx, /*detect_misplaced=*/false);   // префиксы валидны

  if (ev.type == event_type::not_enought_data) return {ev, error{}};

  std::tuple<event, error> result{ev, error{}};
  if (ctx.had_error) {
    result = {ev, ast_row_error(ctx, ev)};
    ctx.op_stack.clear();
  } else if (!ctx.op_stack.empty()) {
    size_t i = 0;
    math_parse_expr(p, ctx, i, 0);
    if (!ctx.had_error && i != ctx.op_stack.size()) ctx.had_error = true;
    if (ctx.had_error) {
      result = {ev, ast_row_error(ctx, ev)};
      ctx.op_stack.clear();
      ctx.rpn_output.clear();
    } else {
      ctx.op_stack.clear();
      reverse_rpn(ctx, ast_nodes);
    }
  }
  if (ctx.output_full) result = {ev, error{error_type::err_output_capacity, ctx.row_start}};
  ast_reset_row(ctx);
  return result;
}

// --- node_view: нешаблонные методы (объявления в ext.h) ---

const node& node_view::root() const {
  static const node empty_node{};   // дефолт: type=invalid, token{}, child_count=0
  return nodes.empty() ? empty_node : nodes[0];
}

node_type node_view::type() const { return root().type; }
const struct token& node_view::token() const { return root().token; }
size_t node_view::footprint() const { return root().child_count; }
bool node_view::empty() const { return nodes.empty(); }
bool node_view::is_block() const { return root().child_count != 0; }

std::span<const node> node_view::descendants() const {
  if (nodes.empty()) return {};
  return nodes.subspan(1, nodes[0].child_count);
}

node_view node_view::iterator::operator*() const {
  return node_view{nodes.subspan(i, nodes[i].child_count + 1)};
}
node_view::iterator& node_view::iterator::operator++() {
  i += nodes[i].child_count + 1;
  return *this;
}
bool node_view::iterator::operator==(const iterator& o) const { return i == o.i; }
bool node_view::iterator::operator!=(const iterator& o) const { return i != o.i; }

node_view::iterator node_view::begin() const {
  return {nodes, nodes.empty() ? size_t{0} : size_t{1}};
}
node_view::iterator node_view::end() const {
  return {nodes, nodes.empty() ? size_t{0} : nodes[0].child_count + 1};
}

size_t node_view::size() const {
  size_t n = 0;
  for (auto c = begin(), e = end(); c != e; ++c) ++n;
  return n;
}

node_view node_view::child(size_t k) const {
  size_t j = 0;
  for (node_view c : *this) { if (j++ == k) return c; }
  return node_view{};
}

size_t next_child_index(std::span<const node> sub, size_t index) {
  return index + sub[index].child_count + 1;
}

}
