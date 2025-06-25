#pragma once

struct scheduler {
  virtual ~scheduler() = default;
  virtual subscription schedule(std::function<void()> &&) = 0;
  virtual subscription schedule(std::function<void()> &&, int) = 0;
};

class threadpool_scheduler : public scheduler {
  struct task {
    int execution_time;
    std::function<void()> action;
    bool is_cancelled;

    bool operator>(const task &other) {
      return execution_time > other.execution_time;
    }
  };

public:
};