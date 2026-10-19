# tavl

TAVL is a UTF-8 text format built from rows, scalar tokens, and three kinds of
bracket blocks. This repository provides a resumable C++20 parser, C++ struct
serialization, three AST builders, and a command-line tool for inspecting and
editing TAVL data.

- [Format specification](docs/SPECIFICATION.md): source syntax, values,
  diagnostics, and canonical float output.
- [`tavl eval` language specification](docs/EVAL.md): paths, conditions,
  transformations, and execution order.
- [Conformance corpus](tests/conformance/README.md): exact events, spans,
  diagnostics, binary64 values, and CLI exit codes.

## Format at a glance

A document is an implicit root tuple. A comma or newline ends a row; spaces
separate tokens within the same row. `{}` holds object-like rows, `()` holds
tuple-like rows, and each row inside `[]` is one array element. In the ordinary
profile, `=` is the only registered operator and is optional between a row name
and its data.

```tavl
window = { width = 1920, title = "main" }
palette = [red, green, blue]
point = (10, 20)
1999-01-01_06:54:32 { status = ok, value = 123 }
```

All items after a name and optional operator belong to that row's data
sequence. A C++ tuple field can therefore read `b = 10 20 30`,
`b = (10, 20, 30)`, or `b = [10, 20, 30]`. These are alternative documents,
not three fields.
Inside an explicit array, `[1 2]` has **one** element row containing two
values, whereas `[1,2]` has **two** element rows. This distinction holds at
every nesting level. A comma requires a row before it; `{,}` and `[,]` are
errors. Trailing commas, empty blocks, and blank LF-separated lines are valid.

The parser can register other operators and override block row modes for
formats built on TAVL. Registration changes tokenization: with `:` registered,
`position:v3` is split around `:`; without it, the text remains one
recoverable `unrecognized` token. Structural diagnostics depend on an
operator's role, not its spelling: a registered binary `:` receives the same
missing-name and missing-value checks as `=`. Applications can interpret
unrecognized tokens in their own profiles.

The ordinary profile accepts UTF-8 text and typed null, boolean, integer,
float, date/time, identifier, and quoted-string tokens. It keeps date/time
fields as written instead of requiring a valid civil calendar or an epoch
conversion. Bare names and quoted names with the same decoded text compare
equally. See the format specification for exact spellings, UTF-8 and control
character rules, and error recovery.

## Build and test

CMake 3.30 or newer and a C++20 compiler are required. CMake fetches
`qlibs/reflect`; tests also fetch `doctest`. `TAVL_ARCH=OFF` selects a portable
instruction-set baseline. The default is `AVX` for TAVL's own sources.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTAVL_ARCH=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The library target is `tavl`, the CLI is `build/bin/tavl`, and the test runner
is `tavl_tests`. Tests and CLI are enabled by default only for a top-level build;
consumers can enable either with `TAVL_BUILD_TESTS` and `TAVL_BUILD_CLI`.

## Command-line tool

`check` and `get` inspect a document. Edit commands write the changed document
to stdout; they leave the input file untouched. Omit a file or use `-` to read
stdin where the command accepts one.

| Command | Operation |
| --- | --- |
| `check [file]` | Report syntax and generic duplicate-field diagnostics. |
| `get <path> [file]` | Print a selected value. |
| `set <path> <data> [file]` | Replace an existing value. |
| `append <path> <rows> [file]` | Add rows to a selected bracket block. |
| `insert <path> <index> <rows> [file]` | Insert at a zero-based position. |
| `upsert <path> <data> [file]` | Replace or add the final named field. |
| `del <path> [file]` | Remove a selected row or value. |
| `filter`, `select`, `map` | Process top-level rows with paths relative to each row. |
| `eval <program> [file]` | Run the query and edit language from [EVAL.md](docs/EVAL.md). |
| `format [file]` | Rewrite layout; `fmt` is an alias. |
| `merge [--arrays=append] <left> <right>` | Merge matching named object fields. |

Ordinary paths include `.field`, `[index]`, and `["literal.key"]`. An array
index selects a complete row, so `.matrix[0][0]` addresses a value inside the
first row of the first nested array. `eval` also supports `[*]`, `@name`,
`@data[index]`, and `.` for the current focus.

```sh
tavl check config.tavl
tavl get '.window.width' config.tavl
tavl set '.window.width' 1280 config.tavl > updated.tavl
tavl eval 'where(date(@name) == 1999-01-01) | edit(@data[0]) { set(.status, reviewed), del(.reason) }' logs.tavl > reviewed.tavl
tavl format --output=pretty reviewed.tavl > readable.tavl
```

`filter`, `select`, `map`, `eval`, and `format` process completed top-level
rows in chunks. Their memory use scales with the largest row. `-f` follows
appended bytes in a regular file and waits for complete rows; file rotation
and truncation are outside that mode. `check`, `get`, standalone edits, and
`merge` load a whole input document. A streaming command can have emitted
earlier rows when a later row fails.

`eval` preserves untouched text by default. `--output=compact` emits one row
per line with only necessary token separators; `--output=pretty` uses two-space
indentation. `format` defaults to pretty output. Explicit compact or pretty
formatting regenerates data and drops comments. `--names-depth=N` drops field
names at depth `N` and below; `0` also drops root names. Objects made
positional are written as tuples. The output style and name-depth options
are available on `eval` and `format`.

Exit status is 0 for success, including warnings; 1 for usage or an invalid
query; 2 for input or parse errors; 3 for a missing target or invalid insert
index; and 4 for an ambiguous or invalid block target. `check` warns about
`unrecognized` tokens because another profile may assign them meaning.

## C++ library

Include `tavl/tavl.h` for the complete API. The parser starts with no
operators; call `add_default_operator()` for ordinary TAVL. Feed arbitrary
byte chunks with `flush()`, call `finish()` at EOF, and pull events with
`poll_event()`. An event carries a diagnostic; `error::is_critical()` tells
whether the input failed validation.

```cpp
#include "tavl/tavl.h"

struct color_rgb { int r, g, b; };

int main() {
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush("r = 255\ng = 128\nb = 0");
  parser.finish();

  tavl::ct_context context;
  color_rgb color{};
  tavl::deserialize(parser, context, color);

  std::string output;
  bool written = tavl::serialize(color, output);
  return written ? 0 : 1;
}
```

The C++ binding fills aggregate fields by name or position. A text name that
does not match a field remains an unknown named field; a row beginning with a
number, boolean, null, or date/time token can fill fields positionally. For a
`std::tuple<int, int, int>` field, `b = 1 2 3`, `b = (1,2,3)`, and
`b = [1,2,3]` map to the same destination. In an explicit `[]`, one row maps
to one container element; `std::set` follows this rule too. Integer conversion
checks destination ranges. Duplicate fills of one aggregate field report
`err_duplicate_field`.

The binding supports primitives, strings, pairs, tuples, standard sequence and
associative containers, nested aggregates, and `std::chrono` values. A
`tavl::token` field accepts any scalar token kind. Its text is a source span:
copy or resolve it before calling `release_before()`, `release_consumed()`, or
`clear()` on the parser. A `string_view` returned by `content()` can also be
invalidated by a later `flush()`.

Default float serialization emits canonical IEEE 754 binary64 text, preserving
finite values and signed zero on a round trip. A custom `float_fmt` may lose
precision; a `long double` that cannot be represented exactly as binary64
causes serialization to fail. The raw `iso_datetime` value preserves parsed
fields. Conversion to `std::chrono` applies local-time or UTC rules and reports
range or precision loss where applicable.

`deserialize_next<T>()` reads one top-level instance at a time. After using an
instance, call `tavl::release_consumed(parser, context)` to keep only input
still needed by the parser and deserializer. Event consumers can use
`parser::consumed_offset()` with `parser::release_before()` directly. A `//---`
comment is an application-level document separator. On its `got_comment`
event, `is_document_separator()` recognizes it, while
`finish_at_document_separator(parser, token)` saves any already-buffered bytes
after the comment's LF and finalizes only the preceding document. Drain that
parser to `eof`, call `clear()`, then `flush()` the returned bytes and continue
feeding later chunks. An incomplete row in those bytes remains pending until
more input or the final `finish()` call. Calling `finish()` directly at the
separator would also parse buffered bytes from the next document.

The optional AST builders are `make_pair_ast` (equal-rank right-associative
pairs), `make_tag_ast` (tag-shaped rows), and `make_math_ast` (registered
fixity, precedence, and associativity). These are interpretations of the same
event stream, not additional ordinary-format syntax.

The tests in `tests/format/` are readable examples. The machine-readable
corpus in `tests/conformance/` checks exact events, source spans, diagnostics,
CLI statuses, and numeric results under whole-file and chunked input.
