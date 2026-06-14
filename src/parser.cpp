#include "tavl/parser.h"
#include "tavl/detail.h"

namespace tavl {

using namespace tavl::detail;

std::string_view parser::content(const source_span& span) const {
  return storage.resolve({span.offset, span.size});
}

// Приоритеты как в C++: ЧЕМ БОЛЬШЕ ЧИСЛО, ТЕМ КРЕПЧЕ связывает (обратно уровням cppreference).
// Дефолты: '=' (присваивание, самый слабый), сравнения; математику пользователь добавит сам.
void parser::add_default_operator() {
  add_operator("=", op_fixity::binary, 1, op_assoc::right);   // присваивание правоассоциативно
}

void parser::add_additional_default_operators() {
  add_operator(">",  op_fixity::binary, 10);  // отношение  (C++ <, >, <=, >=)
  add_operator("<",  op_fixity::binary, 10);
  add_operator(">=", op_fixity::binary, 10);
  add_operator("<=", op_fixity::binary, 10);
  add_operator("<>", op_fixity::binary, 9);   // неравенство (C++ == / != - слабее отношения)
  add_operator("==", op_fixity::binary, 9);
}

void parser::add_math_default_operators() {
  clear_operators();
  add_default_operator();              // '=' правоассоц. (присваивание)
  add_additional_default_operators();

  // все бинарные C++-операторы ниже левоассоциативны (дефолт op_assoc::left);
  // правоассоц. только присваивание ('=') и унарные префиксы (фиксность сама задаёт правую сторону).
  add_operator("||",  op_fixity::binary, 4);
  add_operator("&&",  op_fixity::binary, 5);
  add_operator("|",   op_fixity::binary, 6);
  add_operator("^",   op_fixity::binary, 7);
  add_operator("&",   op_fixity::binary, 8);

  //add_operator("<=>", op_fixity::binary, 11);
  add_operator("<<",  op_fixity::binary, 12);
  add_operator(">>",  op_fixity::binary, 12);
  add_operator("+",   op_fixity::binary, 13);
  add_operator("-",   op_fixity::binary, 13);
  add_operator("*",   op_fixity::binary, 14);
  add_operator("/",   op_fixity::binary, 14);
  add_operator("%",   op_fixity::binary, 14);

  add_operator("-",   op_fixity::prefix, 16);
  add_operator("+",   op_fixity::prefix, 16);
  add_operator("!",   op_fixity::prefix, 16);
  add_operator("~",   op_fixity::prefix, 16);
  add_operator("++",  op_fixity::prefix, 16);
  add_operator("--",  op_fixity::prefix, 16);
}

void parser::add_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc) {
  lexer.add_operator(op, op_info{fixity, precedence, assoc});
}

void parser::add_litteral_operator(const std::string_view& op, op_fixity fixity, int precedence, op_assoc assoc) {
  lexer.add_litteral_operator(op, op_info{fixity, precedence, assoc});
}

std::optional<op_info> parser::operator_info(const std::string_view& op, op_fixity fixity) const {
  return lexer.operator_info(op, fixity);
}

parser::parser() noexcept {
  clear();
}

void parser::clear_operators() {
  lexer.clear_operators();
}

void parser::clear() noexcept {
  storage.clear();
  lexer.clear();

  stack.clear();
  stack.emplace_back();
  events.clear();

  ignore_depth = 0;
  pending_modes = block_modes{};
}

void parser::override_next_block_modes(parse_mode object, parse_mode tuple, parse_mode array) {
  pending_modes = block_modes{true, object, tuple, array};
}

void parser::clear_pending_block_modes() {
  pending_modes = block_modes{};
}

void parser::flush(const std::string_view& data) {
  storage.append(data);
}

size_t parser::consumed_offset() const {
  size_t b = lexer.token_offset();
  for (const auto& pe : events) {
    const size_t o = pe.event.token.span.offset;
    if (o != SIZE_MAX && o < b) b = o;
  }
  return b;
}

void parser::release_before(size_t offset) {
  storage.release_before(offset);
}

static event_type end_event_for(const block_type type) noexcept {
  switch (type) {
    case block_type::object: return event_type::object_end;
    case block_type::array:  return event_type::array_end;
    case block_type::tuple:  return event_type::tuple_end;
    default: break;
  }

  return event_type::invalid;
}

// кладёт событие в очередь вместе с сопутствующей ошибкой (no_error если чисто)
void parser::emit(const event& ev, error err) {
  events.push_back({ev, err});
}

// куда указывает span предупреждения о строке: идентификатор, иначе оператор, иначе сам токен
source_span parser::row_span(const block_frame& f, const token& fallback) const {
  if (f.has_identifier()) return f.identifier.span;
  if (f.op.exists())      return f.op.span;
  return fallback.span;
}

// эмитит отложенные id/op текущей строки и закрывает её событием row_end;
// object-строку без значения помечает предупреждением
void parser::close_row(const token& end_tok) {
  auto& f = stack.back();
  if (!f.has_row()) return;

  error row_err{};
  if (f.row_mode == parse_mode::object_like && !f.has_value()) {
    row_err = error{error_type::warn_object_without_value, row_span(f, end_tok)};
  }

  if (!f.has_value()) {
    if (f.row_mode == parse_mode::object_like) {
      // в object ключ обязателен - это идентификатор/оператор строки
      if (f.has_identifier()) emit(event{event_type::got_row_identifier, f.identifier});
      if (f.op.exists())      emit(event{event_type::got_row_operator, f.op});
    } else if (f.row_mode == parse_mode::tuple_like) {
      // в tuple одинокий идентификатор/оператор без значения - это просто данные
      if (f.has_identifier()) emit(event{event_type::got_token, f.identifier});
      if (f.op.exists())      emit(event{event_type::got_token, f.op});
    }
  }

  emit(event{event_type::row_end, end_tok}, row_err);
  f.new_row();   // строка закрыта - делаем close_row идемпотентным
}

// принудительно закрывает незакрытые вложенные блоки до блока типа target;
// на каждый выкинутый блок эмитит его *_end с ошибкой на открывающую скобку
void parser::close_unmatched(block_type target, const token& closer) {
  while (stack.size() > 1 && stack.back().type != target) {
    close_row(closer);
    const auto f = stack.back();
    const auto err = error{error_type::err_bracket_missmatch, f.open_token.span};
    emit(event{end_event_for(f.type), f.open_token}, err);
    stack.pop_back();
  }
}

// закрывает блок типа target, открытый скобкой и закрываемый closer (end_ev - его событие конца):
// штатно если парный блок найден, иначе помечает closer как лишнюю закрывающую скобку
void parser::close_block(block_type target, event_type end_ev, const token& closer) {
  close_row(closer);
  close_unmatched(target, closer);
  if (stack.size() <= 1) {
    const auto err = error{error_type::err_bracket_missmatch, closer.span};
    emit(event{end_ev, closer}, err);
  } else {
    stack.pop_back();
    emit(event{end_ev, closer});
  }
  // закрытый блок может быть значением в строке родителя
  stack.back().last_value = closer;
}

// открывает блок типа bt скобкой c (begin_ev - его событие начала). Общая логика для (){}[]:
// флаш отложенных id/оператора родителя + разрешение parse_mode (с учётом подмены) + push кадра.
void parser::open_block(const token& c, block_type bt, event_type begin_ev) {
  // жёсткий лимит вложенности: глубже не идём - сигналим критической ошибкой и НЕ пушим кадр
  if (stack.size() >= limits::max_nesting) {
    emit(event{begin_ev, c}, error{error_type::err_nesting_too_deep, c.span});
    return;
  }

  error id_err{};
  // на открывающей скобке проверяем, что object-строка имеет ключ
  if (stack.back().row_mode == parse_mode::object_like &&
      !stack.back().has_value() && !stack.back().has_identifier()) {
    id_err = error{error_type::warn_expected_identifier, c.span};
  }

  if (!stack.back().has_row()) emit(event{event_type::row_begin, c});

  // блок становится значением строки - сбрасываем отложенные id/оператор
  if ((stack.back().row_mode == parse_mode::object_like ||
       stack.back().row_mode == parse_mode::tuple_like) && !stack.back().has_value()) {
    if (stack.back().has_identifier()) emit(event{event_type::got_row_identifier, stack.back().identifier});
    if (stack.back().op.exists()) emit(event{event_type::got_row_operator, stack.back().op});
  }

  // режим блока: отложенная подмена (-> блок+потомки), иначе наследованная карта родителя, иначе дефолт скобки.
  // подмена потребляется первым же открытым блоком; карта (eff) кладётся в кадр для наследования вглубь.
  const block_modes eff = pending_modes.active ? pending_modes : stack.back().modes;
  pending_modes = block_modes{};
  parse_mode pm = (bt == block_type::object) ? parse_mode::object_like
                : (bt == block_type::array)  ? parse_mode::data_driven
                :                              parse_mode::tuple_like;
  if (eff.active) pm = (bt == block_type::object) ? eff.object
                     : (bt == block_type::array)  ? eff.array
                     :                              eff.tuple;

  stack.emplace_back();
  stack.back().type = bt;
  stack.back().row_mode = pm;
  stack.back().modes = eff;
  stack.back().open_token = c;
  emit(event{begin_ev, c}, id_err);
}

// Наполняет очередь events: токенизирует ввод (lexer.lex) и диспетчеризует токены по типу,
// накапливая состояние строки в верхнем кадре stack, пока не наберётся хотя бы одно событие.
void parser::fill_events() {
  if (stack.empty() && events.empty()) {
    emit(event{event_type::eof, token{}});
  }

  while (events.empty()) {
    const auto c = lexer.lex(storage);

    // not_enought_data НЕ кэшируем в очереди: иначе peek() залипнет на нём и не перелексит после долива.
    // Выходим с пустой очередью; poll_event/peek синтезируют not_enought_data сами.
    if (c.type == token_type::not_enought_data) break;

    switch (c.type) {
        case token_type::close_paren:   close_block(block_type::tuple,  event_type::tuple_end,  c); break;
        case token_type::close_brace:   close_block(block_type::object, event_type::object_end, c); break;
        case token_type::close_bracket: close_block(block_type::array,  event_type::array_end,  c); break;

        case token_type::comma:
          // пустая строка перед запятой: синтезируем пустую строку и помечаем её
          if (!stack.back().has_row()) {
            emit(event{event_type::empty_row, c}, error{error_type::warn_empty_row, c.span});
            break;
          }
          close_row(c);
          break;

        case token_type::newline:
          close_row(c);
          break;

        case token_type::open_paren:   open_block(c, block_type::tuple,  event_type::tuple_begin);  break;
        case token_type::open_brace:   open_block(c, block_type::object, event_type::object_begin); break;
        case token_type::open_bracket: open_block(c, block_type::array,  event_type::array_begin);  break;

        // здесь достаточно проверить наличие идентификатора и отсутствие значения
        case token_type::op: {
          if (!stack.back().has_row()) {
            emit(event{event_type::row_begin, c});
          }

          if (
            (stack.back().row_mode == parse_mode::tuple_like ||
            stack.back().row_mode == parse_mode::object_like) && 
            stack.back().has_identifier() && 
            !stack.back().has_value() &&
            !stack.back().op.exists()
          ) {
            stack.back().op = c;
            break;
          }

          error id_err{};
          if (
            stack.back().row_mode == parse_mode::object_like &&
            !stack.back().has_value() &&
            !stack.back().has_identifier()
          ) {
            id_err = error{error_type::warn_expected_identifier, c.span};
          }

          // добавляем отложенные идентификатор и оператор
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) &&
            !stack.back().has_value()
          ) {
            if (stack.back().has_identifier()) {
              emit(event{event_type::got_row_identifier, stack.back().identifier});
            }

            if (stack.back().op.exists()) {
              emit(event{event_type::got_row_operator, stack.back().op});
            }
          }

          // это просто какой то идентификатор
          stack.back().last_value = c;
          emit(event{event_type::got_token, c}, id_err);
          break;
        }
        
        // тут простые проверки видимо без ошибок
        case token_type::unrecognized:
        case token_type::null:
        case token_type::boolean:
        case token_type::number_int:
        case token_type::number_uint:
        case token_type::number_float:
        case token_type::datetime:
        case token_type::identifier:
        case token_type::doublequote_string:
        case token_type::singlequote_string:
          if (!stack.back().has_row()) {
            emit(event{event_type::row_begin, c});
          }

          // первый токен-неоператор строки -> идентификатор. НО только если значения ещё не было:
          // если строка началась с оператора (унарный префикс, напр. "-a"), оператор уже стал
          // данными (got_token), и последующие токены тоже данные, а не идентификатор.
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) &&
            !stack.back().has_identifier() &&
            !stack.back().has_value()
          ) {
            stack.back().identifier = c;
            break;
          }

          // добавляем отложенные идентификатор и оператор
          if (
            (stack.back().row_mode == parse_mode::object_like ||
             stack.back().row_mode == parse_mode::tuple_like) && 
            !stack.back().has_value()
          ) {
            if (stack.back().has_identifier()) {
              emit(event{event_type::got_row_identifier, stack.back().identifier});
            }

            if (stack.back().op.exists()) {
              emit(event{event_type::got_row_operator, stack.back().op});
            }
          }

          stack.back().last_value = c;
          emit(event{event_type::got_token, c});
          break;

        case token_type::eof:
          // конец ввода: закрываем висящую строку и все незакрытые блоки,
          // каждый блок - со ссылкой на его открывающую скобку
          close_row(c);
          while (stack.size() > 1) {
            const auto f = stack.back();
            const auto err = error{error_type::err_bracket_missmatch, f.open_token.span};
            emit(event{end_event_for(f.type), f.open_token}, err);
            stack.pop_back();
            close_row(c);
          }
          stack.clear();
          emit({event_type::eof, c});
          break;

        case token_type::line_comment:
        case token_type::block_comment:
          emit({event_type::got_comment, c});
          break;

        // not_enought_data перехвачен до switch (не кэшируем); здесь недостижим, кейс - для -Wswitch
        case token_type::not_enought_data: break;

        // лексер не отдаёт invalid (битый текст -> unrecognized), count - sentinel: оба недостижимы
        case token_type::invalid:
        case token_type::count: TAVL_CHECK(false, "lexer returned invalid/count token"); break;
    }
  }
}

// Отдаёт следующее событие из очереди (с сопутствующей ошибкой), сняв его с очереди.
std::tuple<event, error> parser::poll_event() {
  fill_events();
  if (!events.empty()) {
    const auto pe = events.front();
    events.erase(events.begin());
    return {pe.event, pe.error};
  }
  return {event{event_type::not_enought_data, {}}, error{}};
}

// Не-снимающий lookahead: следующее событие без снятия с очереди (для классификации потока).
event parser::peek() {
  fill_events();
  if (!events.empty()) return events.front().event;
  return event{event_type::not_enought_data, {}};
}

// отправляем метку что инпута больше не будет
// но после этого парсер все еще может отдать несколько событий 
// (как минимум закрыть структуры + завершить строку + сгенерировать ошибки)
void parser::finish() {
  lexer.finish();
}
std::string parser::to_string(const token& t) const {
  if (!t.exists()) return std::string();

  auto cont = content(t.span);
  if (t.type != token_type::doublequote_string && t.type != token_type::singlequote_string) {
    return std::string(cont);
  }

  std::string str;
  str.reserve(cont.size());

  if (t.type == token_type::singlequote_string) {
    cont.remove_prefix(1);
    if (cont.back() == '\'') cont.remove_suffix(1);

    unescape_singlequote_string(cont, str);
    return str;
  }

  // doublequote_string - стандартные escape + \u/\U unicode
  cont.remove_prefix(1);
  if (cont.back() == '\"') cont.remove_suffix(1);

  unescape_doublequote_string(cont, str);

  return str;
}

std::optional<bool> parser::to_boolean(const token& t) const {
  if (t.type != token_type::boolean) return std::nullopt;

  auto cont = content(t.span);

  bool b;
  is_boolean(cont, b);
  return std::make_optional(b);
}

std::optional<int64_t> parser::to_int(const token& t) const {
  if (t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;

  auto cont = content(t.span);

  if (t.type == token_type::number_uint) {
    uint64_t v;
    if (is_bin_number(cont, v)) return std::make_optional(int64_t(v));
    if (is_oct_number(cont, v)) return std::make_optional(int64_t(v));
    if (is_hex_number(cont, v)) return std::make_optional(int64_t(v));
  }

  int64_t v2;
  is_dec_number(cont, v2);
  return std::make_optional(v2);
}

std::optional<uint64_t> parser::to_uint(const token& t) const {
  if (t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;
  auto cont = content(t.span);
  if (cont[0] == '-') return std::nullopt;

  if (t.type == token_type::number_uint) {
    uint64_t v;
    if (is_bin_number(cont, v)) return std::make_optional(v);
    if (is_oct_number(cont, v)) return std::make_optional(v);
    if (is_hex_number(cont, v)) return std::make_optional(v);
  }

  int64_t v2;
  is_dec_number(cont, v2);
  return std::make_optional(v2);
}

std::optional<double> parser::to_float(const token& t) const {
  if (t.type != token_type::number_float && t.type != token_type::number_int && t.type != token_type::number_uint) return std::nullopt;

  auto cont = content(t.span);

  if (t.type == token_type::number_int) {
    int64_t v2;
    is_dec_number(cont, v2);
    return std::make_optional(double(v2));
  }

  if (t.type == token_type::number_uint) {
    uint64_t v;
    if (is_bin_number(cont, v)) return std::make_optional(double(v));
    if (is_oct_number(cont, v)) return std::make_optional(double(v));
    if (is_hex_number(cont, v)) return std::make_optional(double(v));
  }

  double v;
  is_float_number(cont, v);
  return std::make_optional(v);
}

std::optional<iso_datetime> parser::to_datetime(const token& t) const {
  if (t.type != token_type::datetime) return std::nullopt;

  auto cont = content(t.span);
  iso_datetime dt;
  is_datetime(cont, dt);
  return std::make_optional(dt);
}

}
