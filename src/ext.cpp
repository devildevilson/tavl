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

// Skip the rest of the current row, for example after a row-level error. We count nested blocks
// until row_end at zero depth. The counter lives in parser so the skip survives not_enought_data.
// Returns row_end / not_enought_data / eof and intentionally discards attached errors.
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

// Document separator: a line-comment token whose trimmed content is exactly "//---".
bool is_document_separator(const parser& p, const token& t) {
  if (t.type != token_type::line_comment) return false;
  auto s = p.content(t.span);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s == document_separator;
}

// A plain reverse would swap children inside pairs; rebuild the reversed RPN by subtrees.
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

  // Capacity-aware mode is atomic per row: append all nodes or none. On overflow, mark output_full
  // and avoid reallocating ast_nodes.
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



// Cursor i walks ctx.op_stack (a flat event stream with row/bracket markers), appends postfix nodes
// to ctx.rpn_output, and returns the subtree footprint. Grammar: all operators are equal,
// binary, right-associative, and bind rhs to the whole remaining row. Free operands are grouped
// into node_type::row when there is more than one. Footprints are computed from counters directly.
static size_t ast_reduce_row(ast_context& ctx, size_t& i, const token& row_tok);

// Reduce a bracket group starting at *_begin: children are the rows inside, root is tuple/object/array.
// Returns footprint and consumes through the matching *_end.
static size_t ast_reduce_bracket(ast_context& ctx, size_t& i, [[maybe_unused]] const token& row_tok) {
  const auto open = ctx.op_stack[i];
  i += 1;                                                  // consume *_begin
  size_t sum = 0;                                          // child-row footprints
  while (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_begin) {
    i += 1;                                                // consume row_begin
    sum += ast_reduce_row(ctx, i, open.event.token);
    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_end) i += 1;
  }
  if (i < ctx.op_stack.size() && is_end_block(ctx.op_stack[i].event.type)) i += 1;  // consume *_end
  ctx.rpn_output.push_back(ast_context::event_args{open.event, sum});   // tuple/object/array node
  return 1 + sum;
}

static size_t ast_reduce_row(ast_context& ctx, size_t& i, const token& row_tok) {
  size_t row_sum = 0, row_n = 0;
  while (i < ctx.op_stack.size()) {
    const auto type = ctx.op_stack[i].event.type;
    if (type == event_type::row_end || is_end_block(type)) break;     // end of row/group

    size_t operand_fp;
    if (is_begin_block(type) && type != event_type::row_begin) {      // ( { [ -> subgroup
      operand_fp = ast_reduce_bracket(ctx, i, row_tok);
    } else {
      ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});  // leaf
      operand_fp = 1;
      i += 1;
    }

    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.token.type == token_type::op) {
      const auto op = ctx.op_stack[i];
      i += 1;                                                         // consume operator
      const size_t rhs_fp = ast_reduce_row(ctx, i, row_tok);          // rhs is the remaining row
      const size_t count = operand_fp + rhs_fp;
      ctx.rpn_output.push_back(ast_context::event_args{op.event, count});
      row_sum += 1 + count; row_n += 1;
      break;                                                          // remainder consumed
    }
    row_sum += operand_fp; row_n += 1;                                // free operand
  }

  if (row_n <= 1) return row_sum;   // one element (or empty): no row wrapper
  ctx.rpn_output.push_back(ast_context::event_args{event{event_type::row_begin, row_tok}, row_sum});
  return 1 + row_sum;
}

// Collect one row into ctx.op_stack (operands, operators, bracket markers, nested rows). All state
// lives in ctx, so not_enought_data is resumable. Misplaced operators are detected at any level:
// expect_operand resets at row/bracket boundaries; on error we skip through this row's row_end.
// Returns not_enought_data or the row terminator (outer row_end / eof). Shared by pair/tag AST.
static event ast_collect_row(parser& p, ast_context& ctx, bool detect_misplaced = true) {
  // For math parsing, prefix operators are valid where operands are expected; Pratt reports errors.
  event ev; error err;
  std::tie(ev, err) = p.poll_event();
  while (true) {
    if (ev.type == event_type::not_enought_data) return ev;

    if ((ev.type == event_type::row_end && ctx.nest_counter == 0) || ev.type == event_type::eof) {
      if (detect_misplaced && !ctx.had_error && !ctx.op_stack.empty() &&
          ctx.op_stack.back().event.token.type == token_type::op) {
        ctx.had_error = true;   // dangling operator at end of row
      }
      return ev;
    }

    // Hard per-row token limit: stop collecting and skip to row_end.
    if (!ctx.had_error && ctx.op_stack.size() >= limits::max_row_tokens) {
      ctx.had_error = true;
      ctx.row_too_long = true;
    }

    if (ctx.had_error) {
      // Skip mode: track only bracket depth so we stop at this row's row_end.
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
      ctx.expect_operand = false;    // a bracket group is an operand of the parent
      ctx.op_stack.push_back(ast_context::event_args{ev, 0});
    }

    else if (ev.type == event_type::row_begin) {
      if (ctx.nest_counter == 0) {
        if (!ctx.got_start) { ctx.row_start = ev.token.span; ctx.got_start = true; }  // row marker, not an operand
      } else {
        ctx.expect_operand = true;   // new nested row inside brackets
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else if (ev.type == event_type::row_end) {
      if (detect_misplaced && !ctx.op_stack.empty() && ctx.op_stack.back().event.token.type == token_type::op) {
        ctx.had_error = true;        // dangling operator in a nested row
      } else {
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else if (ev.type == event_type::empty_row || ev.type == event_type::got_comment) {
      // Not represented in AST.
    }

    else if (ev.token.type == token_type::op) {     // operator
      if (detect_misplaced && ctx.expect_operand) ctx.had_error = true; // no lhs
      else {
        ctx.expect_operand = true;
        ctx.op_stack.push_back(ast_context::event_args{ev, 0});
      }
    }

    else {                                          // operand token
      if (!ctx.got_start) { ctx.row_start = ev.token.span; ctx.got_start = true; }
      ctx.expect_operand = false;
      ctx.op_stack.push_back(ast_context::event_args{ev, 0});
    }

    std::tie(ev, err) = p.poll_event();
  }
}

// Reset per-row state after a completed row.
static void ast_reset_row(ast_context& ctx) {
  ctx.nest_counter = 0;
  ctx.expect_operand = true;
  ctx.got_start = false;
  ctx.had_error = false;
  ctx.row_too_long = false;
  ctx.output_full = false;
  // Keep bounded_output; it is a caller option across rows.
}

// Row error spanning the whole row. err_row_too_long for token-limit overflow, otherwise misplaced
// or dangling operator.
static error ast_row_error(const ast_context& ctx, const event& end_ev) {
  const size_t s = ctx.row_start.offset;
  const size_t e = end_ev.token.span.offset;
  const error_type t = ctx.row_too_long ? error_type::err_row_too_long : error_type::err_misplaced_operator;
  return error{t, source_span{s, e > s ? e - s : 0, ctx.row_start.line, ctx.row_start.column}};
}

std::tuple<event, error> make_pair_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes) {
  // Simple AST: all operators are equal, binary, right-associative, no precedence; rhs is the
  // remaining row, free operands become row, brackets become tuple/object/array.
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

// --- make_tag_ast: XML/HCL-like tags ---
// Each row is classified by its first parser event, so behavior follows block_frame::row_mode instead
// of being hardcoded here. got_row_identifier means a tag row: pair(tag, row(data)); the rhs is always
// a row, with got_row_operator or a synthetic token{} as the pair operator. Other rows are data rows.
// Data rows: a lone token is a token; k=v becomes an attribute pair, right-associative. Therefore
// objects are always tags, arrays are always data, and tuples are tags only for id=value rows.
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
    sum += ast2_reduce_row(ctx, i, open.event.token);   // tag/data by first row event
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
    ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});   // leaf
    primary_fp = 1;
    i += 1;
  }

  if (i < ctx.op_stack.size() && ctx.op_stack[i].event.token.type == token_type::op) {
    const auto op = ctx.op_stack[i];
    i += 1;
    const size_t rhs_fp = ast2_reduce_value(ctx, i, row_tok);   // right-assoc, rhs is one value
    const size_t count = primary_fp + rhs_fp;
    ctx.rpn_output.push_back(ast_context::event_args{op.event, count});   // attribute pair
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
  // Tag is the got_row_identifier event that selected this reducer.
  ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});
  i += 1;
  const size_t tag_fp = 1;

  // Tag pair operator is got_row_operator when present, otherwise synthetic token{}.
  event op_ev{event_type::got_row_operator, token{}};
  if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::got_row_operator) {
    op_ev = ctx.op_stack[i].event;
    i += 1;
  }

  // rhs is the remaining data, always wrapped in row so a tag is distinguishable from data.
  size_t row_sum = 0;
  while (i < ctx.op_stack.size()) {
    const auto type = ctx.op_stack[i].event.type;
    if (type == event_type::row_end || is_end_block(type)) break;
    row_sum += ast2_reduce_value(ctx, i, row_tok);
  }
  ctx.rpn_output.push_back(ast_context::event_args{event{event_type::row_begin, row_tok}, row_sum});
  const size_t rhs_fp = 1 + row_sum;

  const size_t count = tag_fp + rhs_fp;
  ctx.rpn_output.push_back(ast_context::event_args{op_ev, count});   // tag pair
  return 1 + count;
}

// Row dispatcher: first event got_row_identifier -> tag, otherwise data. This lets tag/data behavior
// follow parser event types (object always tag; tuple only when id=value; array never).
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

// --- make_math_ast: precedence-aware expressions ---
// Pratt / precedence-climbing over op_stack. Block types are ignored; tokens, operators, and
// brackets are what matter. We build prefix-polish pair nodes, then reverse_rpn converts them to
// the flat AST layout. Operators must be registered and are looked up by fixity. Brackets are
// uniform: comma-separated rows are arguments; one argument collapses to itself, several become the
// bracket block node. primary followed by brackets is a function-call pair with synthetic token{}.
// Binary operators are left-associative by default; prefix operators are right-associative.
static size_t math_parse_expr(parser& p, ast_context& ctx, size_t& i, int min_prec);
static size_t math_parse_bracket(parser& p, ast_context& ctx, size_t& i);

// Is op_stack[i] an operator with the requested fixity? Metadata comes from parser::operator_info.
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
  return is_begin_block(t) && t != event_type::row_begin;   // () {} [] are uniform here
}

static bool math_expr_end_at(const ast_context& ctx, size_t i) {
  if (i >= ctx.op_stack.size()) return true;
  const auto t = ctx.op_stack[i].event.type;
  return t == event_type::row_end || is_end_block(t);
}

// primary: bracket group or leaf token
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

  ctx.rpn_output.push_back(ast_context::event_args{ctx.op_stack[i].event, 0});   // leaf
  i += 1;
  return 1;
}

// postfix operators and function calls (bracket immediately after primary)
static size_t math_parse_postfix(parser& p, ast_context& ctx, size_t& i) {
  size_t fp = math_parse_primary(p, ctx, i);
  if (fp == 0) return 0;
  while (i < ctx.op_stack.size()) {
    op_info info;
    if (math_op_at(p, ctx, i, op_fixity::postfix, info)) {
      const auto op = ctx.op_stack[i].event; i += 1;
      ctx.rpn_output.push_back(ast_context::event_args{op, fp});   // unary postfix, one child
      fp = 1 + fp;
    } else if (math_bracket_at(ctx, i)) {
      const size_t args_fp = math_parse_bracket(p, ctx, i);        // function call
      const size_t count = fp + args_fp;
      ctx.rpn_output.push_back(ast_context::event_args{event{event_type::got_row_operator, token{}}, count}); // pC, token{}
      fp = 1 + count;
    } else break;
  }
  return fp;
}

// prefix unary operators, right-associative
static size_t math_parse_prefix(parser& p, ast_context& ctx, size_t& i) {
  op_info info;
  if (math_op_at(p, ctx, i, op_fixity::prefix, info)) {
    const auto op = ctx.op_stack[i].event; i += 1;
    const size_t operand_fp = math_parse_prefix(p, ctx, i);
    if (operand_fp == 0) {
      ctx.had_error = true;
      return 0;
    }
    ctx.rpn_output.push_back(ast_context::event_args{op, operand_fp});   // unary prefix, one child
    return 1 + operand_fp;
  }
  return math_parse_postfix(p, ctx, i);
}

// binary operators by precedence
static size_t math_parse_expr(parser& p, ast_context& ctx, size_t& i, int min_prec) {
  size_t lhs = math_parse_prefix(p, ctx, i);
  if (lhs == 0) return 0;
  while (true) {
    op_info info;
    if (!math_op_at(p, ctx, i, op_fixity::binary, info)) break;
    if (info.precedence < min_prec) break;
    const auto op = ctx.op_stack[i].event; i += 1;
    // Left-assoc: next level is stricter (prec+1). Right-assoc: same level.
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

// Bracket: arguments are rows. One argument collapses to itself; >1 or 0 become a group node.
static size_t math_parse_bracket(parser& p, ast_context& ctx, size_t& i) {
  const auto open = ctx.op_stack[i].event; i += 1;   // consume *_begin
  size_t sum = 0, n = 0;
  while (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_begin) {
    i += 1;                                           // consume row_begin
    sum += math_parse_expr(p, ctx, i, 0);
    if (ctx.had_error) return 0;
    n += 1;
    if (i < ctx.op_stack.size() && ctx.op_stack[i].event.type == event_type::row_end) i += 1;
  }
  if (i < ctx.op_stack.size() && is_end_block(ctx.op_stack[i].event.type)) i += 1;   // consume *_end
  if (n == 1) return sum;                            // one argument: no wrapper
  ctx.rpn_output.push_back(ast_context::event_args{open, sum});   // tuple/object/array
  return 1 + sum;
}

std::tuple<event, error> make_math_ast(parser& p, ast_context& ctx, std::vector<node>& ast_nodes) {
  const event ev = ast_collect_row(p, ctx, /*detect_misplaced=*/false);   // prefixes are valid

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

// --- node_view non-template methods declared in ext.h ---

const node& node_view::root() const {
  static const node empty_node{};   // default invalid/token{}/child_count=0
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
