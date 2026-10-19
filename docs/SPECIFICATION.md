# TAVL format specification

This document defines the TAVL source format, its core row and block structure,
and the ordinary data profile. It is the format contract for producers and
readers. The C++ binding and the `tavl eval` language consume TAVL; their APIs
and query semantics are described in the [README](../README.md) and
[EVAL.md](EVAL.md).

## 1. Terms and profiles

A **token** is a scalar, a bracket, a row separator, an operator, or a comment.
A **row** is an ordered sequence of data items, optionally preceded by a name
and an operator. A data item is a scalar token or a nested **block**. A block
has one of three bracket kinds: object `{}`, tuple `()`, or array `[]`.
The document is an implicit root tuple. Rows and data items retain their source
order; TAVL does not define a JSON-like key/value tree.

The **core parser** has configurable operators and block row modes. The
**ordinary data profile** registers only the symbolic operator `=` and uses
object-like mode for `{}`, tuple-like mode for `()` and the root, and data-driven
mode for `[]`. Other operators and mode overrides define derived profiles and
are outside the ordinary data profile.

## 2. Byte and token boundaries

A TAVL source document MUST be a well-formed UTF-8 byte sequence. An
ill-formed sequence anywhere in a token or comment is a critical
`err_invalid_utf8`; input remains available for recovery. UTF-8 is checked as
bytes without Unicode normalization. Source offsets and columns count bytes.
This rule does not make non-ASCII bare identifiers valid: quote such names.
The C++ serializer reports failure when asked to write an invalid UTF-8 string.
Opaque binary payloads require an application-defined encoding within a
string, such as Base64; TAVL does not give raw binary bytes a separate type.
The literal byte NUL (`0x00`) is forbidden anywhere in the source, including
quoted strings and comments, and produces critical `err_forbidden_nul`.
An actual U+0000 string value may be written explicitly as `"\u0000"`; after
decoding, its C++ string contains one NUL byte and must be handled with its
length. Serializers must escape this value rather than write a literal NUL.
Other literal C0 controls (`0x01`–`0x1F`) are permitted only for TAB (`0x09`),
LF (`0x0A`), FF (`0x0C`), CR (`0x0D`), and VT (`0x0B`). DEL (`0x7F`) and the C1
controls (Unicode U+0080–U+009F) are forbidden literally. This applies in
scalars, quoted strings, and comments; a forbidden control produces critical
`err_forbidden_control`. A double-quoted `\u` escape may encode any of these
as a string value, including NUL. The default serializer escapes forbidden
controls and may leave the five permitted whitespace controls literal.

The structural characters are ASCII `{ } ( ) [ ] ,` and LF (`0x0A`). A comma
or LF ends a pending row at its current block depth. A matching closing bracket
or EOF also ends a pending row. Space, tab, CR, form feed, and vertical tab
separate tokens without ending a row. Consequently, CRLF ends one row through
its LF, whereas a lone CR does not. A delimiter inside a quoted string or
comment is not structural.

Outside strings and comments, `//` starts a line comment ending before the next
LF or at EOF. `/* ... */` is a block comment; block comments can nest. Comments
are emitted for source-preserving consumers but do not add data items or end
rows. The `//---` document separator is an application convention, not a
structural token or an automatic parser reset.

A scalar ends at whitespace, a comma, a bracket, or a quote. A registered
symbolic operator may split an otherwise contiguous scalar; the longest
registered spelling wins. In the ordinary profile only `=` has that role.
Scalar classification takes place in this order: date/time; registered
operators; null; boolean; prefixed unsigned integer; decimal signed integer;
decimal unsigned integer; floating point; identifier; `unrecognized`.
`unrecognized` retains its exact source span and is recoverable, since a
derived profile may assign it meaning. The ordinary CLI warns about it.

### 2.1 Scalar spellings

| Kind | Spelling in the ordinary profile |
| --- | --- |
| Null | `null` or `NULL` |
| Boolean | `true`, `TRUE`, `false`, or `FALSE` |
| Identifier | ASCII `[A-Za-z_][A-Za-z0-9_.]*`, after reserved spellings are classified |
| Decimal integer | Optional `+` or `-`, then one or more decimal digits; signed 64-bit range, or unsigned 64-bit range when nonnegative |
| Prefixed unsigned integer | `0b`/`0B` binary, `0o`/`0O` octal, or `0x`/`0X` hexadecimal digits; unsigned 64-bit range |
| Decimal float | A finite decimal spelling with a point, exponent, or `f`/`F`/`d`/`D` suffix, optionally signed; also signed `nan`/`NAN`/`NaN` or `inf`/`INF` |
| Date/time | `YYYY-MM-DD`, `HH:MM:SS`, or `YYYY-MM-DD[T|_]HH:MM:SS`, optionally with one to nine fractional second digits and a timezone on a time form |

An integer-looking spelling outside the 64-bit ranges is `unrecognized`; it
does not silently become a float. A spelling such as `0e2` or `0f` is a float.

The ordinary float value is IEEE 754 binary64. A finite decimal spelling
denotes an exact decimal rational, rounded once to binary64 using
roundTiesToEven, regardless of the process rounding mode. The `f`/`F` and
`d`/`D` suffixes are spelling markers; neither changes the target precision.
Conversion must be correctly rounded for the entire accepted spelling, even
when it has more than 20 significant decimal digits. A nonzero exact value
that rounds to infinity or to zero is a critical `err_float_out_of_range`.
Finite subnormal results are valid. An exact zero with any exponent is valid;
its explicit sign is retained, so `-0.0` is negative zero. The explicit
`inf` and `nan` spellings are valid special values, not overflow. NaN payload
bits and its sign are not part of the data model.

The default serializer gives each binary64 value a canonical spelling:

1. For a nonzero finite magnitude, use the fewest significant decimal digits
   whose exact value rounds back to the same binary64 bits. Among decimals at
   that precision, use the one nearest the exact binary64 value; an exact
   decimal tie goes to an even last digit.
2. Remove trailing insignificant zeros. Render both ordinary decimal notation
   (always including a point and a digit on each side) and normalized
   scientific notation (one digit before the optional point, lowercase `e`,
   no leading `+` or padded exponent). Use the shorter result; a tie chooses
   ordinary notation. Thus `1.0`, `0.5`, `1e3` and `5e-324` are canonical.
3. Prefix `-` for a negative finite value. Render signed zeros as `0.0` and
   `-0.0`, infinities as `inf` and `-inf`, and every NaN as `nan`.

The default C++ serializer writes this spelling for both an empty float
format option and `"{}"`. An explicitly supplied custom `float_fmt` may
discard precision or even produce a non-float token. A C++ `long double`
value is serialized only when it is exactly representable as binary64;
otherwise serialization fails instead of silently narrowing it.
Source-preserving tools may retain the original spelling of an existing
float token; canonical output applies when serializing a binary64 value.

The finite input converter and canonical output use integer arithmetic and
do not depend on the C++ vendor's decimal float conversion. Input reports
`err_float_out_of_range` for valid decimal spellings outside the range.
When an integer token is read into a C++ binary64 field, its conversion also
uses roundTiesToEven rather than the process floating-point rounding mode.

Decimal conversion vectors:

| Input | Required result |
| --- | --- |
| `1.00000000000000011102230246251565404236316680908203125` | `1.0` (exact midpoint, ties to even) |
| `1.00000000000000011102230246251565404236316680908203126` | Next binary64 value after `1.0` |
| `5e-324` | Smallest positive subnormal |
| `1e-4000` | `err_float_out_of_range` (nonzero becomes zero) |
| `1e4000` | `err_float_out_of_range` (overflow) |
| `0e9999` | Positive zero |
| `-0.0` | Negative zero |

Date/time fields must use decimal digits and the shown widths. A timezone is
`Z`, `+HH`, `-HH`, `+HHMM`, `-HHMM`, `+HH:MM`, or `-HH:MM`. The parser checks
spelling, not civil calendar or clock ranges. Fractions shorter than nine
digits are padded on the right when read into `iso_datetime.ns`; longer ones
are not date/time tokens. No timezone denotes a local wall time; raw date/time
tokens are not required to map to a POSIX timestamp. Applications decide how
to interpret the fields.

### 2.2 Quoted strings

Both `'...'` and `"..."` may span lines. In single quotes, only `\'` and `\\`
are decoded; any other backslash sequence stays literal. In double quotes the
recognized escapes are `\n`, `\r`, `\t`, `\a`, `\b`, `\f`, `\v`, `\\`, `\"`,
`\u` followed by exactly four hexadecimal digits, and `\U` followed by
exactly eight hexadecimal digits. A `\u` high surrogate must be followed
immediately by a `\u` low surrogate. A lone surrogate or a value above
`0x10FFFF` is invalid. The decoded Unicode escapes produce UTF-8 bytes.

An invalid double-quoted escape or an unterminated quoted string is a critical
diagnostic. The parser still emits the token with its source span for recovery.
Raw non-ASCII bytes in quoted strings must form valid UTF-8. Escape-produced
Unicode scalar values also produce UTF-8. Literal controls follow the rule in
section 2; Unicode escapes may still produce them as string values.

## 3. Rows and blocks

Whitespace alone never divides rows. Within any row, the complete sequence
after its name and optional operator is its data; the sequence may contain
several scalars or blocks. For example, `b = 1 2` has one row with name `b`
and two scalar data items. `b = (1,2)` has one data item: a tuple block whose
contents have their own rows. A typed C++ consumer may map these different
structures to the same destination type.

In object-like `{}` rows, the first non-operator scalar is held as the name.
In tuple-like `()` and root rows, the first non-operator scalar is a possible
name: when further data or an operator appears in that row, it becomes the
name; when the row ends with that scalar alone, it is data. An operator can
become the row operator only after the possible name and before any data.
An operator at the start of a row, or after data has begun, is data. A
binary-only operator at the start of a root or `()` row is permitted as a lone
data token, but another item in that row makes the missing name a critical
`err_expected_identifier`. A leading registered operator in `{}` immediately
produces `err_expected_identifier`. A registered binary row operator after a
name without subsequent data produces `err_missing_value`; a postfix-only
operator may end an expression. These checks depend on the operator's role,
never on whether `add_default_operator()` registered it
or on its spelling. An operator also registered as prefix can start a tuple
expression such as `-a`. In data-driven `[]` rows, every scalar and operator
is data. A block opener is a data item, never a row name.

| Input | Ordinary-profile result |
| --- | --- |
| `{value}` | Warning `warn_object_without_value`; syntactically accepted |
| `{=}` or `{= =}` | Critical `err_expected_identifier` |
| `(=)` | One data token; valid |
| `(= =)` or `(= a)` | Critical `err_expected_identifier` |
| `[=]` or `[= =]` | One array row of data tokens; valid |

More `=` tokens separated only by spaces stay in that same row and follow the
same rule. A comma or LF starts a new row. If `:` is registered instead as a
binary operator, `{:}`, `(: a)`, and `a:` receive the same corresponding
diagnostics as `{=}`, `(= a)`, and `a=`.

This distinction is lexical and structural, not a schema: `(1 2)` has one
tuple row whose parser events designate `1` as its possible name and `2` as
data, while `(1,2)` has two rows containing one data token each. A consumer
with a positional C++ schema may use the first spelling positionally. The
source row structure remains available to generic tools.

Each row inside `[]` is one array element, even if it contains several data
items. Thus `[1 2]` has one element row and `[1,2]` has two. Nesting preserves
this distinction:

```tavl
matrix=[[1 2],[3 4]]  // two outer rows; each inner [] has one row
matrix=[[1,2],[3,4]]  // two outer rows; each inner [] has two rows
```

An empty line adds no row. An empty comma-separated position produces
`warn_empty_row`; a trailing comma is allowed. An object-like row with a
name and no data, such as `{value}`, produces `warn_object_without_value`;
this warning does not make the document invalid. `{=}` has a different problem:
its operator has no name. A binary row operator ending without data, such as
`a=`, is a critical `err_missing_value` in the root,
`{}`, and `()`. Write `a=null` to express an explicit empty value. In `[]`,
`a` and `=` are separate data tokens and this error does not apply.

## 4. Names, duplicates, and validity

For name comparison in the ordinary data profile, a bare scalar and a quoted
string with the same decoded text denote the same name: `a` and `"a"` match.
The parser retains every row, including duplicate names. The generic CLI
reports `err_duplicate_field` for duplicate decoded names within one `{}` or
`()` block. Root rows may repeat, allowing records with the same timestamp.
A C++ aggregate reports the same error when a destination field is filled
more than once. These are profile and consumer rules, not a loss of source rows.

`tavl::token` contains a source span, not an owned copy of its spelling. A
stored token can be resolved with `parser::content()` only while the parser
still retains every byte of that span. In a streaming reader, copy the text or
finish using tokens from a row before releasing that row with
`release_before()` / `release_consumed()`. These release functions do not track
tokens stored by application code. `parser::clear()` discards all old source
text. A `std::string_view` returned by `content()` may also be invalidated by
a later `flush()` even when its source span remains resolvable.

Critical diagnostics include mismatched or unclosed brackets, unterminated
strings and block comments, invalid UTF-8, forbidden literal controls,
invalid double-quoted escapes, a binary row operator without data, a leading
operator where a name is required, and the implementation's depth limit. The
parser continues emitting events
where recovery is possible. `unrecognized` is not a critical parser error.
The CLI exits with status 2 on a critical input diagnostic and 0 on warnings
alone. Semantic validation against an application C++ type is separate from
generic syntax checking.

## 5. Conformance artifacts

The checked-in [format corpus](../tests/conformance/format.tsv) fixes source
bytes, operator profile, exact event and token sequence, byte spans, and
diagnostics. Its runner compares whole-file input with one-, two-, and
seven-byte chunks, releasing consumed input after each event. The
[CLI corpus](../tests/conformance/cli_check.tsv) fixes ordinary `check` exit
statuses and required diagnostics. The [numeric vectors](../tests/conformance/numeric.tsv)
fix decimal-to-binary64 bits and
binary64-to-canonical-text separately; the test does not derive either oracle
from the opposite conversion. Together the corpora exercise the lexical,
structural, recovery, and numeric rules with fixed results suitable for
different compilers and platforms. The rules above remain the authority for
inputs outside the finite corpus.
