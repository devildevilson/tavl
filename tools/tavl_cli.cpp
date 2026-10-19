#include "tavl/parser.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct block;
enum class block_kind { root, object, tuple, array };

struct value {
  tavl::token token;
  size_t end = 0;
  std::unique_ptr<block> nested;
};

struct row {
  std::optional<tavl::token> name;
  std::optional<tavl::token> op;
  std::vector<value> data;
  size_t begin = 0;
  size_t end = 0;
  tavl::token_type end_delimiter = tavl::token_type::invalid;
};

struct block {
  block_kind kind = block_kind::root;
  size_t open_offset = 0;
  size_t close_offset = 0;
  std::vector<row> rows;
};

struct document {
  block root;
  bool errors = false;
};

struct field_names {
  struct scope {
    block_kind kind;
    std::unordered_set<std::string> names;
  };
  std::vector<scope> scopes{{block_kind::root, {}}};

  std::optional<tavl::source_span> observe(const tavl::event& ev, const tavl::parser& parser) {
    switch (ev.type) {
      case tavl::event_type::object_begin:
        scopes.push_back({block_kind::object, {}});
        break;
      case tavl::event_type::tuple_begin:
        scopes.push_back({block_kind::tuple, {}});
        break;
      case tavl::event_type::array_begin:
        scopes.push_back({block_kind::array, {}});
        break;
      case tavl::event_type::object_end:
      case tavl::event_type::tuple_end:
      case tavl::event_type::array_end:
        if (scopes.size() > 1) scopes.pop_back();
        break;
      case tavl::event_type::got_row_identifier:
        if (scopes.back().kind != block_kind::root &&
            scopes.back().kind != block_kind::array &&
            !scopes.back().names.insert(parser.to_string(ev.token)).second)
          return ev.token.span;
        break;
      default:
        break;
    }
    return std::nullopt;
  }
};

bool is_value_event(tavl::event_type t) {
  return t == tavl::event_type::got_token || t == tavl::event_type::got_row_identifier ||
         t == tavl::event_type::got_row_operator;
}

bool read_input(std::string_view path, std::string& out) {
  if (path == "-") {
    out.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
    return !std::cin.bad();
  }
  std::ifstream file(std::string(path), std::ios::binary);
  if (!file) return false;
  out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return !file.bad();
}

void diagnostic(std::string_view filename, const tavl::source_span& span,
                std::string_view level, std::string_view message) {
  std::cerr << filename << ':' << span.line << ':' << span.column
            << ": " << level << ": " << message << '\n';
}

document parse(tavl::parser& parser, std::string_view filename, bool build_view, bool report = true) {
  document doc;
  std::vector<block*> blocks{&doc.root};
  field_names names;
  parser.add_default_operator();
  for (;;) {
    const auto [ev, err] = parser.poll_event();
    if (!err.no_error()) {
      if (report || err.is_critical())
        diagnostic(filename, err.span, err.is_critical() ? "error" : "warning",
                   tavl::to_string(err.type));
      doc.errors |= err.is_critical();
    }
    if (report && is_value_event(ev.type) && ev.token.type == tavl::token_type::unrecognized)
      diagnostic(filename, ev.token.span, "warning", "unrecognized token in default TAVL profile");
    if (const auto duplicate = names.observe(ev, parser)) {
      diagnostic(filename, *duplicate, "error", tavl::to_string(tavl::error_type::err_duplicate_field));
      doc.errors = true;
      build_view = false;
    }
    if (err.is_critical()) build_view = false;
    if (!build_view) {
      if (ev.type == tavl::event_type::eof) return doc;
      continue;
    }
    switch (ev.type) {
      case tavl::event_type::row_begin:
        blocks.back()->rows.emplace_back();
        blocks.back()->rows.back().begin = ev.token.span.offset;
        break;
      case tavl::event_type::row_end:
        blocks.back()->rows.back().end = ev.token.span.offset +
          (ev.token.type == tavl::token_type::comma ||
           ev.token.type == tavl::token_type::newline ? ev.token.span.size : 0);
        blocks.back()->rows.back().end_delimiter = ev.token.type;
        break;
      case tavl::event_type::got_row_identifier:
        blocks.back()->rows.back().name = ev.token;
        break;
      case tavl::event_type::got_row_operator:
        blocks.back()->rows.back().op = ev.token;
        break;
      case tavl::event_type::got_token:
        blocks.back()->rows.back().data.push_back(value{ev.token, ev.token.span.offset + ev.token.span.size, {}});
        break;
      case tavl::event_type::object_begin:
      case tavl::event_type::tuple_begin:
      case tavl::event_type::array_begin: {
        auto child = std::make_unique<block>();
        child->kind = ev.type == tavl::event_type::object_begin ? block_kind::object
                    : ev.type == tavl::event_type::tuple_begin  ? block_kind::tuple
                                                                : block_kind::array;
        child->open_offset = ev.token.span.offset;
        block* ptr = child.get();
        blocks.back()->rows.back().data.push_back(value{ev.token, 0, std::move(child)});
        blocks.push_back(ptr);
        break;
      }
      case tavl::event_type::object_end:
      case tavl::event_type::tuple_end:
      case tavl::event_type::array_end:
        if (blocks.size() > 1) {
          blocks.back()->close_offset = ev.token.span.offset;
          blocks.pop_back();
          blocks.back()->rows.back().data.back().end = ev.token.span.offset + ev.token.span.size;
        }
        break;
      case tavl::event_type::eof:
        return doc;
      default:
        break;
    }
  }
}

enum class output_style { preserve, compact, pretty };

struct output_options {
  output_style style = output_style::preserve;
  size_t names_depth = SIZE_MAX;
};

bool parse_output_option(std::string_view arg, output_options& options) {
  if (arg == "--output=preserve") options.style = output_style::preserve;
  else if (arg == "--output=compact") options.style = output_style::compact;
  else if (arg == "--output=pretty") options.style = output_style::pretty;
  else if (arg.starts_with("--names-depth=")) {
    const auto number = arg.substr(sizeof("--names-depth=") - 1);
    size_t depth = 0;
    const auto parsed = std::from_chars(number.data(), number.data() + number.size(), depth);
    if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
      return false;
    options.names_depth = depth;
    if (options.style == output_style::preserve) options.style = output_style::pretty;
  } else return false;
  return true;
}

void indent_output(std::string& out, size_t depth) { out.append(depth * 2, ' '); }

void render_block(const block& b, std::string_view source, const output_options& options,
                  size_t depth, std::string& out);

void render_row(const row& r, block_kind parent_kind, std::string_view source,
                const output_options& options, size_t depth, std::string& out) {
  const bool named_field = r.op || ((parent_kind == block_kind::root ||
                                     parent_kind == block_kind::object) && !r.data.empty());
  const bool keep_name = r.name && (!named_field || depth < options.names_depth);
  if (keep_name) {
    out.append(source.substr(r.name->span.offset, r.name->span.size));
    if (r.op) out += options.style == output_style::pretty ? " = " : "=";
  }
  for (size_t i = 0; i < r.data.size(); ++i) {
    const bool previous_scalar = i ? !r.data[i - 1].nested : keep_name && !r.op;
    if ((options.style == output_style::pretty && (i || (keep_name && !r.op))) ||
        (previous_scalar && !r.data[i].nested)) out += ' ';
    const auto& v = r.data[i];
    if (v.nested) render_block(*v.nested, source, options, depth + 1, out);
    else out.append(source.substr(v.token.span.offset, v.token.span.size));
  }
}

void render_block(const block& b, std::string_view source, const output_options& options,
                  size_t depth, std::string& out) {
  const bool root = b.kind == block_kind::root;
  const bool positional_object = b.kind == block_kind::object && depth >= options.names_depth;
  const char open = positional_object ? '(' : b.kind == block_kind::object ? '{'
                    : b.kind == block_kind::tuple ? '(' : '[';
  const char close = positional_object ? ')' : b.kind == block_kind::object ? '}'
                     : b.kind == block_kind::tuple ? ')' : ']';
  if (!root) out += open;
  for (size_t i = 0; i < b.rows.size(); ++i) {
    if (i) out += root && options.style == output_style::pretty ? "\n"
                   : options.style == output_style::pretty ? ",\n" : ",";
    else if (!root && options.style == output_style::pretty) out += '\n';
    if (!root && options.style == output_style::pretty) indent_output(out, depth);
    render_row(b.rows[i], b.kind, source, options, root ? 0 : depth, out);
  }
  if (!root) {
    if (!b.rows.empty() && options.style == output_style::pretty) {
      out += '\n';
      indent_output(out, depth - 1);
    }
    out += close;
  }
}

int format_source(std::string_view source, const output_options& options, std::string& output) {
  if (options.style == output_style::preserve) {
    output = source;
    return 0;
  }
  tavl::parser parser;
  parser.flush(source);
  parser.finish();
  const auto doc = parse(parser, "<format>", true, false);
  if (doc.errors) return 2;
  render_block(doc.root, source, options, 0, output);
  if (!output.empty()) output += '\n';
  return 0;
}

enum class step_kind { name, index, wildcard };
struct path_step {
  step_kind kind;
  std::string name;
  size_t index = 0;
};

std::optional<std::vector<path_step>> parse_path(std::string_view path,
                                                 bool allow_wildcard = false) {
  if (path.empty()) return std::nullopt;
  std::vector<path_step> steps;
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '.') {
      ++i;
      const size_t start = i;
      while (i < path.size() && path[i] != '.' && path[i] != '[') ++i;
      if (start == i) return std::nullopt;
      steps.push_back({step_kind::name, std::string(path.substr(start, i - start)), 0});
    } else if (path[i] == '[') {
      ++i;
      if (allow_wildcard && i + 1 < path.size() && path[i] == '*' && path[i + 1] == ']') {
        steps.push_back({step_kind::wildcard, {}, 0});
        i += 2;
      } else if (i < path.size() && path[i] == '"') {
        const size_t start = ++i;
        while (i < path.size() && path[i] != '"') ++i;
        if (i >= path.size() || i + 1 >= path.size() || path[i + 1] != ']') return std::nullopt;
        steps.push_back({step_kind::name, std::string(path.substr(start, i - start)), 0});
        i += 2;
      } else {
        const size_t start = i;
        while (i < path.size() && path[i] >= '0' && path[i] <= '9') ++i;
        if (start == i || i >= path.size() || path[i] != ']') return std::nullopt;
        size_t index = 0;
        const auto num = path.substr(start, i - start);
        const auto result = std::from_chars(num.data(), num.data() + num.size(), index);
        if (result.ec != std::errc{} || result.ptr != num.data() + num.size()) return std::nullopt;
        steps.push_back({step_kind::index, {}, index});
        ++i;
      }
    } else {
      return std::nullopt;
    }
  }
  return steps;
}

struct selected {
  const block* block_ptr = nullptr;
  const row* row_ptr = nullptr;
  const value* value_ptr = nullptr;
  const tavl::token* token_ptr = nullptr;
  const row* owner_row = nullptr;
  bool data_sequence = false;
  std::optional<size_t> array_index;
};

const block* as_block(selected s) {
  if (s.data_sequence) return nullptr;
  if (s.block_ptr) return s.block_ptr;
  if (s.value_ptr && s.value_ptr->nested) return s.value_ptr->nested.get();
  if (s.row_ptr && s.row_ptr->data.size() == 1 && s.row_ptr->data.front().nested)
    return s.row_ptr->data.front().nested.get();
  return nullptr;
}

void select_block_index(const block& b, size_t index, std::vector<selected>& out) {
  if (b.kind == block_kind::root || b.kind == block_kind::object ||
      b.kind == block_kind::array) {
    if (index < b.rows.size())
      out.push_back({nullptr, &b.rows[index], nullptr, nullptr, nullptr, false,
                     b.kind == block_kind::array ? std::optional<size_t>(index) : std::nullopt});
    return;
  }
  // A tuple is a positional sequence; an array indexes complete rows instead.
  for (const auto& r : b.rows) {
    if (r.op) {
      if (index == 0) { out.push_back({nullptr, &r, nullptr, nullptr, nullptr}); return; }
      --index;
      continue;
    }
    if (r.name) {
      if (index == 0) { out.push_back({nullptr, nullptr, nullptr, &*r.name, &r}); return; }
      --index;
    }
    if (index < r.data.size()) {
      out.push_back({nullptr, nullptr, &r.data[index], nullptr, &r});
      return;
    }
    index -= r.data.size();
  }
}

std::vector<selected> select(const tavl::parser& parser, const block& root,
                             const std::vector<path_step>& steps) {
  std::vector<selected> current{{&root, nullptr, nullptr}};
  for (const auto& step : steps) {
    std::vector<selected> next;
    for (const auto s : current) {
      if (step.kind == step_kind::name) {
        const block* b = as_block(s);
        if (!b) continue;
        for (const auto& r : b->rows)
          if (r.name && parser.to_string(*r.name) == step.name)
            next.push_back({nullptr, &r, nullptr, nullptr, nullptr});
      } else {
        if (s.row_ptr && !(s.row_ptr->data.size() == 1 && s.row_ptr->data.front().nested)) {
          if (step.index < s.row_ptr->data.size())
            next.push_back({nullptr, nullptr, &s.row_ptr->data[step.index], nullptr, s.row_ptr});
        } else if (const block* b = as_block(s)) {
          select_block_index(*b, step.index, next);
        }
      }
    }
    current = std::move(next);
  }
  return current;
}

std::string_view value_text(const std::string& source, const value& v) {
  if (v.token.span.offset > source.size() || v.end > source.size() || v.end < v.token.span.offset)
    return {};
  return std::string_view(source).substr(v.token.span.offset, v.end - v.token.span.offset);
}

std::string_view selection_text(const std::string& source, selected s) {
  if (s.value_ptr) return value_text(source, *s.value_ptr);
  if (s.token_ptr && s.token_ptr->span.offset <= source.size() &&
      s.token_ptr->span.size <= source.size() - s.token_ptr->span.offset)
    return std::string_view(source).substr(s.token_ptr->span.offset, s.token_ptr->span.size);
  if (s.row_ptr && !s.row_ptr->data.empty()) {
    const auto& first = s.row_ptr->data.front();
    const auto& last = s.row_ptr->data.back();
    if (first.token.span.offset <= source.size() && last.end <= source.size() &&
        last.end >= first.token.span.offset)
      return std::string_view(source).substr(first.token.span.offset, last.end - first.token.span.offset);
  }
  return {};
}

struct span { size_t begin, end; };

std::optional<span> selection_span(selected s) {
  if (s.value_ptr) return span{s.value_ptr->token.span.offset, s.value_ptr->end};
  if (s.token_ptr) return span{s.token_ptr->span.offset,
                                s.token_ptr->span.offset + s.token_ptr->span.size};
  if (s.row_ptr && !s.row_ptr->data.empty())
    return span{s.row_ptr->data.front().token.span.offset, s.row_ptr->data.back().end};
  return std::nullopt;
}

std::optional<span> row_deletion_span(const row& r, std::string_view source) {
  size_t begin = r.begin;
  size_t end = r.end;
  if (r.end_delimiter == tavl::token_type::comma) {
    size_t previous = begin;
    while (previous > 0 && std::isspace(static_cast<unsigned char>(source[previous - 1])))
      --previous;
    if (previous > 0 && (source[previous - 1] == ',' ||
                         source[previous - 1] == '{' ||
                         source[previous - 1] == '(' ||
                         source[previous - 1] == '[')) begin = previous;
  }
  if (r.end_delimiter == tavl::token_type::close_brace ||
      r.end_delimiter == tavl::token_type::close_paren ||
      r.end_delimiter == tavl::token_type::close_bracket) {
    size_t previous = begin;
    while (previous > 0 && std::isspace(static_cast<unsigned char>(source[previous - 1])))
      --previous;
    if (previous > 0 && source[previous - 1] == ',') begin = previous - 1;
    while (end > r.begin && std::isspace(static_cast<unsigned char>(source[end - 1]))) --end;
  }
  return span{begin, end};
}

std::optional<span> deletion_span(selected s, std::string_view source) {
  if (s.data_sequence) return selection_span(s);
  if (s.row_ptr) return row_deletion_span(*s.row_ptr, source);
  if (s.owner_row && !s.owner_row->name && !s.owner_row->op &&
      s.owner_row->data.size() == 1)
    return row_deletion_span(*s.owner_row, source);
  return selection_span(s);
}

bool nullify_deleted_value(selected s) {
  const row* owner = s.owner_row;
  if (!owner || !owner->name || owner->data.size() != 1) return false;
  return (s.value_ptr && s.value_ptr == &owner->data.front()) || s.data_sequence;
}

enum class stream_operation { filter, select, map };

// A stream path is relative to one top-level row, so `.abc` can inspect
// `1999-01-01_00:00:00 { abc = ok }` without naming the timestamp.
int process_row(std::string_view raw, const std::vector<path_step>& path,
                stream_operation operation, std::string_view argument, std::string& output) {
  tavl::parser p;
  p.flush(raw);
  p.finish();
  const auto doc = parse(p, "<row>", true, false);
  if (doc.errors || doc.root.rows.size() != 1) return 2;
  std::vector<path_step> absolute;
  absolute.reserve(path.size() + 1);
  absolute.push_back({step_kind::index, {}, 0});
  absolute.insert(absolute.end(), path.begin(), path.end());
  std::string source(raw);
  const auto matches = select(p, doc.root, absolute);
  if (operation == stream_operation::filter) {
    for (const auto match : matches)
      if (selection_text(source, match) == argument) {
        output = source;
        if (!output.empty() && output.back() == ',') output.pop_back();
        if (output.empty() || output.back() != '\n') output += '\n';
        break;
      }
  } else if (operation == stream_operation::select) {
    for (const auto match : matches) {
      output += selection_text(source, match);
      output += '\n';
    }
  } else {
    if (matches.size() > 1) {
      std::cerr << "map path matches more than one value in a row\n";
      return 4;
    }
    if (!matches.empty()) {
      const auto target = selection_span(matches.front());
      if (!target || target->begin > target->end || target->end > source.size()) return 2;
      source.replace(target->begin, target->end - target->begin, argument);
      tavl::parser check;
      check.flush(source);
      check.finish();
      if (parse(check, "<output row>", false, false).errors) return 2;
    }
    output = std::move(source);
  }
  return 0;
}

using row_processor = std::function<int(std::string_view, std::string&)>;

int stream_rows(std::string_view filename, bool follow, bool preserve_trailing,
                const row_processor& process) {
  if (follow && filename == "-") {
    std::cerr << "-f requires a regular file path\n";
    return 1;
  }
  std::ifstream file;
  std::istream* input = &std::cin;
  if (filename != "-") {
    file.open(std::string(filename), std::ios::binary);
    if (!file) { std::cerr << "cannot read input: " << filename << '\n'; return 2; }
    input = &file;
  }
  tavl::parser p;
  p.add_default_operator();
  field_names names;
  std::string window;
  size_t buffer_offset = 0;
  size_t row_offset = 0;
  int depth = 0;
  bool finished = false;
  char chunk[65536];
  for (;;) {
    const auto [ev, err] = p.poll_event();
    if (!err.no_error()) {
      diagnostic(filename, err.span, err.is_critical() ? "error" : "warning",
                 tavl::to_string(err.type));
      if (err.is_critical()) return 2;
    }
    if (is_value_event(ev.type) && ev.token.type == tavl::token_type::unrecognized)
      diagnostic(filename, ev.token.span, "warning", "unrecognized token in default TAVL profile");
    if (const auto duplicate = names.observe(ev, p)) {
      diagnostic(filename, *duplicate, "error", tavl::to_string(tavl::error_type::err_duplicate_field));
      return 2;
    }
    if (ev.type == tavl::event_type::object_begin ||
        ev.type == tavl::event_type::tuple_begin ||
        ev.type == tavl::event_type::array_begin) ++depth;
    if (ev.type == tavl::event_type::object_end ||
        ev.type == tavl::event_type::tuple_end ||
        ev.type == tavl::event_type::array_end) --depth;
    if (ev.type == tavl::event_type::row_end && depth == 0) {
      const size_t end = ev.token.span.offset +
        (ev.token.type == tavl::token_type::newline ||
         ev.token.type == tavl::token_type::comma ? ev.token.span.size : 0);
      if (end < buffer_offset + row_offset || end - buffer_offset > window.size()) return 2;
      const std::string_view raw(window.data() + row_offset,
                                 end - buffer_offset - row_offset);
      std::string output;
      const int status = process(raw, output);
      if (status != 0) return status;
      if (!output.empty()) {
        std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
        if (follow) std::cout.flush();
        if (!std::cout) return 2;
      }
      row_offset = end - buffer_offset;
    }
    if (ev.type == tavl::event_type::eof) {
      if (preserve_trailing && row_offset < window.size())
        std::cout.write(window.data() + row_offset,
                        static_cast<std::streamsize>(window.size() - row_offset));
      return std::cout ? 0 : 2;
    }
    if (ev.type != tavl::event_type::not_enought_data) continue;
    if (row_offset > 0) {
      window.erase(0, row_offset);
      buffer_offset += row_offset;
      row_offset = 0;
    }
    p.release_before(p.consumed_offset());
    input->read(chunk, sizeof(chunk));
    const auto n = input->gcount();
    if (n > 0) {
      p.flush(std::string_view(chunk, static_cast<size_t>(n)));
      window.append(chunk, static_cast<size_t>(n));
      continue;
    }
    if (input->bad()) { std::cerr << "cannot read input: " << filename << '\n'; return 2; }
    if (follow) {
      input->clear();
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    } else if (!finished) {
      p.finish();
      finished = true;
    }
  }
}

size_t row_content_end(const row& r) {
  return r.end - ((r.end_delimiter == tavl::token_type::comma ||
                   r.end_delimiter == tavl::token_type::newline) ? 1 : 0);
}

std::string leading_indent(const std::string& source, size_t position);
std::string indent_fragment(std::string_view fragment, std::string_view indent);

std::optional<std::string> append_array_text(std::string_view left_source, const block& left,
                                             std::string_view right_source, const block& right) {
  if (left.close_offset >= left_source.size() || right.close_offset >= right_source.size() ||
      right.open_offset >= right.close_offset) return std::nullopt;
  std::string result(left_source.substr(left.open_offset,
                                         left.close_offset - left.open_offset + 1));
  auto content = right_source.substr(right.open_offset + 1,
                                     right.close_offset - right.open_offset - 1);
  while (!content.empty() && std::isspace(static_cast<unsigned char>(content.front())))
    content.remove_prefix(1);
  while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back())))
    content.remove_suffix(1);
  if (content.empty()) return result;
  auto existing = left_source.substr(left.open_offset + 1,
                                     left.close_offset - left.open_offset - 1);
  while (!existing.empty() && std::isspace(static_cast<unsigned char>(existing.back())))
    existing.remove_suffix(1);
  std::string addition;
  if (!existing.empty()) addition = existing.back() == ',' ? " " : ", ";
  addition += content;
  result.insert(left.close_offset - left.open_offset, addition);
  return result;
}

struct patch {
  size_t begin = 0;
  size_t end = 0;
  std::string text;
};

std::optional<std::string> merge_block_text(const std::string& left_source,
                                             const tavl::parser& left_parser, const block& left,
                                             const std::string& right_source,
                                             const tavl::parser& right_parser, const block& right,
                                             bool append_arrays) {
  const bool root = left.kind == block_kind::root;
  const size_t begin = root ? 0 : left.open_offset;
  const size_t end = root ? left_source.size() : left.close_offset + 1;
  if (end > left_source.size() || left.kind != right.kind) return std::nullopt;

  auto names = [](const tavl::parser& parser, const block& b)
      -> std::optional<std::unordered_map<std::string, const row*>> {
    std::unordered_map<std::string, const row*> result;
    for (const auto& r : b.rows) {
      if (!r.name) {
        std::cerr << "merge requires named rows in document and object blocks\n";
        return std::nullopt;
      }
      const auto name = parser.to_string(*r.name);
      if (!result.emplace(name, &r).second) {
        std::cerr << "merge has duplicate name: " << name << '\n';
        return std::nullopt;
      }
    }
    return result;
  };
  const auto left_names = names(left_parser, left);
  const auto right_names = names(right_parser, right);
  if (!left_names || !right_names) return std::nullopt;

  std::vector<patch> patches;
  std::vector<std::string> additions;
  for (const auto& rr : right.rows) {
    const row* lr = nullptr;
    if (rr.name) {
      const auto it = left_names->find(right_parser.to_string(*rr.name));
      if (it != left_names->end()) lr = it->second;
    }
    if (!lr) {
      const size_t row_end = row_content_end(rr);
      if (rr.begin > row_end || row_end > right_source.size()) return std::nullopt;
      additions.push_back(right_source.substr(rr.begin, row_end - rr.begin));
      continue;
    }
    if (lr->data.empty() || rr.data.empty()) {
      std::cerr << "merge requires data on both sides of a matching name\n";
      return std::nullopt;
    }
    const auto left_selection = selected{nullptr, lr, nullptr, nullptr, nullptr};
    const auto right_selection = selected{nullptr, &rr, nullptr, nullptr, nullptr};
    const auto target = selection_span(left_selection);
    if (!target || target->end > left_source.size()) return std::nullopt;
    std::string replacement;
    const block* lb = as_block(left_selection);
    const block* rb = as_block(right_selection);
    if (lb && rb && lb->kind == block_kind::object && rb->kind == block_kind::object) {
      const auto nested = merge_block_text(left_source, left_parser, *lb,
                                           right_source, right_parser, *rb, append_arrays);
      if (!nested) return std::nullopt;
      replacement = *nested;
    } else if (append_arrays && lb && rb && lb->kind == block_kind::array &&
               rb->kind == block_kind::array) {
      const auto appended = append_array_text(left_source, *lb, right_source, *rb);
      if (!appended) return std::nullopt;
      replacement = *appended;
    } else {
      replacement = selection_text(right_source, right_selection);
    }
    patches.push_back({target->begin, target->end, std::move(replacement)});
  }
  if (!additions.empty()) {
    const size_t position = root ? left_source.size() : left.close_offset;
    std::string insertion;
    if (root) {
      if (position > 0 && left_source[position - 1] != '\n') insertion += '\n';
      for (const auto& addition : additions) insertion += addition + '\n';
    } else {
      const std::string base_indent = leading_indent(left_source, left.open_offset);
      const std::string child_indent = base_indent + "  ";
      for (const auto& addition : additions)
        insertion += '\n' + child_indent + indent_fragment(addition, child_indent);
      insertion += '\n' + base_indent;
    }
    patches.push_back({position, position, std::move(insertion)});
  }
  std::sort(patches.begin(), patches.end(), [](const patch& a, const patch& b) {
    return a.begin > b.begin;
  });
  std::string result = left_source.substr(begin, end - begin);
  for (const auto& p : patches) {
    if (p.begin < begin || p.end > end || p.begin > p.end) return std::nullopt;
    result.replace(p.begin - begin, p.end - p.begin, p.text);
  }
  return result;
}

int merge_files(std::string_view left_path, std::string_view right_path, bool append_arrays) {
  if (left_path == "-" && right_path == "-") {
    std::cerr << "merge can read stdin for only one input\n";
    return 1;
  }
  std::string left_source, right_source;
  if (!read_input(left_path, left_source) || !read_input(right_path, right_source)) {
    std::cerr << "cannot read merge input\n";
    return 2;
  }
  tavl::parser left_parser, right_parser;
  left_parser.flush(left_source);
  right_parser.flush(right_source);
  left_parser.finish();
  right_parser.finish();
  const auto left = parse(left_parser, left_path, true);
  const auto right = parse(right_parser, right_path, true);
  if (left.errors || right.errors) return 2;
  const auto merged = merge_block_text(left_source, left_parser, left.root,
                                       right_source, right_parser, right.root, append_arrays);
  if (!merged) return 4;
  tavl::parser check;
  check.flush(*merged);
  check.finish();
  if (parse(check, "<merged>", false, false).errors) return 2;
  std::cout << *merged;
  return std::cout ? 0 : 2;
}

bool validate_set_fragment(std::string_view fragment) {
  std::string wrapped = "value = ";
  wrapped += fragment;
  tavl::parser p;
  p.flush(wrapped);
  p.finish();
  const auto doc = parse(p, "<replacement>", true);
  if (doc.errors || doc.root.rows.size() != 1 ||
      doc.root.rows.front().data.empty()) {
    std::cerr << "replacement must contain one nonempty TAVL data row\n";
    return false;
  }
  return true;
}

bool validate_append_fragment(std::string_view fragment, block_kind kind) {
  const char open = kind == block_kind::object ? '{' : kind == block_kind::tuple ? '(' : '[';
  const char close = kind == block_kind::object ? '}' : kind == block_kind::tuple ? ')' : ']';
  std::string wrapped;
  wrapped += open;
  wrapped += '\n';
  wrapped += fragment;
  wrapped += '\n';
  wrapped += close;
  tavl::parser p;
  p.flush(wrapped);
  p.finish();
  const auto doc = parse(p, "<append>", true);
  if (doc.errors || doc.root.rows.size() != 1 ||
      doc.root.rows.front().data.size() != 1 ||
      !doc.root.rows.front().data.front().nested ||
      doc.root.rows.front().data.front().nested->rows.empty()) {
    std::cerr << "append requires one or more TAVL rows\n";
    return false;
  }
  return true;
}

std::string leading_indent(const std::string& source, size_t position) {
  const size_t previous = position == 0 ? std::string::npos : source.rfind('\n', position - 1);
  const size_t start = previous == std::string::npos ? 0 : previous + 1;
  size_t end = start;
  while (end < position && (source[end] == ' ' || source[end] == '\t')) ++end;
  return source.substr(start, end - start);
}

std::string indent_fragment(std::string_view fragment, std::string_view indent) {
  while (!fragment.empty() && std::isspace(static_cast<unsigned char>(fragment.front())))
    fragment.remove_prefix(1);
  while (!fragment.empty() && std::isspace(static_cast<unsigned char>(fragment.back())))
    fragment.remove_suffix(1);
  std::string out;
  for (const char c : fragment) {
    out += c;
    if (c == '\n') out += indent;
  }
  return out;
}

void append_to_block(std::string& source, const block& target, std::string_view fragment) {
  size_t begin = target.close_offset;
  while (begin > target.open_offset + 1 &&
         std::isspace(static_cast<unsigned char>(source[begin - 1]))) --begin;
  const auto body = std::string_view(source).substr(target.open_offset + 1,
                                                    target.close_offset - target.open_offset - 1);
  if (body.find('\n') == std::string_view::npos && body.find('\r') == std::string_view::npos &&
      fragment.find('\n') == std::string_view::npos &&
      fragment.find('\r') == std::string_view::npos &&
      fragment.find("//") == std::string_view::npos) {
    std::string insertion;
    if (begin == target.open_offset + 1) {
      if (!body.empty()) insertion += ' ';
    } else {
      insertion = source[begin - 1] == ',' ? " " : ", ";
    }
    insertion += fragment;
    source.insert(begin, insertion);
    return;
  }
  const std::string base_indent = leading_indent(source, target.open_offset);
  const std::string child_indent = base_indent + "  ";
  const std::string newline = source.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  source.replace(begin, target.close_offset - begin,
                 newline + child_indent + indent_fragment(fragment, child_indent) +
                   newline + base_indent);
}

// The eval profile adds operators only to its own parser. Its AST uses TAVL rows:
// commas/newlines separate steps, while | joins stages guarded by where().
struct query_item {
  const tavl::token* token = nullptr;
  const block* nested = nullptr;
  size_t begin = 0;
  size_t end = 0;
};

std::vector<query_item> query_items(const row& r) {
  std::vector<query_item> items;
  if (r.name) items.push_back({&*r.name, nullptr, r.name->span.offset,
                               r.name->span.offset + r.name->span.size});
  if (r.op) items.push_back({&*r.op, nullptr, r.op->span.offset,
                             r.op->span.offset + r.op->span.size});
  for (const auto& v : r.data)
    items.push_back({&v.token, v.nested.get(), v.token.span.offset, v.end});
  std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
    return a.begin < b.begin;
  });
  return items;
}

std::string_view trim_view(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

enum class eval_expr_kind { literal, path, call, comparison };
struct eval_expr {
  eval_expr_kind kind = eval_expr_kind::literal;
  std::string text;
  tavl::token_type literal_type = tavl::token_type::invalid;
  std::vector<path_step> path;
  std::unique_ptr<eval_expr> left, right;
};

enum class eval_stage_kind { where, edit, set, append, insert, upsert, del };
struct eval_program;
struct eval_stage {
  eval_stage_kind kind;
  std::unique_ptr<eval_expr> condition;
  std::vector<path_step> path;
  std::string replacement;
  size_t index = 0;
  std::unique_ptr<eval_program> body;
};
struct eval_step { std::vector<eval_stage> stages; };
struct eval_program { std::vector<eval_step> steps; };

std::optional<std::vector<path_step>> query_path(std::string_view source,
                                                  const std::vector<query_item>& items) {
  if (items.empty()) return std::nullopt;
  const auto raw = trim_view(source.substr(items.front().begin,
                                           items.back().end - items.front().begin));
  if (raw == ".") return std::vector<path_step>{};
  if (raw == "@name") return std::vector<path_step>{{step_kind::name, "@name", 0}};
  if (raw == "@data") return std::vector<path_step>{{step_kind::name, "@data", 0}};
  if (raw.starts_with("@data")) {
    auto rest = parse_path(raw.substr(5), true);
    if (!rest) return std::nullopt;
    rest->insert(rest->begin(), {step_kind::name, "@data", 0});
    return rest;
  }
  return parse_path(raw, true);
}

std::unique_ptr<eval_expr> compile_expr(const tavl::parser& parser, std::string_view source,
                                        const std::vector<query_item>& items) {
  if (items.empty()) return {};
  for (size_t i = 0; i < items.size(); ++i) {
    const auto text = source.substr(items[i].begin, items[i].end - items[i].begin);
    if (text == "==" || text == "!=" || text == "<>" || text == ">" ||
        text == "<" || text == ">=" || text == "<=") {
      auto result = std::make_unique<eval_expr>();
      result->kind = eval_expr_kind::comparison;
      result->text = std::string(text);
      result->left = compile_expr(parser, source, {items.begin(), items.begin() + i});
      result->right = compile_expr(parser, source, {items.begin() + i + 1, items.end()});
      return result->left && result->right ? std::move(result) : nullptr;
    }
  }
  if (items.size() == 2 && items[1].nested && items[1].nested->kind == block_kind::tuple) {
    const auto name = source.substr(items[0].begin, items[0].end - items[0].begin);
    const auto& args = items[1].nested->rows;
    if ((name == "date" || name == "time" || name == "exist" || name == "count") &&
        args.size() == 1) {
      auto result = std::make_unique<eval_expr>();
      result->kind = eval_expr_kind::call;
      result->text = std::string(name);
      result->left = compile_expr(parser, source, query_items(args.front()));
      if ((name == "exist" || name == "count") && result->left &&
          result->left->kind != eval_expr_kind::path) return {};
      return result->left ? std::move(result) : nullptr;
    }
    if ((name == "any" || name == "all") && (args.size() == 1 || args.size() == 2)) {
      auto result = std::make_unique<eval_expr>();
      result->kind = eval_expr_kind::call;
      result->text = std::string(name);
      result->left = compile_expr(parser, source, query_items(args.front()));
      if (!result->left || result->left->kind != eval_expr_kind::path) return {};
      if (args.size() == 2) {
        result->right = compile_expr(parser, source, query_items(args[1]));
        if (!result->right) return {};
      }
      return result;
    }
    return {};
  }
  if (auto path = query_path(source, items)) {
    auto result = std::make_unique<eval_expr>();
    result->kind = eval_expr_kind::path;
    result->path = std::move(*path);
    return result;
  }
  if (items.size() != 1 || items[0].nested) return {};
  auto result = std::make_unique<eval_expr>();
  result->text = parser.to_string(*items[0].token);
  result->literal_type = items[0].token->type;
  return result;
}

std::optional<eval_program> compile_program(const tavl::parser& parser,
                                            std::string_view source, const block& b);

std::optional<eval_stage> compile_stage(const tavl::parser& parser, std::string_view source,
                                        const std::vector<query_item>& items) {
  if (items.size() < 2 || !items[1].nested ||
      items[1].nested->kind != block_kind::tuple) return std::nullopt;
  const auto name = source.substr(items[0].begin, items[0].end - items[0].begin);
  const auto& args = items[1].nested->rows;
  eval_stage stage{};
  if (name == "where" && items.size() == 2 && args.size() == 1) {
    stage.kind = eval_stage_kind::where;
    stage.condition = compile_expr(parser, source, query_items(args.front()));
    if (!stage.condition) return std::nullopt;
  } else if (name == "edit" && items.size() == 3 && items[2].nested &&
             items[2].nested->kind == block_kind::object && args.size() == 1) {
    stage.kind = eval_stage_kind::edit;
    auto path = query_path(source, query_items(args.front()));
    auto body = compile_program(parser, source, *items[2].nested);
    if (!path || !body) return std::nullopt;
    stage.path = std::move(*path);
    stage.body = std::make_unique<eval_program>(std::move(*body));
  } else if (name == "set" && items.size() == 2 && args.size() == 2) {
    stage.kind = eval_stage_kind::set;
    auto path = query_path(source, query_items(args[0]));
    if (!path) return std::nullopt;
    stage.path = std::move(*path);
    const auto replacement = trim_view(source.substr(args[1].begin,
                                                        row_content_end(args[1]) - args[1].begin));
    if (replacement.empty() || !validate_set_fragment(replacement)) return std::nullopt;
    stage.replacement = replacement;
  } else if (name == "append" && items.size() == 2 && args.size() == 2) {
    stage.kind = eval_stage_kind::append;
    auto path = query_path(source, query_items(args[0]));
    if (!path) return std::nullopt;
    stage.path = std::move(*path);
    const auto fragment = trim_view(source.substr(args[1].begin,
                                                     row_content_end(args[1]) - args[1].begin));
    if (fragment.empty()) return std::nullopt;
    stage.replacement = fragment;
  } else if (name == "insert" && items.size() == 2 && args.size() == 3) {
    stage.kind = eval_stage_kind::insert;
    auto path = query_path(source, query_items(args[0]));
    if (!path) return std::nullopt;
    stage.path = std::move(*path);
    const auto index_text = trim_view(source.substr(args[1].begin,
                                                      row_content_end(args[1]) - args[1].begin));
    const auto number = std::from_chars(index_text.data(), index_text.data() + index_text.size(),
                                        stage.index);
    if (index_text.empty() || number.ec != std::errc{} ||
        number.ptr != index_text.data() + index_text.size()) return std::nullopt;
    const auto fragment = trim_view(source.substr(args[2].begin,
                                                     row_content_end(args[2]) - args[2].begin));
    if (fragment.empty()) return std::nullopt;
    stage.replacement = fragment;
  } else if (name == "upsert" && items.size() == 2 && args.size() == 2) {
    stage.kind = eval_stage_kind::upsert;
    auto path = query_path(source, query_items(args[0]));
    if (!path || path->empty() || path->back().kind != step_kind::name ||
        path->back().name == "@name" || path->back().name == "@data") return std::nullopt;
    stage.path = std::move(*path);
    const auto replacement = trim_view(source.substr(args[1].begin,
                                                        row_content_end(args[1]) - args[1].begin));
    if (replacement.empty() || !validate_set_fragment(replacement)) return std::nullopt;
    stage.replacement = replacement;
  } else if (name == "del" && items.size() == 2 && args.size() == 1) {
    stage.kind = eval_stage_kind::del;
    auto path = query_path(source, query_items(args.front()));
    if (!path) return std::nullopt;
    stage.path = std::move(*path);
  } else return std::nullopt;
  return stage;
}

std::optional<eval_program> compile_program(const tavl::parser& parser,
                                            std::string_view source, const block& b) {
  eval_program program;
  for (const auto& r : b.rows) {
    const auto items = query_items(r);
    if (items.empty()) continue;
    eval_step step;
    size_t begin = 0;
    for (size_t i = 0; i <= items.size(); ++i) {
      if (i != items.size() && source.substr(items[i].begin,
                                             items[i].end - items[i].begin) != "|") continue;
      if (begin == i) return std::nullopt;
      auto stage = compile_stage(parser, source, {items.begin() + begin, items.begin() + i});
      if (!stage) return std::nullopt;
      step.stages.push_back(std::move(*stage));
      begin = i + 1;
    }
    program.steps.push_back(std::move(step));
  }
  return program.steps.empty() ? std::nullopt : std::optional<eval_program>(std::move(program));
}

struct eval_match {
  selected item;
  std::vector<path_step> concrete_path;
};

std::vector<eval_match> eval_select_matches(const tavl::parser& parser, const block& root,
                                            const std::vector<path_step>& path) {
  if (root.rows.size() != 1) return {};
  std::vector<eval_match> current{{{nullptr, &root.rows.front(), nullptr, nullptr}, {}}};
  for (const auto& step : path) {
    std::vector<eval_match> next;
    for (const auto& match : current) {
      const auto s = match.item;
      auto add = [&](selected item, path_step concrete) {
        auto steps = match.concrete_path;
        steps.push_back(std::move(concrete));
        next.push_back({item, std::move(steps)});
      };
      if (step.kind == step_kind::name && step.name == "@name") {
        if (s.row_ptr && s.array_index)
          add({nullptr, nullptr, nullptr, nullptr, nullptr, false, s.array_index}, step);
        else if (s.row_ptr && s.row_ptr->name)
          add({nullptr, nullptr, nullptr, &*s.row_ptr->name, s.row_ptr}, step);
      } else if (step.kind == step_kind::name && step.name == "@data") {
        if (s.row_ptr) add({nullptr, s.row_ptr, nullptr, nullptr, s.row_ptr, true}, step);
      } else if (step.kind == step_kind::name) {
        const block* b = as_block(s);
        if (!b) continue;
        for (const auto& r : b->rows)
          if (r.name && parser.to_string(*r.name) == step.name)
            add({nullptr, &r, nullptr, nullptr}, step);
      } else if (step.kind == step_kind::index) {
        if (s.row_ptr && (!s.array_index || s.data_sequence || !as_block(s))) {
          if (step.index < s.row_ptr->data.size())
            add({nullptr, nullptr, &s.row_ptr->data[step.index], nullptr, s.row_ptr}, step);
        } else if (const block* b = as_block(s)) {
          std::vector<selected> found;
          select_block_index(*b, step.index, found);
          for (const auto item : found) add(item, step);
        }
      } else if (s.row_ptr && (!s.array_index || s.data_sequence || !as_block(s))) {
        for (size_t i = 0; i < s.row_ptr->data.size(); ++i)
          add({nullptr, nullptr, &s.row_ptr->data[i], nullptr, s.row_ptr},
              {step_kind::index, {}, i});
      } else if (const block* b = as_block(s)) {
        size_t index = 0;
        if (b->kind == block_kind::root || b->kind == block_kind::object ||
            b->kind == block_kind::array) {
          for (const auto& r : b->rows)
            add({nullptr, &r, nullptr, nullptr, nullptr, false,
                 b->kind == block_kind::array ? std::optional<size_t>(index) : std::nullopt},
                {step_kind::index, {}, index++});
        } else {
          for (const auto& r : b->rows) {
            if (r.op) {
              add({nullptr, &r, nullptr, nullptr}, {step_kind::index, {}, index++});
              continue;
            }
            if (r.name)
              add({nullptr, nullptr, nullptr, &*r.name, &r},
                  {step_kind::index, {}, index++});
            for (const auto& v : r.data)
              add({nullptr, nullptr, &v, nullptr, &r}, {step_kind::index, {}, index++});
          }
        }
      }
    }
    current = std::move(next);
  }
  return current;
}

std::vector<selected> eval_select(const tavl::parser& parser, const block& root,
                                  const std::vector<path_step>& path) {
  std::vector<selected> result;
  for (const auto& match : eval_select_matches(parser, root, path))
    result.push_back(match.item);
  return result;
}

struct eval_scalar {
  enum class kind { missing, text, number, date, time, boolean } type = kind::missing;
  std::string text;
  bool boolean = false;
  long double number = 0;
};

eval_scalar scalar_from_token(const tavl::parser& parser, const tavl::token& token) {
  eval_scalar result{eval_scalar::kind::text, parser.to_string(token)};
  if (token.type == tavl::token_type::boolean) {
    result.type = eval_scalar::kind::boolean;
    result.boolean = result.text == "true";
  } else if (token.type == tavl::token_type::number_int ||
             token.type == tavl::token_type::number_uint ||
             token.type == tavl::token_type::number_float) {
    const auto parsed = std::from_chars(result.text.data(),
                                        result.text.data() + result.text.size(), result.number);
    if (parsed.ec == std::errc{} && parsed.ptr == result.text.data() + result.text.size())
      result.type = eval_scalar::kind::number;
  }
  return result;
}

std::string date_part(std::string_view text) {
  if (text.size() >= 10 && text[4] == '-' && text[7] == '-')
    return std::string(text.substr(0, 10));
  return {};
}

std::string time_part(std::string_view text) {
  if (const auto underscore = text.find('_'); underscore != std::string_view::npos)
    text.remove_prefix(underscore + 1);
  if (text.size() < 8 || text[2] != ':' || text[5] != ':') return {};
  std::string result(text.substr(0, 8));
  std::string fraction;
  if (text.size() > 8 && text[8] == '.') {
    size_t i = 9;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
      fraction += text[i++];
  }
  fraction.resize(9, '0');
  return result + '.' + fraction;
}

eval_scalar eval_expression(const eval_expr& expr, const tavl::parser& parser,
                            const block& root, const std::string& source,
                            const std::vector<path_step>& anchor);
bool eval_truth(eval_scalar value);

eval_scalar eval_expression(const eval_expr& expr, const tavl::parser& parser,
                            const block& root, const std::string& source,
                            const std::vector<path_step>& anchor) {
  if (expr.kind == eval_expr_kind::literal) {
    eval_scalar result{eval_scalar::kind::text, expr.text};
    if (expr.literal_type == tavl::token_type::boolean) {
      result.type = eval_scalar::kind::boolean;
      result.boolean = expr.text == "true";
    } else if (expr.literal_type == tavl::token_type::number_int ||
               expr.literal_type == tavl::token_type::number_uint ||
               expr.literal_type == tavl::token_type::number_float) {
      const auto parsed = std::from_chars(result.text.data(),
                                          result.text.data() + result.text.size(), result.number);
      if (parsed.ec == std::errc{} && parsed.ptr == result.text.data() + result.text.size())
        result.type = eval_scalar::kind::number;
    }
    return result;
  }
  if (expr.kind == eval_expr_kind::path) {
    auto path = anchor;
    path.insert(path.end(), expr.path.begin(), expr.path.end());
    const auto matches = eval_select(parser, root, path);
    if (matches.size() != 1) return {};
    const auto s = matches.front();
    if (s.array_index && !s.row_ptr && !s.value_ptr && !s.token_ptr)
      return {eval_scalar::kind::number, std::to_string(*s.array_index), false,
              static_cast<long double>(*s.array_index)};
    if (s.token_ptr) return scalar_from_token(parser, *s.token_ptr);
    if (s.value_ptr && !s.value_ptr->nested)
      return scalar_from_token(parser, s.value_ptr->token);
    if (s.row_ptr && s.row_ptr->data.size() == 1 && !s.row_ptr->data.front().nested)
      return scalar_from_token(parser, s.row_ptr->data.front().token);
    return {eval_scalar::kind::text, std::string(selection_text(source, s)), false};
  }
  if (expr.kind == eval_expr_kind::call) {
    if (expr.text == "exist" || expr.text == "count" ||
        expr.text == "any" || expr.text == "all") {
      auto path = anchor;
      path.insert(path.end(), expr.left->path.begin(), expr.left->path.end());
      const auto matches = eval_select_matches(parser, root, path);
      if (expr.text == "exist")
        return {eval_scalar::kind::boolean, {}, !matches.empty()};
      if (expr.text == "count")
        return {eval_scalar::kind::number, std::to_string(matches.size()), false,
                static_cast<long double>(matches.size())};
      bool result = expr.text == "all";
      for (const auto& match : matches) {
        eval_expr current;
        current.kind = eval_expr_kind::path;
        const bool truth = eval_truth(eval_expression(expr.right ? *expr.right : current,
                                                     parser, root, source,
                                                     match.concrete_path));
        if (expr.text == "any" && truth) { result = true; break; }
        if (expr.text == "all" && !truth) { result = false; break; }
      }
      return {eval_scalar::kind::boolean, {}, result};
    }
    auto arg = eval_expression(*expr.left, parser, root, source, anchor);
    if (arg.type == eval_scalar::kind::missing) return arg;
    if (expr.text == "date") {
      arg.text = date_part(arg.text);
      arg.type = arg.text.empty() ? eval_scalar::kind::missing : eval_scalar::kind::date;
    } else if (expr.text == "time") {
      arg.text = time_part(arg.text);
      arg.type = arg.text.empty() ? eval_scalar::kind::missing : eval_scalar::kind::time;
    }
    return arg;
  }
  auto left = eval_expression(*expr.left, parser, root, source, anchor);
  auto right = eval_expression(*expr.right, parser, root, source, anchor);
  if (left.type == eval_scalar::kind::missing || right.type == eval_scalar::kind::missing)
    return {eval_scalar::kind::boolean, {}, false};
  if (left.type == eval_scalar::kind::number || right.type == eval_scalar::kind::number) {
    if (left.type != eval_scalar::kind::number || right.type != eval_scalar::kind::number)
      return {eval_scalar::kind::boolean, {}, false};
    const bool result = expr.text == "==" ? left.number == right.number
                      : expr.text == "!=" || expr.text == "<>" ? left.number != right.number
                      : expr.text == ">" ? left.number > right.number
                      : expr.text == "<" ? left.number < right.number
                      : expr.text == ">=" ? left.number >= right.number
                      : left.number <= right.number;
    return {eval_scalar::kind::boolean, {}, result};
  }
  if (left.type == eval_scalar::kind::boolean || right.type == eval_scalar::kind::boolean) {
    if (left.type != eval_scalar::kind::boolean || right.type != eval_scalar::kind::boolean)
      return {eval_scalar::kind::boolean, {}, false};
    const int cmp = static_cast<int>(left.boolean) - static_cast<int>(right.boolean);
    const bool result = expr.text == "==" ? cmp == 0 : expr.text == "!=" || expr.text == "<>" ? cmp != 0
                      : expr.text == ">" ? cmp > 0 : expr.text == "<" ? cmp < 0
                      : expr.text == ">=" ? cmp >= 0 : cmp <= 0;
    return {eval_scalar::kind::boolean, {}, result};
  }
  if (left.type == eval_scalar::kind::date || right.type == eval_scalar::kind::date) {
    left.text = date_part(left.text);
    right.text = date_part(right.text);
  } else if (left.type == eval_scalar::kind::time || right.type == eval_scalar::kind::time) {
    left.text = time_part(left.text);
    right.text = time_part(right.text);
  }
  if (left.text.empty() || right.text.empty())
    return {eval_scalar::kind::boolean, {}, false};
  const int cmp = left.text.compare(right.text);
  const bool result = expr.text == "==" ? cmp == 0 : expr.text == "!=" || expr.text == "<>" ? cmp != 0
                    : expr.text == ">" ? cmp > 0 : expr.text == "<" ? cmp < 0
                    : expr.text == ">=" ? cmp >= 0 : cmp <= 0;
  return {eval_scalar::kind::boolean, {}, result};
}

bool eval_truth(eval_scalar value) {
  if (value.type == eval_scalar::kind::boolean) return value.boolean;
  if (value.type == eval_scalar::kind::number) return value.number != 0;
  return value.type != eval_scalar::kind::missing && !value.text.empty() &&
         value.text != "false" && value.text != "null";
}

std::vector<size_t> block_element_offsets(const block& b) {
  std::vector<size_t> offsets;
  for (const auto& r : b.rows) {
    if (b.kind == block_kind::object || b.kind == block_kind::root ||
        b.kind == block_kind::array || r.op) {
      offsets.push_back(r.begin);
      continue;
    }
    if (r.name) offsets.push_back(r.name->span.offset);
    for (const auto& v : r.data) offsets.push_back(v.token.span.offset);
  }
  return offsets;
}

bool insert_into_block(std::string& source, const block& target, size_t index,
                       std::string_view fragment) {
  const auto offsets = block_element_offsets(target);
  if (index > offsets.size()) return false;
  if (index == offsets.size()) {
    append_to_block(source, target, fragment);
    return true;
  }
  const std::string separator = fragment.find("//") != std::string_view::npos ||
                                fragment.find('\n') != std::string_view::npos ||
                                fragment.find('\r') != std::string_view::npos ? "\n" : ", ";
  source.insert(offsets[index], std::string(fragment) + separator);
  return true;
}

bool insert_into_data(std::string& source, const row& target, size_t index,
                      std::string_view fragment) {
  if (index > target.data.size()) return false;
  if (index < target.data.size()) {
    source.insert(target.data[index].token.span.offset, std::string(fragment) + " ");
    return true;
  }
  const size_t position = !target.data.empty() ? target.data.back().end
                          : target.op ? target.op->span.offset + target.op->span.size
                          : target.name ? target.name->span.offset + target.name->span.size
                                        : target.begin;
  source.insert(position, " " + std::string(fragment));
  return true;
}

std::string encoded_name(std::string_view name) {
  const auto bare_start = [](unsigned char c) { return std::isalpha(c) || c == '_'; };
  const auto bare_continue = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
  bool bare = !name.empty() && bare_start(static_cast<unsigned char>(name.front()));
  for (const char c : name)
    if (!bare_continue(static_cast<unsigned char>(c))) bare = false;
  if (bare) return std::string(name);
  std::string quoted = "\"";
  for (const char c : name) {
    if (c == '\\' || c == '"') quoted += '\\';
    quoted += c;
  }
  quoted += '"';
  return quoted;
}

int upsert_field(std::string& source, const tavl::parser& parser, const block& b,
                 std::string_view name, std::string_view replacement) {
  if (b.kind != block_kind::object && b.kind != block_kind::root) {
    std::cerr << "upsert parent must select an object or document block\n";
    return 4;
  }
  const row* existing = nullptr;
  for (const auto& r : b.rows) {
    if (!r.name || parser.to_string(*r.name) != name) continue;
    if (existing) {
      std::cerr << "upsert field is ambiguous in object\n";
      return 4;
    }
    existing = &r;
  }
  if (!existing) {
    const std::string new_row = encoded_name(name) + " = " + std::string(replacement);
    if (!validate_append_fragment(new_row, block_kind::object)) return 2;
    if (b.kind == block_kind::root) {
      if (!source.empty() && source.back() != '\n') source += '\n';
      source += new_row + '\n';
    } else {
      if (b.close_offset > source.size()) return 2;
      append_to_block(source, b, new_row);
    }
  } else if (const auto target = selection_span({nullptr, existing, nullptr, nullptr})) {
    if (target->end > source.size()) return 2;
    source.replace(target->begin, target->end - target->begin, replacement);
  } else if (existing->op) {
    source.insert(existing->op->span.offset + existing->op->span.size,
                  " " + std::string(replacement));
  } else if (existing->name) {
    source.insert(existing->name->span.offset + existing->name->span.size,
                  " = " + std::string(replacement));
  } else return 2;
  return 0;
}

int run_upsert(const eval_stage& stage, const tavl::parser& parser, const block& root,
               std::string& source, const std::vector<path_step>& anchor) {
  auto parent_path = anchor;
  parent_path.insert(parent_path.end(), stage.path.begin(), stage.path.end() - 1);
  auto parents = eval_select_matches(parser, root, parent_path);
  const bool wildcard = std::any_of(stage.path.begin(), stage.path.end() - 1,
                                    [](const path_step& s) { return s.kind == step_kind::wildcard; });
  if (parents.size() > 1 && !wildcard) {
    std::cerr << "upsert parent path is ambiguous in row\n";
    return 4;
  }
  if (parents.empty()) {
    std::cerr << "upsert parent path does not exist in row\n";
    return 3;
  }
  const auto& name = stage.path.back().name;
  std::sort(parents.begin(), parents.end(), [](const eval_match& a, const eval_match& b) {
    const auto* left = as_block(a.item);
    const auto* right = as_block(b.item);
    return (left ? left->open_offset : 0) > (right ? right->open_offset : 0);
  });
  for (const auto& parent : parents) {
    const block* b = as_block(parent.item);
    if (!b || b->kind != block_kind::object) {
      std::cerr << "upsert parent must select an object block\n";
      return 4;
    }
    const int status = upsert_field(source, parser, *b, name, stage.replacement);
    if (status) return status;
  }
  return 0;
}

int run_program(const eval_program& program, std::string& source,
                const std::vector<path_step>& anchor, bool root_scope, bool& keep) {
  for (const auto& step : program.steps) {
    bool step_passed = true;
    for (const auto& stage : step.stages) {
      tavl::parser parser;
      parser.flush(source);
      parser.finish();
      const auto doc = parse(parser, "<row>", true, false);
      if (doc.errors || doc.root.rows.size() != 1) return 2;
      if (stage.kind == eval_stage_kind::where) {
        if (!eval_truth(eval_expression(*stage.condition, parser, doc.root, source, anchor))) {
          step_passed = false;
          break;
        }
        continue;
      }
      if (stage.kind == eval_stage_kind::upsert) {
        const int status = run_upsert(stage, parser, doc.root, source, anchor);
        if (status) return status;
        continue;
      }
      auto path = anchor;
      path.insert(path.end(), stage.path.begin(), stage.path.end());
      auto matches = eval_select_matches(parser, doc.root, path);
      const bool wildcard = std::any_of(stage.path.begin(), stage.path.end(),
                                        [](const path_step& s) { return s.kind == step_kind::wildcard; });
      if (matches.size() > 1 && !wildcard) {
        std::cerr << "eval path is ambiguous in row\n";
        return 4;
      }
      std::sort(matches.begin(), matches.end(), [](const eval_match& a, const eval_match& b) {
        const auto sa = selection_span(a.item);
        const auto sb = selection_span(b.item);
        return (sa ? sa->begin : a.item.row_ptr ? a.item.row_ptr->begin : 0) >
               (sb ? sb->begin : b.item.row_ptr ? b.item.row_ptr->begin : 0);
      });
      if (stage.kind == eval_stage_kind::edit) {
        for (const auto& match : matches) {
          if (!as_block(match.item)) {
            std::cerr << "edit path must select a bracket block\n";
            return 4;
          }
          bool ignored = true;
          const int status = run_program(*stage.body, source, match.concrete_path, false, ignored);
          if (status) return status;
        }
        continue;
      }
      if (matches.empty()) {
        if (stage.kind == eval_stage_kind::del) continue;
        std::cerr << "eval path does not exist in row\n";
        return 3;
      }
      if (stage.kind == eval_stage_kind::append || stage.kind == eval_stage_kind::insert) {
        for (const auto& match : matches) {
          if (match.item.data_sequence) {
            if (!match.item.row_ptr || !validate_set_fragment(stage.replacement)) return 2;
            const size_t index = stage.kind == eval_stage_kind::append
                                   ? match.item.row_ptr->data.size() : stage.index;
            if (!insert_into_data(source, *match.item.row_ptr, index, stage.replacement)) {
              std::cerr << "insert index is out of range\n";
              return 3;
            }
            continue;
          }
          const block* target = as_block(match.item);
          if (!target || target->kind == block_kind::root ||
              target->close_offset > source.size() ||
              !validate_append_fragment(stage.replacement, target->kind)) {
            std::cerr << "append/insert path must select a bracket block\n";
            return 4;
          }
          if (stage.kind == eval_stage_kind::append) {
            append_to_block(source, *target, stage.replacement);
          } else if (!insert_into_block(source, *target, stage.index, stage.replacement)) {
            std::cerr << "insert index is out of range\n";
            return 3;
          }
        }
        continue;
      }
      struct pending_edit { span target; bool nullify = false; };
      std::vector<pending_edit> targets;
      for (const auto& match : matches) {
        const auto target = stage.kind == eval_stage_kind::del
                              ? deletion_span(match.item, source)
                              : selection_span(match.item);
        if (!target && stage.kind == eval_stage_kind::del && match.item.data_sequence)
          continue;
        if (!target || target->begin > target->end || target->end > source.size()) return 2;
        targets.push_back({*target, stage.kind == eval_stage_kind::del &&
                                    nullify_deleted_value(match.item)});
      }
      std::sort(targets.begin(), targets.end(), [](const pending_edit& a, const pending_edit& b) {
        return a.target.begin > b.target.begin;
      });
      std::vector<pending_edit> edits;
      for (const auto target : targets) {
        if (!edits.empty() && target.target.end >= edits.back().target.begin) {
          if (stage.kind != eval_stage_kind::del) {
            std::cerr << "eval path selects overlapping values\n";
            return 4;
          }
          edits.back().target.begin = target.target.begin;
          edits.back().target.end = std::max(edits.back().target.end, target.target.end);
          edits.back().nullify &= target.nullify;
        } else {
          edits.push_back(target);
        }
      }
      for (const auto target : edits)
        source.replace(target.target.begin, target.target.end - target.target.begin,
                       stage.kind == eval_stage_kind::set ? stage.replacement
                       : target.nullify ? "null" : "");
    }
    if (root_scope && step_passed) keep = true;
  }
  return 0;
}

int eval_row(std::string_view raw, const eval_program& program,
             const output_options& options, std::string& output) {
  std::string source(raw);
  bool keep = false;
  const int status = run_program(program, source, {}, true, keep);
  if (status || !keep) return status;
  tavl::parser check;
  check.flush(source);
  check.finish();
  if (parse(check, "<output row>", false, false).errors) return 2;
  if (options.style == output_style::preserve) {
    if (!source.empty() && source.back() == ',') source.pop_back();
    if (source.empty() || source.back() != '\n') source += '\n';
    output = std::move(source);
  } else if (format_source(source, options, output)) return 2;
  return 0;
}

std::optional<eval_program> make_eval_ast(std::string_view source) {
  tavl::parser parser;
  parser.add_operator("|", tavl::op_fixity::binary, 1);
  parser.add_operator("==", tavl::op_fixity::binary, 9);
  parser.add_operator("!=", tavl::op_fixity::binary, 9);
  parser.add_operator("<>", tavl::op_fixity::binary, 9);
  parser.add_operator(">", tavl::op_fixity::binary, 10);
  parser.add_operator("<", tavl::op_fixity::binary, 10);
  parser.add_operator(">=", tavl::op_fixity::binary, 10);
  parser.add_operator("<=", tavl::op_fixity::binary, 10);
  parser.flush(source);
  parser.finish();
  const auto doc = parse(parser, "<eval>", true, false);
  if (doc.errors) return std::nullopt;
  return compile_program(parser, source, doc.root);
}

void usage() {
  std::cerr << "usage: tavl check [file|-]\n"
               "       tavl get <path> [file|-]\n"
               "       tavl set <path> <data> [file|-]\n"
               "       tavl append <path> <rows> [file|-]\n"
               "       tavl insert <path> <index> <rows> [file|-]\n"
               "       tavl upsert <path> <data> [file|-]\n"
               "       tavl del <path> [file|-]\n"
               "       tavl filter [-f] <path> <data> [file|-]\n"
               "       tavl select [-f] <path> [file|-]\n"
               "       tavl map [-f] <path> <data> [file|-]\n"
               "       tavl eval [-f] [--output=preserve|compact|pretty] [--names-depth=N] <program> [file|-]\n"
               "       tavl format [-f] [--output=preserve|compact|pretty] [--names-depth=N] [file|-]\n"
               "       tavl merge [--arrays=append] <left> <right>\n"
               "paths: .field, [index], [\"literal.key\"]; defaults to stdin\n"
               "eval paths also support [*] for each child and . for the current focus\n"
               "eval: where/edit/set/append/insert/upsert/del; exist/count/any/all/date/time; == != > < >= <=\n"
               "compact and pretty regenerate data; names-depth=0 drops named fields from the root\n"
               "edits write the changed document to stdout; stream paths are relative to each row\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { usage(); return 1; }
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h") { usage(); return 0; }
  const bool get = command == "get";
  const bool set = command == "set";
  const bool append = command == "append";
  const bool insert = command == "insert";
  const bool upsert = command == "upsert";
  const bool del = command == "del";
  const bool filter = command == "filter";
  const bool select_command = command == "select";
  const bool map_command = command == "map";
  const bool merge = command == "merge";
  const bool eval = command == "eval";
  const bool format = command == "format" || command == "fmt";
  const bool edit = set || append || insert || upsert || del;
  if (command != "check" && !get && !edit && !filter && !select_command && !map_command && !merge && !eval && !format) {
    usage(); return 1;
  }
  if (merge) {
    const bool append_arrays = argc > 2 && std::string_view(argv[2]) == "--arrays=append";
    if (argc != (append_arrays ? 5 : 4)) { usage(); return 1; }
    return merge_files(argv[append_arrays ? 3 : 2], argv[append_arrays ? 4 : 3], append_arrays);
  }
  if (eval || format) {
    int i = 2;
    bool follow = false;
    bool explicit_preserve = false;
    output_options options;
    if (format) options.style = output_style::pretty;
    while (i < argc && std::string_view(argv[i]).starts_with("-")) {
      const std::string_view arg = argv[i];
      if (arg == "-f") follow = true;
      else {
        if (arg == "--output=preserve") explicit_preserve = true;
        else if (arg.starts_with("--output=")) explicit_preserve = false;
        if (explicit_preserve && arg.starts_with("--names-depth=")) {
          std::cerr << "--names-depth requires compact or pretty output\n";
          return 1;
        }
        if (!parse_output_option(arg, options)) break;
      }
      ++i;
    }
    if (options.style == output_style::preserve && options.names_depth != SIZE_MAX) {
      std::cerr << "--names-depth requires compact or pretty output\n";
      return 1;
    }
    if (argc - i < (eval ? 1 : 0) || argc - i > (eval ? 2 : 1)) { usage(); return 1; }
    std::optional<eval_program> program;
    if (eval) {
      program = make_eval_ast(argv[i]);
      if (!program) { std::cerr << "invalid eval program\n"; return 1; }
    }
    const std::string_view filename = argc - i == (eval ? 2 : 1) ? argv[i + (eval ? 1 : 0)] : "-";
    return stream_rows(filename, follow, format && options.style == output_style::preserve,
      [&](std::string_view raw, std::string& output) {
        return eval ? eval_row(raw, *program, options, output)
                    : format_source(raw, options, output);
      });
  }
  if (filter || select_command || map_command) {
    int i = 2;
    const bool follow = i < argc && std::string_view(argv[i]) == "-f";
    if (follow) ++i;
    const int required = select_command ? 1 : 2;
    if (argc - i < required || argc - i > required + 1) { usage(); return 1; }
    const auto relative = parse_path(argv[i]);
    if (!relative) { std::cerr << "invalid path: " << argv[i] << '\n'; return 1; }
    if (map_command && !validate_set_fragment(argv[i + 1])) return 2;
    const auto operation = filter ? stream_operation::filter
                         : select_command ? stream_operation::select : stream_operation::map;
    const std::string_view argument = select_command ? "" : argv[i + 1];
    const std::string_view filename = argc - i == required + 1 ? argv[i + required] : "-";
    return stream_rows(filename, follow, map_command,
      [&](std::string_view raw, std::string& output) {
        return process_row(raw, *relative, operation, argument, output);
      });
  }
  if ((command == "check" && argc > 3) ||
      (get && (argc < 3 || argc > 4)) ||
      ((set || append || upsert) && (argc < 4 || argc > 5)) ||
      (insert && (argc < 5 || argc > 6)) ||
      (del && (argc < 3 || argc > 4))) { usage(); return 1; }
  const std::string_view filename = insert ? (argc == 6 ? argv[5] : "-")
                                  : (set || append || upsert) ? (argc == 5 ? argv[4] : "-")
                                  : del ? (argc == 4 ? argv[3] : "-")
                                  : get  ? (argc == 4 ? argv[3] : "-")
                                         : (argc == 3 ? argv[2] : "-");
  std::optional<std::vector<path_step>> path;
  if (get || edit) {
    path = parse_path(argv[2]);
    if (!path) { std::cerr << "invalid path: " << argv[2] << '\n'; return 1; }
  }
  std::string source;
  if (!read_input(filename, source)) {
    std::cerr << "cannot read input: " << filename << '\n';
    return 2;
  }
  tavl::parser parser;
  parser.flush(source);
  parser.finish();
  const auto doc = parse(parser, filename, get || edit);
  if (doc.errors) return 2;
  if (command == "check") return 0;
  if (upsert) {
    if (path->empty() || path->back().kind != step_kind::name) {
      std::cerr << "upsert requires a named field path\n";
      return 1;
    }
    if (!validate_set_fragment(argv[3])) return 2;
    auto parent_path = *path;
    const std::string name = parent_path.back().name;
    parent_path.pop_back();
    const auto parents = select(parser, doc.root, parent_path);
    if (parents.empty()) return 3;
    if (parents.size() != 1) {
      std::cerr << "upsert parent path is ambiguous\n";
      return 4;
    }
    const block* parent = as_block(parents.front());
    if (!parent) {
      std::cerr << "upsert parent must select an object or document block\n";
      return 4;
    }
    const int status = upsert_field(source, parser, *parent, name, argv[3]);
    if (status) return status;
  } else {
    const auto matches = select(parser, doc.root, *path);
    if (matches.empty()) return 3;
    if (get) {
      for (const auto s : matches) std::cout << selection_text(source, s) << '\n';
      return 0;
    }
    if (matches.size() != 1) {
      std::cerr << "edit path matches more than one value\n";
      return 4;
    }
    if (del) {
      const auto selected = matches.front();
      const auto target = deletion_span(selected, source);
      if (!target || target->begin > target->end || target->end > source.size()) return 2;
      source.replace(target->begin, target->end - target->begin,
                     nullify_deleted_value(selected) ? "null" : "");
    } else if (set) {
      const std::string_view fragment = argv[3];
      const auto target = selection_span(matches.front());
      if (!target || target->begin > target->end || target->end > source.size() ||
          !validate_set_fragment(fragment)) return 2;
      source.replace(target->begin, target->end - target->begin, fragment);
    } else {
      const std::string_view fragment = insert ? argv[4] : argv[3];
      const block* target = as_block(matches.front());
      if (!target || target->kind == block_kind::root ||
          target->close_offset > source.size() ||
          !validate_append_fragment(fragment, target->kind)) {
        std::cerr << "append/insert path must select a bracket block\n";
        return 2;
      }
      if (insert) {
        const std::string_view index_text = argv[3];
        size_t index = 0;
        const auto parsed = std::from_chars(index_text.data(),
                                            index_text.data() + index_text.size(), index);
        if (index_text.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != index_text.data() + index_text.size()) {
          std::cerr << "insert index must be a nonnegative integer\n";
          return 1;
        }
        if (!insert_into_block(source, *target, index, fragment)) {
          std::cerr << "insert index is out of range\n";
          return 3;
        }
      } else {
        append_to_block(source, *target, fragment);
      }
    }
  }
  tavl::parser output_parser;
  output_parser.flush(source);
  output_parser.finish();
  if (parse(output_parser, "<output>", false, false).errors) return 2;
  std::cout << source;
  return 0;
}
