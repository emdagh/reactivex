#pragma once

#include <exception>
#include <functional>

namespace rx {
template <typename T> struct observer_impl {
  using fun_next = std::function<void(const T &)>;
  using fun_complete = std::function<void(void)>;
  using fun_error = std::function<void(std::exception_ptr)>;

  fun_next on_next = nullptr;
  fun_complete on_complete = nullptr;
  fun_error on_error = nullptr;
};

template <typename T> struct observer {
  observer_impl<T> impl;

public:
  explicit observer(const observer_impl<T> &impl) : impl(impl) {}
  observer() : impl{} {}

  virtual void next(const T &value) {
    return static_cast<const observer<T> &>(*this).next(value);
  }

  void next(const T &value) const {
    if (impl.on_next != nullptr) {
      impl.on_next(value);
    }
  }

  template <typename... Ts> void nexts(Ts &&...ts) const {
    (next(std::forward<Ts>(ts)), ...);
  }

  virtual void complete() {
    return static_cast<const observer<T> &>(*this).complete();
  }

  void complete() const {
    if (impl.on_complete != nullptr) {
      impl.on_complete();
    }
  }

  void error(std::exception_ptr ep) const {
    if (impl.on_error) {
      impl.on_error(ep);
    }
  }

  virtual void error(std::exception_ptr ep) {
    return static_cast<const observer<T> &>(*this).error(ep);
  }
};
} // namespace rx