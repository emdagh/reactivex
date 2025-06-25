#pragma once

#include <chrono>
#include <iostream>
#include <iterator>

#define DEBUG_METHOD()                                                         \
  std::cout << __PRETTY_FUNCTION__ << "(" << __FILE__ << ":" << __LINE__       \
            << ")" << std::endl
#define DEBUG_INFO(...) print(std::cout, __VA_ARGS__)
using namespace std::literals;

template <typename Arg, typename... Args>
void print(std::ostream &out, Arg &&arg, Args &&...args) {
  out << std::forward<Arg>(arg);
  ((out << ',' << std::forward<Args>(args)), ...);
}

template <typename T>
std::ostream &operator<<(std::ostream &os, const std::vector<T> &v) {
  std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, ","));
  return os;
}
template <class C, class V>
auto append(C &container, V &&value, int)
    -> decltype(container.push_back(std::forward<V>(value)), void()) {
  container.push_back(std::forward<V>(value));
}

template <class C, class V> void append(C &container, V &&value, ...) {
  container.insert(std::forward<V>(value));
}

template <class C, class V> void append_to(C &container, V &&value) {
  append(container, std::forward<V>(value), 0);
}

template <typename, typename = void> constexpr bool is_iterable{};

template <typename T>
constexpr bool is_iterable<T, std::void_t<decltype(std::declval<T>().begin()),
                                          decltype(std::declval<T>().end())>> =
    true;
