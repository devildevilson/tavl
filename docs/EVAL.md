# `tavl eval` language specification

This document specifies the query and edit language accepted by `tavl eval`.
`eval` parses its program with TAVL's parser. Its operator profile adds `|`,
`==`, `!=`, `<>`, `<`, `>`, `<=`, and `>=` to the default `=` operator. These
operators are local to the query parser; `tavl check` still uses the default
data profile. The query uses TAVL blocks and row delimiters, so shell quoting
only keeps the program in one argument.

```sh
tavl eval 'where(date(@name) == 1999-01-01) | where(time(@name) > 04:04:04) | edit(@data[0]) {
    set(.abc, reviewed),
    del(.reason),
    where(exist(.def)) | edit(.def[0]) { set(.x, ok) }
  }' logs.tavl > result.tavl
```

Each top-level input row is one record. `@name` refers to its name token;
`@data[0]` is its first data item, irrespective of whether `=` was written.
`@data` is the whole data sequence, including when it is written without
brackets. For example, `append(@data, 30)` changes `b = 10 20` to
`b = 10 20 30`, and `insert(@data, 1, 15)` gives `b = 10 15 20`.
Use `count(@data[*])` to count its items.
`append(@data[0], 30)` instead appends inside the first item when it is a
bracket block.
In `1999-01-01_06:54:32 { abc = fail }`, the first item is the `{}` block. An
`edit` stage enters the selected bracket block for its body. Paths such as
`.abc` then refer to named rows of that block; `.def[0]` selects the first
data item of its `def` row. `@name` and `@data[index]` operate on a row focus.
Inside an `edit` block, the focus is the block, so named paths are normally
used there.

`[*]` selects every immediate element of an array or tuple, or every row of
an object. `@data[*]` selects every data item of a row. The selector works at
any depth and can be followed by another path segment; `[*][*]` crosses two
levels. `.` means the current
focus. For example, each of these rows can be transformed without knowing
how many children it contains:

```tavl
matrix = [[1], [2], [3]]
matrix = ([1], [2], [3])
matrix = { a = [1], b = [2], c = [3] }
```

```sh
tavl eval 'edit(@data[0][*]) { append(., 4) }' matrix.tavl
```

An array element is a complete row, separated by a comma or newline. Thus
`[1 2]` has one element row with two data values, and `[1,2]` has two element
rows. `count(@data[0][*])` returns 1 and 2 respectively. A tuple remains a
positional sequence, so `(1 2)` and `(1,2)` expose the same two positions.

For arrays or tuples, each inner `[1]` becomes `[1, 4]`; for the object,
each named field's array gains `4`. `set(@data[0][*][0], 9)` and
`del(@data[0][*])` likewise operate on all matches. Without `[*]`, an edit
path that matches more than one value remains an ambiguity error. A wildcard
that matches nothing causes `edit` and `del` to do nothing; `set` and
`append` report a missing target. `edit` requires each selected value to be
a bracket block. Comparisons require a unique scalar; `exist(path)` can test
whether any wildcard match exists.

When `[*]` visits the rows of an object, `@name` refers to each visited row.
For example, `edit(@data[0][*]) { where(@name == a) | append(., 4) }`
changes only the `a` field's bracket block.
When `[*]` visits an array, `@name` is the current zero-based row index as a
number. For example, `edit(@data[0][*]) { where(@name == 1) | set(.x, 9) }`
changes the second object in `items=[{x=1},{x=2}]`.

A comma or newline ends an execution step. Stages within a step are joined
by `|`. If `where` is false, the remaining stages in that step are skipped.
At the root, a row is emitted if at least one root step completes; inside
`edit`, the next step after a comma still runs. `where(exist(.def)) | edit(...)` therefore guards
only that edit. A missing `edit` or `del` path is a no-op; a missing `set`
path is an error. Ambiguous paths are errors.

Supported expressions are a path, a TAVL scalar, `date(expr)`, `time(expr)`,
`exist(path)`, `count(path)`, `any(path, condition)`, `all(path, condition)`,
or one comparison between two expressions. The condition is evaluated with
each matched element as the current focus. For example:

```sh
tavl eval 'where(count(@data[0][*]) > 2) | where(any(@data[0][*], .score > 9))' groups.tavl
```

`any(path)` and `all(path)` without a condition test the truth of each
selected value. `any` is false and `all` is true when the path has no
matches. `count` is zero in that case. `date` extracts
the written `YYYY-MM-DD` fields. `time` extracts `HH:MM:SS` and compares
fractional seconds at nine-digit precision; an absent fraction is zero.
Neither function converts timezones or validates a civil calendar. Numeric
tokens and `count` compare numerically; booleans compare as booleans; other scalars compare their text
values in lexical order. `exist` tests whether a path has a match. A direct
comparison of a wildcard path with several matches does not reduce the
values; use `any`, `all`, or `count` to state the intended reduction.
The query profile supports all six comparisons: `>`, `<`, `==`, `!=`, `>=`,
and `<=`. It does not define arithmetic operators.

The stages are `where(condition)`, `edit(path) { steps }`,
`set(path, TAVL-data)`, `append(path, TAVL-rows)`,
`insert(path, index, TAVL-rows)`, `upsert(path, TAVL-data)`, and `del(path)`.
`append` adds rows to a selected `{}`, `()`, or `[]` block. `set` requires
an existing target and one valid TAVL data fragment. `insert` uses a zero-based
position among array rows, tuple positions, or object rows. An index equal to the length appends, and a larger
index is an error. `upsert` replaces an existing named field or appends it to
an object. It creates only the final field: the parent object must already
exist, and duplicate matching field names in `{}` or `()` cause the critical
`err_duplicate_field` diagnostic (CLI exit status 2). Both stages
accept `[*]` in the path to operate on several blocks. For example:

```sh
tavl eval 'insert(@data[0][*], 1, 9) | upsert(@data[1].status, reviewed)' rows.tavl
```

Query paths support `.field`, `[index]`, `[*]`, `@data[index]`, and `.`;
`@name` is readable in conditions. Programs are compiled once. Input is read
in chunks, but a complete row is buffered while it is
evaluated. Normal output is newline-delimited TAVL rows. `-f` waits for
newly appended complete rows; it does not handle file rotation or truncation.

Output style is explicit when needed:

```sh
tavl eval --output=compact 'where(.status == ok)' logs.tavl > compact.tavl
tavl eval --output=pretty --names-depth=1 'set(@data[0].status, reviewed)' logs.tavl > positional.tavl
tavl format --output=pretty compact.tavl > readable.tavl
```

The default `eval` output preserves untouched source text and comments, though
edits may add separators around changed values. Compact output emits one input
record per line and removes decorative whitespace. A single space remains
between adjacent scalar tokens when needed to preserve their boundary.
Pretty output uses two spaces per bracket depth. `format` (also `fmt`) applies
these modes to an existing file or stdin and defaults to pretty; use it after
other CLI edits via a pipe. Both commands accept `-f` with a regular file.
`--names-depth=N` keeps field names only at depths strictly below `N`, the
same rule as `tavl::sopts.names_depth`: `0` drops names at the root too, and
`1` keeps root names while making nested structures positional. Objects whose
field names are dropped become `()` tuples. A bare scalar in a positional
array or tuple remains a value. Since a row without `=` can be either a named
field or a positional sequence, the formatter treats its first token as a
name in root and object blocks when the row has subsequent data. Explicit
formatting regenerates data and omits comments.

Exit status is 0 on success, 1 for invalid program/usage, 2 for input or
parse errors, 3 for a missing target or out-of-range insert index, and 4 for
an ambiguous or invalid block target. A stream may already have emitted
earlier rows when a later row fails.
