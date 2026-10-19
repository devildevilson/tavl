#include "tavl/parser.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

namespace {
bool count_allocations = false;
size_t allocation_count = 0;
}

void* operator new(std::size_t size) {
  if (count_allocations) ++allocation_count;
  if (void* ptr = std::malloc(size ? size : 1)) return ptr;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  if (count_allocations) ++allocation_count;
  if (void* ptr = std::malloc(size ? size : 1)) return ptr;
  throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

int main() {
  {
    tavl::char_storage storage;
    storage.append("abcdef");
    storage.release_before(2);
    if (storage.size() != 6 || storage.buffer_size() != 4 ||
        storage.resolve({2, 4}) != "cdef") return 1;
    storage.append("ghijklmnopqrstuvwxyz"); // compacts the released prefix
    if (storage.resolve({2, 24}) != "cdefghijklmnopqrstuvwxyz") return 1;
    storage.release_before(25);
    storage.append("0123456789");
    if (storage.resolve({25, 11}) != "z0123456789") return 1;
  }

  tavl::parser parser;
  parser.add_default_operator();
  std::string input;
  for (int i = 0; i < 128; ++i)
    input += "value = 1.2345678901234567e-123\n";

  const auto parse = [&]() {
    parser.clear();
    parser.flush(input);
    parser.finish();
    size_t values = 0;
    for (;;) {
      const auto [event, error] = parser.poll_event();
      if (error.is_critical()) return false;
      if (event.type == tavl::event_type::got_token &&
          event.token.type == tavl::token_type::number_float) {
        if (!parser.to_float(event.token)) return false;
        ++values;
      }
      if (event.type == tavl::event_type::eof) break;
    }
    return values == 128;
  };

  const auto parse_stream = [&]() {
    parser.clear();
    constexpr std::string_view row = "value = 1.2345678901234567e-123\n";
    size_t values = 0;
    for (int i = 0; i < 128; ++i) {
      parser.flush(row);
      for (;;) {
        const auto [event, error] = parser.poll_event();
        if (error.is_critical()) return false;
        if (event.type == tavl::event_type::got_token &&
            event.token.type == tavl::token_type::number_float) {
          if (!parser.to_float(event.token)) return false;
          ++values;
        }
        if (event.type == tavl::event_type::row_end)
          parser.release_before(parser.consumed_offset());
        if (event.type == tavl::event_type::not_enought_data) break;
      }
    }
    parser.finish();
    return std::get<0>(parser.poll_event()).type == tavl::event_type::eof && values == 128;
  };

  if (!parse() || !parse_stream()) return 1; // establish all buffer capacities
  count_allocations = true;
  const bool parsed = parse() && parse_stream();
  count_allocations = false;
  if (!parsed || allocation_count != 0) {
    std::fprintf(stderr, "parser allocated %zu times after warmup\n", allocation_count);
    return 1;
  }
  return 0;
}
