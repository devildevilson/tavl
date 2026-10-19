# TAVL conformance vectors

These UTF-8 TSV files contain fixed expected results. Test runners should
compare results against the files, never derive expected values from another
operation of the implementation under test.

`format.tsv` has four tab-separated fields: case name, operator profile,
source bytes in lowercase hex, and the complete event sequence. Profiles are
`equal` (register binary `=`), `colon` (register binary `:`), `math` (the
library's math operator bundle), `postfix` (the math bundle plus postfix `!`),
and `none` (no registered operators). Events
are separated by `;`. Each event is
`event_type:token_type:offset:size:line:column:error_type:source_hex`.
Offset, size, line, and column count bytes; `-` denotes an unavailable offset.
The event's `source_hex` is taken from the original input at its source span.
After streaming input is released, the parser is not required to retain the
text of an earlier opener used in a later recovery event, though its span must
still identify the original bytes.

`numeric.tsv` has four tab-separated fields. A `parse` row gives input decimal
text, expected status (`valid`, `out_of_range`, or `invalid`), and the exact
binary64 bit pattern for valid input. A `format` row gives input binary64 bits,
`-`, and required canonical text. These constants are external oracles; the
test must not print a value and parse that same printout as its only check.

`cli_check.tsv` has case name, ordinary-profile source text, expected process
exit status, and a required stderr fragment (`-` means stderr is empty). The
two characters `\n` in the source field denote one LF byte. This table uses
only text that CMake can write portably; byte-level encoding cases live in
`format.tsv` and the existing CLI tests.

The C++ format runner tests each source as a whole and in chunks of one, two,
and seven bytes while releasing consumed input. Add vectors for every new
syntax or diagnostic rule, especially boundary and recovery cases. This
corpus covers the current rule classes; no finite corpus is a proof over all
possible inputs. Cross-platform confidence also requires running it on each
supported compiler and target.
