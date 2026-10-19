#include "tavl/detail.h"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace tavl::detail {

namespace {

static_assert(sizeof(double) == sizeof(uint64_t) &&
              std::numeric_limits<double>::is_iec559 &&
              std::numeric_limits<double>::digits == 53,
              "TAVL finite floats require IEEE 754 binary64");

constexpr uint64_t sign_bit = uint64_t{1} << 63;
constexpr uint64_t fraction_mask = (uint64_t{1} << 52) - 1;
constexpr uint64_t max_finite_bits = 0x7fefffffffffffffULL;

bool ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }

std::string decimal_unsigned(uint64_t value) {
  char reversed[20];
  size_t count = 0;
  do {
    reversed[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  std::string result;
  result.reserve(count);
  while (count) result.push_back(reversed[--count]);
  return result;
}

std::string decimal_signed(int value) {
  if (value < 0) return "-" + decimal_unsigned(static_cast<unsigned>(-value));
  return decimal_unsigned(static_cast<unsigned>(value));
}

}  // namespace

// Little-endian base 2^32. The parser supplies reusable storage for all
// temporaries involved in rounding a decimal token.
using big_uint = float_big_uint;

big_uint big_uint::from_u64(uint64_t value) {
  big_uint result;
  if (value >> 32)
    result.words = {static_cast<uint32_t>(value),
                    static_cast<uint32_t>(value >> 32)};
  else
    result.words[0] = static_cast<uint32_t>(value);
  return result;
}

void big_uint::reset(uint32_t value) {
  words.clear();
  words.push_back(value);
}

void big_uint::copy_from(const big_uint &other) {
  words.assign(other.words.begin(), other.words.end());
}

void big_uint::normalize() {
  while (words.size() > 1 && words.back() == 0)
    words.pop_back();
}

void big_uint::mul_small(uint32_t factor) {
  uint64_t carry = 0;
  for (uint32_t &word : words) {
    const uint64_t product = uint64_t(word) * factor + carry;
    word = static_cast<uint32_t>(product);
    carry = product >> 32;
  }
  if (carry)
    words.push_back(static_cast<uint32_t>(carry));
  normalize();
}

void big_uint::add_small(uint32_t value) {
  uint64_t carry = value;
  for (uint32_t &word : words) {
    if (!carry)
      break;
    const uint64_t sum = uint64_t(word) + carry;
    word = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  if (carry)
    words.push_back(static_cast<uint32_t>(carry));
}

void big_uint::add(const big_uint &other) {
  if (words.size() < other.words.size())
    words.resize(other.words.size(), 0);
  uint64_t carry = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    const uint64_t sum = uint64_t(words[i]) +
                         (i < other.words.size() ? other.words[i] : 0) + carry;
    words[i] = static_cast<uint32_t>(sum);
    carry = sum >> 32;
  }
  if (carry)
    words.push_back(static_cast<uint32_t>(carry));
}

void big_uint::shift_left(unsigned bits) {
  if (words.size() == 1 && words[0] == 0)
    return;
  const size_t whole = bits / 32;
  const unsigned part = bits % 32;
  words.insert(words.begin(), whole, 0);
  if (!part)
    return;
  uint64_t carry = 0;
  for (size_t i = whole; i < words.size(); ++i) {
    const uint64_t shifted = (uint64_t(words[i]) << part) | carry;
    words[i] = static_cast<uint32_t>(shifted);
    carry = shifted >> 32;
  }
  if (carry)
    words.push_back(static_cast<uint32_t>(carry));
}

void big_uint::mul_u64(uint64_t factor, big_uint &upper) {
  const uint32_t high = static_cast<uint32_t>(factor >> 32);
  if (!high) {
    mul_small(static_cast<uint32_t>(factor));
    return;
  }
  upper.copy_from(*this);
  mul_small(static_cast<uint32_t>(factor));
  upper.mul_small(high);
  upper.shift_left(32);
  add(upper);
}

uint32_t big_uint::div_small(uint32_t divisor) {
  uint64_t remainder = 0;
  for (size_t i = words.size(); i-- > 0;) {
    const uint64_t current = (remainder << 32) | words[i];
    words[i] = static_cast<uint32_t>(current / divisor);
    remainder = current % divisor;
  }
  normalize();
  return static_cast<uint32_t>(remainder);
}

std::string big_uint::decimal() const {
  if (words.size() == 1 && words[0] == 0)
    return "0";
  big_uint remaining = *this;
  std::vector<uint32_t> chunks;
  while (remaining.words.size() != 1 || remaining.words[0] != 0)
    chunks.push_back(remaining.div_small(1'000'000'000));
  std::string result = decimal_unsigned(chunks.back());
  for (size_t i = chunks.size() - 1; i-- > 0;) {
    const std::string chunk = decimal_unsigned(chunks[i]);
    result.append(9 - chunk.size(), '0');
    result += chunk;
  }
  return result;
}

int big_uint::compare(const big_uint &other) const noexcept {
  if (words.size() != other.words.size())
    return words.size() < other.words.size() ? -1 : 1;
  for (size_t i = words.size(); i-- > 0;) {
    if (words[i] != other.words[i])
      return words[i] < other.words[i] ? -1 : 1;
  }
  return 0;
}

namespace {

struct dyadic {
  uint64_t significand;
  int exponent;
};

dyadic from_bits(uint64_t bits) noexcept {
  const uint64_t exponent = (bits >> 52) & 0x7ff;
  const uint64_t fraction = bits & fraction_mask;
  if (exponent == 0) return {fraction, -1074};
  return {(uint64_t{1} << 52) | fraction, static_cast<int>(exponent) - 1075};
}

dyadic midpoint(uint64_t lower_bits, uint64_t upper_bits) noexcept {
  const dyadic lower = from_bits(lower_bits);
  const dyadic upper = from_bits(upper_bits);
  const int base = lower.exponent < upper.exponent
    ? lower.exponent : upper.exponent;
  return {
    (lower.significand << (lower.exponent - base)) +
    (upper.significand << (upper.exponent - base)),
    base - 1
  };
}

// Compare N/D to significand * 2^exponent without any floating arithmetic.
int compare_to_dyadic(const big_uint& n, const big_uint& d,
                      uint64_t significand, int exponent,
                      float_parse_workspace& workspace) {
  workspace.left.copy_from(n);
  workspace.right.copy_from(d);
  workspace.right.mul_u64(significand, workspace.upper);
  if (exponent >= 0) workspace.right.shift_left(static_cast<unsigned>(exponent));
  else workspace.left.shift_left(static_cast<unsigned>(-exponent));
  return workspace.left.compare(workspace.right);
}

uint64_t rounded_bits(const big_uint& n, const big_uint& d,
                      float_parse_workspace& workspace) {
  uint64_t low = 0;
  uint64_t high = max_finite_bits;
  while (low < high) {
    const uint64_t middle = low + (high - low + 1) / 2;
    if (const auto x = from_bits(middle);
        compare_to_dyadic(n, d, x.significand, x.exponent, workspace) >= 0)
      low = middle;
    else
      high = middle - 1;
  }

  if (low == max_finite_bits) {
    // The nearest-rounding overflow threshold is maxFinite + half an ulp.
    return compare_to_dyadic(n, d, (uint64_t{1} << 54) - 1, 970, workspace) >= 0
      ? 0 : max_finite_bits;
  }

  const dyadic center = midpoint(low, low + 1);
  const int relation = compare_to_dyadic(
    n, d, center.significand, center.exponent, workspace);
  if (relation < 0) return low;
  if (relation > 0) return low + 1;
  return (low & 1) ? low + 1 : low;  // midpoint: even last bit wins
}

struct decimal_value {
  std::string digits;  // no leading or trailing zeros
  int exponent;        // value = digits * 10^exponent
};

decimal_value exact_decimal(uint64_t bits) {
  const dyadic value = from_bits(bits);
  big_uint integer = big_uint::from_u64(value.significand);
  int exponent = 0;
  if (value.exponent >= 0) {
    integer.shift_left(static_cast<unsigned>(value.exponent));
  } else {
    exponent = value.exponent;
    for (int i = 0; i < -value.exponent; ++i) integer.mul_small(5);
  }
  std::string digits = integer.decimal();
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    ++exponent;
  }
  return {std::move(digits), exponent};
}

uint64_t nearest_digits(std::string_view exact, size_t count) noexcept {
  uint64_t result = 0;
  const size_t used = count < exact.size() ? count : exact.size();
  for (size_t i = 0; i < used; ++i)
    result = result * 10 + static_cast<unsigned>(exact[i] - '0');
  if (count >= exact.size()) return result;
  const char next = exact[count];
  bool tail = false;
  for (size_t i = count + 1; i < exact.size(); ++i)
    if (exact[i] != '0') { tail = true; break; }
  if (next > '5' || (next == '5' && (tail || (result & 1)))) ++result;
  return result;
}

bool rounds_to_bits(uint64_t coefficient, int decimal_exponent, uint64_t bits,
                    float_parse_workspace& workspace) {
  auto& numerator = workspace.numerator;
  auto& denominator = workspace.denominator;
  numerator.copy_from(big_uint::from_u64(coefficient));
  denominator.reset(1);
  if (decimal_exponent >= 0)
    for (int i = 0; i < decimal_exponent; ++i) numerator.mul_small(10);
  else
    for (int i = 0; i < -decimal_exponent; ++i) denominator.mul_small(10);

  const dyadic lower = midpoint(bits - 1, bits);
  const dyadic upper = bits == max_finite_bits
    ? dyadic{(uint64_t{1} << 54) - 1, 970}
    : midpoint(bits, bits + 1);
  const int lower_relation = compare_to_dyadic(
    numerator, denominator, lower.significand, lower.exponent, workspace);
  const int upper_relation = compare_to_dyadic(
    numerator, denominator, upper.significand, upper.exponent, workspace);
  return (lower_relation > 0 || (lower_relation == 0 && !(bits & 1))) &&
         (upper_relation < 0 || (upper_relation == 0 && !(bits & 1)));
}

std::string candidate_text(uint64_t coefficient, int exponent) {
  while (coefficient % 10 == 0) {
    coefficient /= 10;
    ++exponent;
  }
  const std::string digits = decimal_unsigned(coefficient);
  const int point = static_cast<int>(digits.size()) + exponent;
  std::string fixed;
  if (point <= 0) {
    fixed = "0.";
    fixed.append(static_cast<size_t>(-point), '0');
    fixed += digits;
  } else if (point >= static_cast<int>(digits.size())) {
    fixed = digits;
    fixed.append(static_cast<size_t>(point) - digits.size(), '0');
    fixed += ".0";
  } else {
    fixed = digits.substr(0, point) + "." + digits.substr(point);
  }

  std::string scientific(1, digits.front());
  if (digits.size() > 1) {
    scientific += '.';
    scientific += digits.substr(1);
  }
  scientific += 'e';
  scientific += decimal_signed(point - 1);
  return fixed.size() <= scientific.size() ? fixed : scientific;
}

} // namespace

float_parse_result parse_float_number(std::string_view text, double& output,
                                      float_parse_workspace& workspace) {
  if (text.empty()) return float_parse_result::invalid;
  bool negative = false;
  if (text.front() == '+' || text.front() == '-') {
    negative = text.front() == '-';
    text.remove_prefix(1);
  }
  if (text.empty()) return float_parse_result::invalid;

  if (text == "nan" || text == "NAN" || text == "NaN") {
    output = std::bit_cast<double>(0x7ff8000000000000ULL);
    return float_parse_result::valid;
  }
  if (text == "inf" || text == "INF") {
    output = std::bit_cast<double>(0x7ff0000000000000ULL |
                                    (negative ? sign_bit : 0));
    return float_parse_result::valid;
  }

  bool suffix = false;
  if (text.back() == 'f' || text.back() == 'F' ||
      text.back() == 'd' || text.back() == 'D') {
    suffix = true;
    text.remove_suffix(1);
  }
  if (text.empty()) return float_parse_result::invalid;
  if (!ascii_digit(text.front()) && text.front() != '.')
    return float_parse_result::invalid;
  if (!suffix && text.find_first_of(".eE") == std::string_view::npos)
    return float_parse_result::invalid;

  size_t pos = 0;
  auto& digits = workspace.digits;
  digits.clear();
  digits.reserve(text.size());
  while (pos < text.size() && ascii_digit(text[pos])) digits.push_back(text[pos++]);
  bool point = false;
  size_t fractional_digits = 0;
  if (pos < text.size() && text[pos] == '.') {
    point = true;
    ++pos;
    while (pos < text.size() && ascii_digit(text[pos])) {
      digits.push_back(text[pos++]);
      ++fractional_digits;
    }
  }
  if (digits.empty()) return float_parse_result::invalid;

  bool exponent_present = false;
  int64_t decimal_exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    exponent_present = true;
    ++pos;
    bool exponent_negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
      exponent_negative = text[pos++] == '-';
    if (pos == text.size() || !ascii_digit(text[pos]))
      return float_parse_result::invalid;
    // Exponents larger than this are decided by the decimal magnitude check.
    while (pos < text.size() && ascii_digit(text[pos])) {
      if (decimal_exponent < 1'000'000'000)
        decimal_exponent = decimal_exponent * 10 + (text[pos] - '0');
      ++pos;
    }
    if (exponent_negative) decimal_exponent = -decimal_exponent;
  }
  if (pos != text.size() || (!point && !exponent_present && !suffix))
    return float_parse_result::invalid;

  const size_t first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    output = std::bit_cast<double>(negative ? sign_bit : uint64_t{0});
    return float_parse_result::valid;
  }
  digits.erase(0, first_nonzero);
  decimal_exponent -= static_cast<int64_t>(fractional_digits);
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    ++decimal_exponent;
  }

  const int64_t adjusted_exponent =
    decimal_exponent + static_cast<int64_t>(digits.size()) - 1;
  if (adjusted_exponent >= 309 || adjusted_exponent <= -325)
    return float_parse_result::out_of_range;

  auto& numerator = workspace.numerator;
  numerator.reset(0);
  for (const char digit : digits) {
    numerator.mul_small(10);
    numerator.add_small(static_cast<uint32_t>(digit - '0'));
  }
  auto& denominator = workspace.denominator;
  denominator.reset(1);
  if (decimal_exponent >= 0)
    for (int64_t i = 0; i < decimal_exponent; ++i) numerator.mul_small(10);
  else
    for (int64_t i = 0; i < -decimal_exponent; ++i) denominator.mul_small(10);

  const uint64_t bits = rounded_bits(numerator, denominator, workspace);
  if (bits == 0) return float_parse_result::out_of_range;
  output = std::bit_cast<double>(bits | (negative ? sign_bit : uint64_t{0}));
  return float_parse_result::valid;
}

float_parse_result parse_float_number(std::string_view text, double& output) {
  float_parse_workspace workspace;
  return parse_float_number(text, output, workspace);
}

bool is_float_number(const std::string_view& text, double& output) {
  return parse_float_number(text, output) == float_parse_result::valid;
}

double integer_to_binary64(uint64_t magnitude, bool negative) noexcept {
  const uint64_t sign = negative ? sign_bit : 0;
  if (magnitude == 0) return std::bit_cast<double>(sign);

  int exponent = static_cast<int>(std::bit_width(magnitude)) - 1;
  const int shift = exponent - 52;
  uint64_t significand = shift <= 0
    ? magnitude << -shift : magnitude >> shift;
  if (shift > 0) {
    const uint64_t remainder = magnitude & ((uint64_t{1} << shift) - 1);
    const uint64_t midpoint = uint64_t{1} << (shift - 1);
    if (remainder > midpoint || (remainder == midpoint && (significand & 1)))
      ++significand;
  }
  if (significand == (uint64_t{1} << 53)) {
    significand >>= 1;
    ++exponent;
  }
  return std::bit_cast<double>(sign |
    (uint64_t(exponent + 1023) << 52) | (significand & fraction_mask));
}

std::string format_float_canonical(double value) {
  const uint64_t raw = std::bit_cast<uint64_t>(value);
  const bool negative = (raw & sign_bit) != 0;
  const uint64_t bits = raw & ~sign_bit;
  if (bits == 0) return negative ? "-0.0" : "0.0";
  if ((bits >> 52) == 0x7ff)
    return (bits & fraction_mask) ? "nan" : (negative ? "-inf" : "inf");

  const decimal_value exact = exact_decimal(bits);
  const int adjusted = static_cast<int>(exact.digits.size()) + exact.exponent - 1;
  const size_t max_precision = exact.digits.size() < 17
    ? exact.digits.size() : 17;
  float_parse_workspace workspace;
  for (size_t precision = 1; precision <= max_precision; ++precision) {
    const uint64_t rounded = nearest_digits(exact.digits, precision);
    const int exponent = adjusted - static_cast<int>(precision) + 1;
    for (const int delta : {0, -1, 1}) {
      const uint64_t coefficient = delta < 0 ? rounded - 1
        : rounded + static_cast<uint64_t>(delta);
      if (coefficient != 0 && rounds_to_bits(coefficient, exponent, bits, workspace)) {
        std::string result = candidate_text(coefficient, exponent);
        if (negative) result.insert(result.begin(), '-');
        return result;
      }
    }
  }
  TAVL_CHECK(false, "every binary64 value has a 17-digit decimal round-trip");
  return {};
}

}  // namespace tavl::detail
