#pragma once

#include <cstddef>
#include <optional>
#include <memory>
#include <utility>
#include <tuple>
#include <array>
#include <chrono>
#include <type_traits>

// Traits for deserialize: detection of standard types (optional/unique_ptr/
// pair/tuple/std::array/char-buffer) and container categories (push_back-able,
// map vs set) by duck typing. Used in deserialize.h.
namespace tavl {

template <typename> struct ds_is_optional : std::false_type {};
template <typename U> struct ds_is_optional<std::optional<U>> : std::true_type {};
template <typename> struct ds_is_unique_ptr : std::false_type {};
template <typename U, typename D> struct ds_is_unique_ptr<std::unique_ptr<U, D>> : std::true_type {};
template <typename> struct ds_is_pair : std::false_type {};
template <typename A, typename B> struct ds_is_pair<std::pair<A, B>> : std::true_type {};
template <typename> struct ds_is_tuple : std::false_type {};
template <typename... Ts> struct ds_is_tuple<std::tuple<Ts...>> : std::true_type {};
template <typename> struct ds_is_std_array : std::false_type {};
template <typename E, size_t N> struct ds_is_std_array<std::array<E, N>> : std::true_type {};
// Fixed string buffers: std::array<char,N> and char[N].
template <typename> struct ds_is_char_array : std::false_type {};
template <size_t N> struct ds_is_char_array<std::array<char, N>> : std::true_type {};
template <size_t N> struct ds_is_char_array<char[N]> : std::true_type {};
// Back-insertable containers (vector/list/deque); std::string is handled earlier.
template <typename T, typename = void> struct ds_has_push_back : std::false_type {};
template <typename T>
struct ds_has_push_back<T, std::void_t<decltype(std::declval<T&>().push_back(std::declval<typename T::value_type>()))>> : std::true_type {};
// Map has mapped_type; set-like containers only have key_type.
template <typename T, typename = void> struct ds_is_map : std::false_type {};
template <typename T> struct ds_is_map<T, std::void_t<typename T::mapped_type, typename T::key_type>> : std::true_type {};
template <typename T, typename = void> struct ds_has_key_type : std::false_type {};
template <typename T> struct ds_has_key_type<T, std::void_t<typename T::key_type>> : std::true_type {};
// chrono time_point/duration, read from datetime tokens.
template <typename> struct ds_is_duration : std::false_type {};
template <typename R, typename P> struct ds_is_duration<std::chrono::duration<R, P>> : std::true_type {};
template <typename> struct ds_is_time_point : std::false_type {};
template <typename C, typename D> struct ds_is_time_point<std::chrono::time_point<C, D>> : std::true_type {};

}
