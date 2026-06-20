#pragma once

// Test utilities over the streaming tavl API: event polling, full/byte-by-byte deserialize,
// round-trip helpers, AST building, and AST rendering for CHECK comparisons.

#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <span>

#include "tavl/tavl.h"

namespace tavl_test {

struct ev_err {
  tavl::event event;
  tavl::error error;
};

// Flush all of src, finish, and collect every (event, error) pair through eof.
inline std::vector<ev_err> poll_all(tavl::parser& p, std::string_view src) {
  p.clear();
  p.flush(src);
  p.finish();

  std::vector<ev_err> out;
  while (true) {
    auto [ev, err] = p.poll_event();
    out.push_back({ev, err});
    if (ev.type == tavl::event_type::eof) break;
  }
  return out;
}

inline bool has_critical(const std::vector<ev_err>& evs) {
  for (const auto& e : evs) if (e.error.is_critical()) return true;
  return false;
}

inline bool has_error(const std::vector<ev_err>& evs, tavl::error_type t) {
  for (const auto& e : evs) if (e.error.type == t) return true;
  return false;
}

inline size_t count_event(const std::vector<ev_err>& evs, tavl::event_type t) {
  size_t n = 0;
  for (const auto& e : evs) if (e.event.type == t) n += 1;
  return n;
}

// Token types from all got_token events, used to test lexer behavior through the event stream.
inline std::vector<tavl::token_type> got_token_types(const std::vector<ev_err>& evs) {
  std::vector<tavl::token_type> out;
  for (const auto& e : evs)
    if (e.event.type == tavl::event_type::got_token) out.push_back(e.event.token.type);
  return out;
}

// --- deserialize ---

// Deserialize after a full input flush.
template <typename T>
inline T deserialize_all(tavl::parser& p, std::string_view src, tavl::ct_context& ctx) {
  p.clear();
  p.flush(src);
  p.finish();
  T val{};
  tavl::deserialize(p, ctx, val);
  return val;
}

template <typename T>
inline T deserialize_all(tavl::parser& p, std::string_view src) {
  tavl::ct_context ctx;
  return deserialize_all<T>(p, src, ctx);
}

// Deserialize in chunk-sized pieces; verifies resume via stalled -> flush more -> continue.
template <typename T>
inline T deserialize_streamed(tavl::parser& p, std::string_view src, size_t chunk = 1) {
  p.clear();
  tavl::ct_context ctx;
  T val{};
  size_t i = 0;
  do {
    if (i < src.size()) {
      p.flush(src.substr(i, chunk));
      i += chunk;
      if (i >= src.size()) p.finish();
    } else {
      p.finish();
    }
    tavl::deserialize(p, ctx, val);
  } while (ctx.stalled);
  return val;
}

// Round-trip: value -> text via serialize -> value via deserialize.
template <auto Opts = tavl::sopts{}, typename T>
inline T round_trip(tavl::parser& p, const T& val) {
  std::string out;
  tavl::serialize<Opts>(val, out);
  return deserialize_all<T>(p, out);
}

template <auto Opts = tavl::sopts{}, typename T>
inline std::string to_text(const T& val) {
  std::string out;
  tavl::serialize<Opts>(val, out);
  return out;
}

// --- AST ---

inline std::string_view node_type_name(tavl::node_type t) {
  switch (t) {
    case tavl::node_type::object: return "object";
    case tavl::node_type::tuple:  return "tuple";
    case tavl::node_type::array:  return "array";
    case tavl::node_type::pair:   return "pair";
    case tavl::node_type::row:    return "row";
    case tavl::node_type::token:  return "tok";
    default:                      return "invalid";
  }
}

// Recursive render of a flat AST subtree as an S-expression:
//   (pair '=' (tok 'a') (tok 'b')). Empty token text is omitted for synthetic nodes.
inline void ast_node_str(const tavl::parser& p, const tavl::node* nodes, std::string& out) {
  out += "(";
  out += node_type_name(nodes[0].type);
  const auto txt = p.content(nodes[0].token.span);
  if (!txt.empty()) { out += " '"; out += std::string(txt); out += "'"; }
  for (size_t i = 1; i < nodes[0].child_count + 1; i += nodes[i].child_count + 1) {
    out += " ";
    ast_node_str(p, &nodes[i], out);
  }
  out += ")";
}

inline std::string ast_str(const tavl::parser& p, const std::vector<tavl::node>& nodes) {
  if (nodes.empty()) return "";
  std::string out;
  ast_node_str(p, nodes.data(), out);
  return out;
}

// Run one builder (make_pair_ast / make_tag_ast / make_math_ast) on one row and return the built
// AST. src is flushed all at once; we advance to row_begin, as a caller normally would.
struct ast_result {
  tavl::event event;
  tavl::error error;
  std::vector<tavl::node> nodes;
};

template <typename Builder>
inline ast_result build_ast_result(tavl::parser& p, std::string_view src, Builder builder) {
  p.clear();
  p.flush(src);
  p.finish();

  ast_result result;
  tavl::ast_context ctx;

  tavl::event ev{};
  tavl::error err;
  do { std::tie(ev, err) = p.poll_event(); }
  while (ev.type != tavl::event_type::row_begin && ev.type != tavl::event_type::eof);
  if (ev.type == tavl::event_type::eof) {
    result.event = ev;
    result.error = err;
    return result;
  }

  std::tie(result.event, result.error) = builder(p, ctx, result.nodes);
  return result;
}

template <typename Builder>
inline std::vector<tavl::node> build_ast(tavl::parser& p, std::string_view src, Builder builder) {
  return build_ast_result(p, src, builder).nodes;
}

template <typename Builder>
inline ast_result build_ast_streamed(tavl::parser& p, std::string_view src, Builder builder, size_t chunk = 1) {
  p.clear();

  ast_result result;
  tavl::ast_context ctx;
  size_t i = 0;
  bool finished = false;
  bool in_row = false;

  while (true) {
    if (!in_row) {
      auto [ev, err] = p.poll_event();
      if (ev.type == tavl::event_type::row_begin) {
        in_row = true;
      } else if (ev.type == tavl::event_type::eof) {
        result.event = ev;
        result.error = err;
        return result;
      } else if (ev.type == tavl::event_type::not_enought_data) {
        if (i < src.size()) { p.flush(src.substr(i, chunk)); i += chunk; }
        else if (!finished) { p.finish(); finished = true; }
      }
      continue;
    }

    std::tie(result.event, result.error) = builder(p, ctx, result.nodes);
    if (result.event.type == tavl::event_type::not_enought_data) {
      if (i < src.size()) { p.flush(src.substr(i, chunk)); i += chunk; }
      else if (!finished) { p.finish(); finished = true; }
      continue;
    }
    return result;
  }
}

}  // namespace tavl_test
