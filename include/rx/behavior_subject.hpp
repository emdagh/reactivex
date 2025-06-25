#pragma once

#include "observable.hpp"
#include "observer.hpp"
#include "subject.hpp"
#include "subscription.hpp"
#include <mutex>
#include <optional>

namespace rx {
template <typename T> class behavior_subject : public subject<T> {
  std::optional<T> _current;
  std::mutex _mtx;
  bool _is_terminated = false;
  // std::vector<rx::observer<T>> _lst;
  virtual subscription subscribe_impl(const observer<T> &obs) override {
    std::lock_guard<std::mutex> lock(_mtx);
    if (_is_terminated) {
      obs.complete();
      return subscription{};
    }
    if (_current.has_value()) {
      obs.next(_current.value());
    }
    return subject<T>::subscribe_impl(obs);
  }

public:
  explicit behavior_subject(const T &t) : subject<T>() { _current = t; }
  virtual ~behavior_subject() {}
  virtual void next(const T &t) {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _current = t;
    }
    subject<T>::next(t);
  }

  const std::optional<T> &get_value() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _current;
  }
};
} // namespace rx