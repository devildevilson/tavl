# tavl

A small, streaming, resumable parser for a custom **row-based** config/data language, with
compile-time (de)serialization to and from C++ structs via [`qlibs/reflect`](https://github.com/qlibs/reflect).

`tavl` is an experimental R&D project. The library is C++20, has no runtime dependencies of its own,
and is driven through a tiny push/pull API: you `flush()` arbitrary byte chunks and `poll_event()`
for parse events. Everything downstream — three AST builders and the struct (de)serializer — is
likewise resumable, so a chunk that ends mid-token simply stalls and resumes on the next `flush()`.

## The language

The unit of structure is a **row**: `identifier operator data...`. A row begins with its first token
and ends at a comma, a newline, or the end of its enclosing block. Rows live inside blocks:

| Block | Brackets | Row parse mode |
|---|---|---|
| object | `{ }` | first non-operator token is the row identifier, the next operator is the row operator |
| tuple  | `( )` | hybrid: `id op data` ⇒ like object, otherwise ⇒ like array |
| array  | `[ ]` | every token is data |

The whole file is an implicit tuple. A block's mode can be overridden (see
`parser::override_next_block_modes`).

### Data types

`null`, `boolean` (`true`/`false`), integers (decimal, `0x` hex, `0o` octal, `0b` binary),
floats (incl. `nan`/`inf`, `e` exponent, leading `.`), single-quoted strings `'...'` (multiline,
minimal escaping), double-quoted strings `"..."` (standard escapes + `\u`/`\U` unicode, multiline),
ISO `datetime` (`T` or `_` between date and time, optional fraction and timezone), identifiers
(`[A-Za-z_][A-Za-z0-9_.]*`), registered operators, and line `//` / block `/* */` comments
(block comments nest).

See [`tests/format/`](tests/format) for commented, runnable examples of the format. Those files are
part of the test suite, so the documented syntax is continuously parsed by `tavl_tests`.

```tavl
window = {
  width  = 1920
  title  = "main"
}
palette = [red, green, blue]
point   = (x = 10, y = 20)
created = 2026-06-14T12:30:00
```

## Build

MSYS2/MinGW + GCC, or MSVC — C++20, CMake (Ninja). `reflect` and (for tests) `doctest` are fetched
at configure time via `FetchContent`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure   # run the test suite
```

The main artifact is the static library **`tavl`**; the test runner is **`tavl_tests`**.
Disable tests with `-DTAVL_BUILD_TESTS=OFF`.

## Using the library

`#include "tavl/tavl.h"` pulls in everything. The pipeline is
`char_storage → lexer → parser (events) →` either an AST builder or `deserialize<T>`.

**Events**
```cpp
tavl::parser p;
p.add_default_operator();              // register '=' (and friends via add_*_operators)
p.flush("k = v\n");
p.finish();
for (;;) {
  auto [ev, err] = p.poll_event();     // err rides every event; err.is_critical() flags fatal ones
  if (ev.type == tavl::event_type::eof) break;
}
```

**AST builders** (in `ext.h`) turn the event stream into a `tavl::node` tree:
- `make_pair_ast` — every operator equal, binary, right-associative; free operands group into rows.
- `make_tag_ast` — XML/HCL-like: row identifier is a tag, the data after the operator are attributes.
- `make_math_ast` — operator-precedence (shunting-yard / Pratt) using each operator's
  fixity + precedence + associativity.

**Deserialize / serialize**
```cpp
struct color_rgb { int r, g, b; };

tavl::parser p; p.add_default_operator();
tavl::ct_context ctx;
color_rgb c{};
p.flush("r = 255\ng = 128\nb = 0"); p.finish();
tavl::deserialize(p, ctx, c);          // fills fields by name (or positionally)

std::string out;
tavl::serialize(c, out);               // mirror; options via tavl::serialize<sopts{...}>(c, out)
```

`deserialize<T>` / `serialize<T>` handle primitives, `std::string`, `std::array<char,N>` buffers,
optional / unique_ptr, pair / tuple / `std::array`, vector / list / deque, map / set, nested
aggregates, and `std::chrono` time_point / duration. Extend a custom type `T` (e.g. an `enum`) with
an overload in `T`'s namespace (found by ADL):
```cpp
void deserialize(tavl::parser&, tavl::ct_context&, T&);                // non-template
template <auto Opts> bool serialize(const T&, std::string&, size_t);   // template (explicit NTTP), returns bool
```

`serialize<Opts>` returns `bool` — normally `true`; with `sopts{.bounded = true}` it never grows `out`
beyond its `capacity()` and returns `false` if the output didn't fit (useful for fixed-buffer / bounded-memory rendering).

> Note: aggregates may not have C-array members (`char buf[8]`) — `reflect` miscounts them. Use
> `std::array<char, 8>` instead.

## Many instances of one struct

Often you don't want a separate file per record. `deserialize_next<T>(p, ctx, out)` reads **one
top-level instance** per call (skipping row separators) and returns `false` at eof. The *same loop*
yields one instance from a single-record file or N from a "list" file — there is no fixed list
schema, each top-level block/row is just treated as an instance:

```cpp
struct data { int a; int b; std::array<int, 3> c; };

tavl::parser p; p.add_default_operator();
tavl::ct_context ctx;
std::vector<data> items;
for (data d{}; tavl::deserialize_next(p, ctx, d); d = data{}) items.push_back(d);
```

```tavl
a = 5, b = 6, c = 1 2 3            // one file → 1 instance

(a = 4, b = 2, c = (4,5,6)),       // one file → N instances; each (...) is one record,
(a = 0, b = 2, c = 7 8 9)          // commas/newlines between/inside records are interchangeable
```

To classify up front, `parser::peek()` is a non-consuming lookahead: the first *content* event
(skipping `row_begin`) being a block-begin ⇒ list, otherwise ⇒ single record.

For unbounded streams (e.g. records arriving as network packets), call `tavl::release_consumed(p, ctx)`
after each `deserialize_next` to drop the already-parsed bytes — the input buffer then stays bounded by
the in-flight window instead of growing with the whole stream. (Event-level consumers can use
`parser::consumed_offset()` + `parser::release_before()` directly.)

## Multiple documents in one stream

Concatenate independent documents with a `//---` line — it's an ordinary **comment**, so it has no
effect on the format and (being a comment token) is never confused with `//---` appearing inside a
string. `is_document_separator(p, token)` (in `ext.h`) recognizes it on a `got_comment` event:

```tavl
a = 1
//---
a = 2
```
On a separator, `finish()` the current document, drain to `eof`, then `clear()` the parser and feed
the rest to a fresh document. (Split documents *before* iterating instances with `deserialize_next`,
which otherwise skips the comment.)

## Layout

| Path | Holds |
|---|---|
| `include/tavl/` + `src/` | the library: `common` / `detail` / `lexer` / `parser` / `type_traits` / `ext` / `deserialize` / `serialize` |
| `tests/format/` | commented `.tavl` examples (documentation) + a parse-clean test |
| `tests/features/` | lexer, parser, serialize, deserialize, chrono |
| `tests/errors/` | error and misuse handling |
| `tests/extensions/` | the three AST builders, separately |
