#pragma once

#include "observer.hpp"
#include "subscription.hpp"
#include "util.hpp"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

namespace rx {
template <typename T> class observable {
  using subscribe_fun = std::function<subscription(const observer<T> &)>;
  subscribe_fun _fun;

public:
  observable(subscribe_fun &&fun) : _fun(std::move(fun)) {}
  virtual ~observable() {}

  virtual subscription subscribe(const observer_impl<T> &impl) {
    observer<T> obs(impl);
    return subscribe(obs);
  }

  virtual subscription subscribe(const observer<T> &obs) { return _fun(obs); }

  template <typename F, typename U = std::invoke_result_t<F, T>>
  auto map(F &&fun) {

    return observable<U>(
        [this, fun = std::forward<F>(fun)](const observer<U> &obs) {
          return this->subscribe(
              {.on_next = [fun, obs](const T &t) { return obs.next(fun(t)); },
               .on_complete = [obs] { obs.complete(); },
               .on_error = [obs](std::exception_ptr ep) { obs.error(ep); }});
        });
  }
  template <typename F, typename U = std::invoke_result_t<F, T, T>>
  auto reduce(F &&fun, T seed = T{0}) {
    return observable<U>([this, fun = std::forward<F>(fun),
                          seed](const observer<U> &obs) -> subscription {
      struct reduce_state {
        U current;
        std::mutex mx;

        reduce_state(U seed) : current(seed) {}
      };

      auto state = std::make_shared<reduce_state>(
          seed); // make async compatible (no real way around heap here...)

      return this->subscribe(
          {.on_next =
               [obs, state, fun](const T &value) {
                 std::lock_guard<std::mutex> lock(state->mx);
                 state->current = fun(state->current, value);
               },
           .on_complete =
               [obs, state, fun] {
                 std::lock_guard<std::mutex> lock(state->mx);
                 obs.next(state->current);
                 obs.complete();
               },
           .on_error =
               [obs, state](const std::exception_ptr ep) { obs.error(ep); }});
    });
  }

  template <typename U, typename Fun> auto flat_map(Fun &&mapper) {
    return observable<U>(
        [this, mapper](const observer<U> &obs) -> subscription {
          return this->subscribe([mapper, &obs](const T &value) {
            return mapper(value).subscribe(obs);
          });
        });
  }

  auto distinct() {
    return observable<T>([this](const observer<T> &obs) {
      struct distinct_state {
        std::unordered_set<T> seen = {};
        std::mutex mtx;
      };
      // std::unordered_set<T> seen = {};
      auto state = std::make_shared<distinct_state>();

      return this->subscribe(
          {.on_next =
               [obs, state](const T &value) {
                 std::unique_lock<std::mutex> lock(state->mtx);
                 if (state->seen.insert(value).second) {
                   obs.next(value);
                 }
               },
           .on_complete =
               [obs, state] {
                 std::unique_lock<std::mutex> lock(state->mtx);
                 obs.complete();
               }});
    });
  }

  template <typename Compare> auto distinct_until_changed(Compare &&compare) {
    return observable<T>([this, compare = std::forward<Compare>(compare)](
                             const observer<T> &obs) {
      struct distinct_until_changed_state {
        std::optional<T> last;
        std::mutex mtx;
        bool is_terminated = false;
      };

      auto state = std::make_shared<distinct_until_changed_state>();

      return this->subscribe(
          {.on_next =
               [state, compare, obs](const T &value) {
                 std::lock_guard<std::mutex> lock(state->mtx);
                 if (state->is_terminated) {
                   return;
                 }
                 if (!state->last.has_value() ||
                     !compare(state->last.value(), value)) {
                   obs.next(value);
                   state->last = value;
                 }
               },
           .on_complete =
               [state, obs] {
                 std::lock_guard<std::mutex> lock(state->mtx);
                 state->is_terminated = true;
                 obs.complete();
               }});
    });
  }

  auto distinct_until_changed() {
    return distinct_until_changed(std::equal_to<T>());
  }

  auto first() {
    return observable<T>([&](const observer<T> &obs) {
      bool has_taken_first = false;
      subscription sub =
          this->subscribe({.on_next =
                               [&](const T &value) {
                                 if (!has_taken_first) {
                                   has_taken_first = true;

                                   obs.next(value);
                                   obs.complete();
                                   sub.unsubscribe();
                                 }
                               },
                           .on_complete = [&] { obs.complete(); }});

      return sub;
    });
  }

  auto last() {
    return observable<T>([this](const observer<T> &obs) {
      T last;
      struct last_state {
        T value;
        std::mutex mtx;
        bool is_terminated;
      };
      auto state = std::make_shared<last_state>();
      return this->subscribe(
          {.on_next =
               [state](const T &value) {
                 std::lock_guard<std::mutex> lock(state->mtx);
                 if (state->is_teminated) {
                   return;
                 }
                 state.value = value;
               },
           .on_complete =
               [state, obs] {
                 std::lock_guard<std::mutex> lock(state->mtx);
                 if (state->is_terminated) {
                   return;
                 }
                 state->is_terminated = true;
                 obs.next(state->last);
                 obs.complete();
               }});
    });
  }

  auto skip(size_t n) {
    return observable<T>([this, n](const observer<T> &obs) {
      size_t count = 0;
      return this->subscribe({.on_next = [&count, &obs, n](const T &value) {
        if (count++ >= n) {
          return obs.next(value);
        }
      }});
    });
  }

  auto take(size_t count) {
    return observable<T>([=](const observer<T> &obs) -> subscription {
      size_t remain = count;
      subscription sub;
      sub = this->subscribe({.on_next =
                                 [&](const T &value) {
                                   if (remain > 0) {
                                     obs.next(value);
                                     remain--;
                                     if (remain == 0) {
                                       obs.complete();
                                       sub.unsubscribe();
                                     }
                                   }
                                 },
                             .on_complete = [&] { obs.complete(); }});
      return sub;
    });
  }
  template <typename F, typename K = std::invoke_result_t<F, T>>
  auto group_by(F &&key_for);

  template <typename Container,
            typename std::enable_if_t<is_iterable<Container>, bool> = true>
  auto to_iterable() const {
    return observable<Container>([this](const observer<Container> &obs) {
      Container res = {};
      auto o_first = std::back_inserter(res);

      return this->subscribe(
          {.on_next = [&o_first](const T &t) { *o_first++ = t; },
           .on_complete = [&obs, &res] { obs.on_next(res); }});
    });
  }

  template <typename Period> auto delay(const Period &a_while) {

    return observable<T>([&](const observer<T> &obs) {
      std::this_thread::sleep_for(a_while);
      return this->subscribe(
          {.on_next = [&](const T &t) { return obs.next(t); }});
    });
  }

  template <typename F> auto filter(F &&pred) {
    auto upstream = this;

    return observable<T>([upstream, pred = std::forward<F>(pred)](
                             const observer<T> &downstream) {
      return upstream->subscribe(
          {.on_next =
               [pred, downstream](const T &value) {
                 if (pred(value)) {
                   downstream.next(value);
                 }
               },
           .on_complete = [downstream] { downstream.complete(); }});
    });
  }

  template <typename Period> auto debounce(const Period &timeout) {
    using clock_t = std::chrono::steady_clock;

    return observable<T>([this, timeout](const observer<T> &obs) {
      struct debounce_state {
        std::optional<T> last;
        subscription timer;
        std::mutex mtx;
        bool is_terminated;
      };

      auto state = std::make_shared<debounce_state>();

      auto last_time = clock_t::now();

      return this->subscribe(
          {.on_next = [state, timeout, obs, &last_time](const T &value) {
            std::lock_guard<std::mutex> lock(state->mtx);
            if (state->is_terminated) {
              return;
            }
            state->last = value;

            // when a new value comes in, check if the previous value
            // arrived before the `timeout` if it didn't -> emit new
            // value
            auto current_time = clock_t::now();
            if (current_time - last_time < timeout) {
              return obs.next(value);
            }
            last_time = current_time;
          }});
    });
  }

  template <typename Duration> auto window(const Duration &duration) {
    using U = observable<T>; // std::vector<T>;
    using clock_t = std::chrono::steady_clock;

    return observable<U>([this, duration](const observer<U> &obs) {
      std::vector<T> buffer = {};
      auto when = clock_t::now() + duration;
      return this->subscribe(
          {.on_next =
               [&obs, &buffer, &when, duration](const T &val) {
                 buffer.push_back(val);
                 auto now = clock_t::now();
                 if (now >= when) {
                   auto inner = from_iterable(buffer);
                   buffer.clear();
                   when = now + duration;
                   return obs.next(inner);
                 }
               },
           .on_complete =
               [&] {
                 // clear out any remainders
                 if (buffer.size() > 0)
                   obs.next(from_iterable(buffer));
               }});
    });
  }

  template <typename U> auto buffer_with_count(size_t n) {
    return observable<U>([this, n](const observer<U> &obs) {
      U buffer = {};

      return this->subscribe({.on_next =
                                  [&buffer, &obs, n](const T &val) {
                                    append_to(buffer, val);
                                    if (buffer.size() >= n) {
                                      auto ret = obs.next(buffer);
                                      buffer.clear();
                                      return ret;
                                    }
                                  },
                              .on_complete =
                                  [&] {
                                    if (!buffer.empty()) {
                                      obs.next(buffer);
                                      buffer.clear();
                                    }
                                  }});
    });
  }
};

template <typename T, typename K>
struct grouped_observable : public observable<T> {
  using key_t = K;
  key_t _key;

  explicit grouped_observable(const key_t &key) : observable<T>(), _key(key) {}

  const key_t get_key() const { return _key; }
};

template <typename T>
template <typename F, typename K>
auto observable<T>::group_by(F &&fun) {
  using shared_grouped_obaservable = std::shared_ptr<grouped_observable<T, K>>;
  return observable<T>(
      [this, fun = std::forward<F>(fun)](const observer<T> &obs) {
        struct group_by_state {
          std::map<K, observable<T>> downstream;
          std::mutex mtx;
          bool is_terminated = false;
        };
        auto state = std::make_shared<group_by_state>();
        return this->subscribe({.on_next = [state, fun, obs](const T &value) {
          if (state.is_terminated)
            return;
          K key = fun(value);
        }});
      });
}
} // namespace rx