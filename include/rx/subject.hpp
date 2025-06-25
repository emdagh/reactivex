#pragma once

namespace rx {
template <typename T> class subject : public observable<T>, public observer<T> {
  // std::unordered_map<observer<T>, bool> subscribers;
  struct observer_context {
    observer<T> obs;
    bool is_active = true;

    explicit observer_context(observer<T> obs) : obs(std::move(obs)) {}
  };
  std::vector<observer_context> subscribers;

  std::mutex mtx;
  bool is_terminated = false;

  void cleanup_inactive_subscribers() {
    std::lock_guard<std::mutex> lock(mtx);
    subscribers.erase(
        std::remove_if(subscribers.begin(), subscribers.end(),
                       [](const auto &context) { return !context.is_active; }),
        subscribers.end());
  }

protected:
  virtual subscription subscribe_impl(const observer<T> &obs) {
    if (is_terminated) {
      obs.complete();
      return subscription{};
    }
    auto context = observer_context(obs);
    {
      std::lock_guard<std::mutex> lock(mtx);
      subscribers.push_back(context);
    }
    return subscription(
        [this, context]() mutable { context.is_active = false; });
  }
  using pre_subscribe_hook = std::function<void(const observer<T> &)>;

public:
  explicit subject(pre_subscribe_hook &&hook = nullptr)
      : observable<T>([this, hook = std::forward<decltype(hook)>(hook)](
                          const observer<T> &obs) -> subscription {
          if (hook) {
            hook(obs);
          }
          return subscribe_impl(obs);
        }) {}

  virtual void next(const T &value) override {
    if (is_terminated) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mtx);
      for (const auto &context : subscribers) {
        if (context.is_active) {
          context.obs.next(value);
        }
      }
    }
    cleanup_inactive_subscribers();
  }

  void complete() {
    if (is_terminated) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mtx);
      for (auto &context : subscribers) {
        if (context.is_active) {
          context.obs.complete();
          context.is_active = false;
        }
      }
    }
    subscribers.clear();
  }
};
} // namespace rx