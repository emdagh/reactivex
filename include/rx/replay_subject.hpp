#pragma once

#include <deque>

namespace rx {
template <typename T> class replay_subject : public subject<T> {
  size_t _len;
  std::deque<T> _q;
  std::mutex _mtx;
  bool _is_terminated = false;
  bool _is_completed = false;

  subscription subscribe_impl(const observer<T> &obs) override {
    std::lock_guard<std::mutex> lock(_mtx);

    for (auto &item : _q) {
      obs.next(item);
    }
    if (_is_terminated) {
      return subscription{};
    } else if (_is_completed) {
      obs.complete();
      return subscription{};
    }
    return subject<T>::subscribe_impl(obs);
  }

public:
  replay_subject(size_t buf_len) : subject<T>(), _len(buf_len) {}

  virtual ~replay_subject() {}

  virtual void next(const T &t) override {
    {
      std::lock_guard<std::mutex> lock(_mtx);
      _q.push_back(t);
      if (_q.size() > _len) {
        _q.pop_front();
      }
    }

    subject<T>::next(t);
  }

  void complete() override {
    // Protect shared state
    std::lock_guard<std::mutex> lock(_mtx);
    if (_is_terminated) {
      return;
    } // Atomically set and check

    _is_completed = true;
    subject<T>::complete(); // Delegate to base Subject's complete method
  }
};
} // namespace rx