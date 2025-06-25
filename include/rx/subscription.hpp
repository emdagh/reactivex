#pragma once

namespace rx {
class subscription {
  using unsubscribe_fun = std::function<void(void)>;
  unsubscribe_fun _fun;
  bool _is_disposed = false;

public:
  subscription() : _fun(nullptr) {}

  subscription(unsubscribe_fun &&fun) : _fun(std::move(fun)) {}

  virtual ~subscription() { unsubscribe(); }

  void unsubscribe() {
    if (_fun && !_is_disposed) {
      _fun();
      _is_disposed = true;
    }
  }

  bool is_active() const { return !_is_disposed; }

  bool is_disposed() const { return !is_active(); }
};
} // namespace rx