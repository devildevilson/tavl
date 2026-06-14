#pragma once

/**
 * The tavl umbrella header: pulls in every library layer with a single include.
 *   common      - types/enums (token/event/error/node), X-macro lists
 *   detail      - parsing utilities (character classification, type predicates, unescaping)
 *   lexer       - char_storage + lexer (bytes -> tokens)
 *   parser      - parser (tokens -> events), to_* conversions
 *   type_traits - type traits for deserialize
 *   ext         - extensions over the event stream: AST building (make_pair_ast/make_tag_ast/make_math_ast)
 *   deserialize - filling a C++ struct from the event stream
 *   serialize   - the inverse of deserialize: textual representation of a C++ struct
 *
 * The parser is resilient to partial input: you feed a chunk of characters (flush) and
 * poll for events (poll_event) - begin object/tuple/array/row, got_token, etc.
 * The row abstraction: identifier + operator + sequential data;
 * a comma or a newline ends a row. Rows live inside array/object/tuple
 * blocks, with an implicit global tuple by default.
 */

#include "tavl/common.h"
#include "tavl/detail.h"
#include "tavl/lexer.h"
#include "tavl/parser.h"
#include "tavl/type_traits.h"
#include "tavl/ext.h"
#include "tavl/deserialize.h"
#include "tavl/serialize.h"
