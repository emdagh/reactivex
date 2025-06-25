#pragma once
#include "behavior_subject.hpp"
#include "observable.hpp"
#include "observer.hpp"
#include "replay_subject.hpp"
#include "subject.hpp"

namespace rx {

template <typename Iterable> auto from_iterable(Iterable iterable);

template <typename T, typename Traits = std::char_traits<T>>
static auto from_istream(std::basic_istream<T, Traits> &iss) {
  using char_type = typename std::basic_istream<T, Traits>::char_type;
  return observable<char_type>([&iss](const observer<char_type> &obs) {
    while (!iss.eof() && !iss.bad()) {
      T value;
      iss >> value;
      if (iss.fail()) {
        obs.error();
        break;
      }
      obs.next(value);
    }
    obs.complete();
  });
}

template <typename... Ts> auto of(Ts &&...ts) {
  using T = typename std::common_type<Ts...>::type;
  return observable<T>([ts...](const observer<T> &obs) {
    std::initializer_list<T> list{(ts)...};
    for (auto i : list) {
      obs.next(i);
    }
    obs.complete();
    return subscription{};
  });
}

template <typename T> auto range(T start, T count) {
  return observable<T>([start, count](const observer<T> &obs) {
    for (T i = start; i < start + count; ++i) {
      obs.next(i);
    }
    obs.complete();
    return subscription{};
  });
}

template <typename Iterable> auto from_iterable(Iterable iterable) {
  using T = typename std::remove_reference<decltype(*iterable.begin())>::type;
  return observable<T>([iterable](const observer<T> &obs) {
    for (auto i : iterable) {
      obs.next(i);
    }
    obs.complete();
    return subscription{};
  });
}

template <typename T>
static observable<T> make_observable(
    std::function<subscription(const observer<T> &)> on_subscribe_func) {
  return observable<T>(std::move(on_subscribe_func));
}

} // namespace rx
