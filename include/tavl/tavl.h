#pragma once

/**
 * Зонтичный заголовок tavl: тянет все слои библиотеки одним include.
 *   common      - типы/енумы (token/event/error/node), X-macro списки
 *   detail      - утилиты разбора (классификация символов, предикаты типов, unescape)
 *   lexer       - char_storage + lexer (байты -> токены)
 *   parser      - parser (токены -> события), to_* преобразования
 *   type_traits - трейты типов для deserialize
 *   ext         - расширения поверх событий: сборка AST (make_pair_ast/make_tag_ast/make_math_ast)
 *   deserialize - заполнение C++ структуры из потока событий
 *   serialize   - обратное к deserialize: текстовое представление C++ структуры
 *
 * Парсер устойчив к частичному вводу: принимаем пачку символов (flush) и
 * выдаём события (poll_event) - begin object/tuple/array/row, got_token и т.д.
 * Абстракция строки (row): идентификатор + оператор + последовательные данные;
 * запятая или новая строка заканчивают строку. Строки живут в блоках
 * array/object/tuple, по умолчанию есть глобальный tuple.
 */

#include "tavl/common.h"
#include "tavl/detail.h"
#include "tavl/lexer.h"
#include "tavl/parser.h"
#include "tavl/type_traits.h"
#include "tavl/ext.h"
#include "tavl/deserialize.h"
#include "tavl/serialize.h"
