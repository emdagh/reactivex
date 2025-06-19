 #include <cassert>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_set>

#define DEBUG_METHOD() std::cout << __PRETTY_FUNCTION__ << "(" << __FILE__ << ":" << __LINE__ << ")" << std::endl

using namespace std::literals;

template <typename T> std::ostream& operator<<(std::ostream& os, const std::vector<T>& v)
{
    std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, ","));
    return os;
}

namespace rx
{

template <typename Iterable> auto from_iterable(Iterable iterable);

enum class retval : uint8_t
{
    kNext,
    kComplete,
    kError
};

std::ostream& operator<<(std::ostream& os, retval rv)
{
    os << static_cast<int>(rv);
    return os;
}

template <class C, class V> auto append(C& container, V&& value, int) -> decltype(container.push_back(std::forward<V>(value)), void())
{
    container.push_back(std::forward<V>(value));
}

template <class C, class V> void append(C& container, V&& value, ...)
{
    container.insert(std::forward<V>(value));
}

template <class C, class V> void append_to(C& container, V&& value)
{
    append(container, std::forward<V>(value), 0);
}

template <typename, typename = void> constexpr bool is_iterable{};

template <typename T> constexpr bool is_iterable<T, std::void_t<decltype(std::declval<T>().begin()), decltype(std::declval<T>().end())>> = true;

struct subscription
{
    using unsubscribe_fun = std::function<void(void)>;
    unsubscribe_fun _fun;
    subscription()
      : _fun(nullptr)
    {
    }
    subscription(unsubscribe_fun&& fun)
      : _fun(std::forward<unsubscribe_fun>(fun))
    {
    }
    virtual ~subscription()
    {
        unsubscribe();
    }
    void unsubscribe()
    {
        if(_fun != nullptr)
        {
            _fun();
        }
    }
};


template <typename T> struct observer
{
    using fun_next     = std::function<void(const T&)>;
    using fun_complete = std::function<void(void)>;
    using fun_error    = std::function<void(std::exception_ptr)>;

    fun_next on_next         = nullptr;
    fun_complete on_complete = nullptr;
    fun_error on_error       = nullptr;

    mutable bool is_complete = false;

    void next(const T& value) const
    {
        if(on_next != nullptr && !is_complete)
        {
             on_next(value);
        }
    }

    void complete() const
    {
        if(on_complete != nullptr && !is_complete)
        {
            on_complete();
        }
        is_complete = true;
    }
};

template <typename T> class observable
{
    using subscribe_fun = std::function<subscription(const observer<T>&)>;
    subscribe_fun _fun;

public:
    observable(subscribe_fun fun)
      : _fun(std::move(fun))
    {
    }
    virtual ~observable()
    {
    }

    virtual subscription subscribe(const observer<T>& obs)
    {
        auto ret = _fun(obs);
        obs.complete();
        return ret;
    }

    template <typename F> auto map(F&& fun)
    {
        DEBUG_METHOD();
        return observable<T>([&](const observer<T>& obs) {
            return this->subscribe({.on_next = [&](const T& t) {
                return obs.next(fun(t));
            }});
        });
    }
    template <typename F> auto reduce(F&& fun, T seed = T{0})
    {
        return observable<T>([this, fun, seed](const observer<T>& obs) {
            T result = seed;
            return this->subscribe({.on_next =
                                        [&](const T& value) {
                                            result = fun(result, value);
                                            //return retval::kNext;
                                        },
                                    .on_complete =
                                        [&] {
                                            obs.next(result);
                                        }});
        });
    }

    template <typename U, typename Fun> auto flat_map(Fun&& mapper)
    {
        return observable<U>([this, mapper](const observer<U>& obs) -> subscription {
            return this->subscribe([mapper, &obs](const T& value) {
                return mapper(value).subscribe(obs);
            });
        });
    }

    auto distinct()
    {
        return observable<T>([this](const observer<T>& obs) {
            std::unordered_set<T> seen = {};
            return this->subscribe({.on_next = [&](const T& value) {
                if(seen.insert(value).second)
                {
                    obs.next(value);
                }
                //return retval::kNext;
            }});
        });
    }

    auto first()
    {
        return observable<T>([&] (const observer<T>& obs) {
            subscription sub = this->subscribe({
                .on_next = [&] (const T& value) {
                    obs.next(value);
                    obs.complete();
                    sub.unsubscribe();
                },
            });
            return sub;
        });
    }

    auto last()
    {
        return observable<T>([this](const observer<T>& obs) {
            T last;
            subscription sub;
            this->subscribe({.on_next =
                                        [&](const T& value) {
                                            last = value;
                                        },
                                    .on_complete =
                                        [&] {
                                            obs.next(last);                                            
                                        }});
            return sub;
        });
    }

    auto skip(size_t n)
    {
        return observable<T>([this, n](const observer<T>& obs) {
            size_t count = 0;
            return this->subscribe({.on_next = [&count, &obs, n](const T& value) {
                if(count++ >= n)
                {
                    return obs.next(value);
                }
                //return retval::kNext;
            }});
        });
    }

    auto take(size_t count)
    {
        DEBUG_METHOD();
        return observable<T>([=](const observer<T>& obs) -> subscription {
            size_t remain = count;
            subscription sub;
            sub = this->subscribe({
            .on_next = [&](const T& value) {
                obs.next(value);
                if(--remain <= 0) {
                    obs.complete();
                    sub.unsubscribe();
                }
                /*if(remain > 0) {
                    obs.next(value);
                
                    remain--;
                    if(remain == 0) {
                       obs.complete();
                       sub.unsubscribe();
                    }
                }*/
            },
                .on_complete = [&] {
                    obs.complete();   
                }});
            return sub;
        });
    }
    template <typename KeySelector> auto group_by(KeySelector key_for)
    {
        using U = std::vector<T>;
        using Y = observable<T>;
        return observable<Y>([this, key_for](const observer<Y>& obs) -> subscription {
            std::unordered_map<T, U> buffer = {};

            return this->subscribe({.on_next =
                                        [&obs, &buffer, key_for](const T& value) {
                                            buffer[key_for(value)].push_back(value);
                                            return retval::kNext;
                                        },
                                    .on_complete =
                                        [&obs, &buffer] {
                                            for(auto group : buffer)
                                            {
                                                obs.next(from_iterable(group.second));
                                            }
                                        }});
        });
    }

    template <typename Container, typename std::enable_if_t<is_iterable<Container>, bool> = true> auto to_iterable() const
    {
        return observable<Container>([this](const observer<Container>& obs) {
            Container res = {};
            auto o_first  = std::back_inserter(res);

            return this->subscribe({.on_next =
                                        [&o_first](const T& t) {
                                            *o_first++ = t;
                                            return retval::kNext;
                                        },
                                    .on_complete =
                                        [&obs, &res] {
                                            obs.on_next(res);
                                        }});
        });
    }

    template <typename Period> auto delay(const Period& a_while)
    {

        return observable<T>([&](const observer<T>& obs) {
            std::this_thread::sleep_for(a_while);
            return this->subscribe({.on_next = [&](const T& t) {
                return obs.next(t);
            }});
        });
    }

    template <typename Period> auto debounce(const Period& timeout)
    {
        using clock_t = std::chrono::steady_clock;
        return observable<T>([this, timeout](const observer<T>& obs) {
            auto last_time = clock_t::now();
            return this->subscribe({.on_next = [&](const T& value) {
                // when a new value comes in, check if the previous value
                // arrived before the `timeout` if it didn't -> emit new
                // value
                auto current_time = clock_t::now();
                if(current_time - last_time < timeout)
                {
                    return obs.next(value);
                }
                last_time = current_time;
                return retval::kNext;
            }});
        });
    }

    template <typename Duration> auto window(const Duration& duration)
    {
        using U       = observable<T>; // std::vector<T>;
        using clock_t = std::chrono::steady_clock;

        return observable<U>([this, duration](const observer<U>& obs) {
            std::vector<T> buffer = {};
            auto when             = clock_t::now() + duration;
            return this->subscribe({.on_next =
                                        [&obs, &buffer, &when, duration](const T& val) {
                                            buffer.push_back(val);
                                            auto now = clock_t::now();
                                            if(now >= when)
                                            {
                                                auto inner = from_iterable(buffer);
                                                buffer.clear();
                                                when = now + duration;
                                                return obs.next(inner);
                                            }
                                            return retval::kNext;
                                        },
                                    .on_complete =
                                        [&] {
                                            // clear out any remainders
                                            if(buffer.size() > 0)
                                                obs.next(from_iterable(buffer));
                                        }});
        });
    }

    template <typename U> auto buffer_with_count(size_t n)
    {
        return observable<U>([this, n](const observer<U>& obs) {
            U buffer = {};

            return this->subscribe({.on_next =
                                        [&buffer, &obs, n](const T& val) {
                                            append_to(buffer, val);
                                            if(buffer.size() >= n)
                                            {
                                                auto ret = obs.next(buffer);
                                                buffer.clear();
                                                return ret;
                                            }
                                            return retval::kNext;
                                        },
                                    .on_complete =
                                        [&] {
                                            if(!buffer.empty())
                                            {
                                                obs.next(buffer);
                                                buffer.clear();
                                            }
                                        }});
        });
    }
};

template <typename T, typename Traits = std::char_traits<T>> static auto from_istream(std::basic_istream<T, Traits>& iss)
{
    using char_type = typename std::basic_istream<T, Traits>::char_type;
    return observable<char_type>([&iss](const observer<char_type>& obs) {
        while(!iss.eof() && !iss.bad())
        {
            T value;
            iss >> value;
            if(iss.fail())
            {
                obs.error();
                break;
            }
            obs.next(value);
        }
        obs.complete();
    });
}

template <typename... Ts> auto of(Ts&&... ts)
{
    using T = typename std::common_type<Ts...>::type;
    return observable<T>([ts...](const observer<T>& obs) {
        std::initializer_list<T> list{(ts)...};
        for(auto i : list)
        {
            obs.next(i);
            //if(obs.next(i) != retval::kNext)
            //{
            //    break;
            //}
        }
        return subscription {};
    });
}

template <typename T> auto range(T start, T count)
{
    return observable<T>([start, count](const observer<T>& obs) {
        for(T i = start; i < start + count; ++i)
        {
            retval ret = obs.next(i);
            if(ret != retval::kNext)
            {
                break;
            }
        }
        return subscription{};
    });
}

template <typename Iterable> auto from_iterable(Iterable iterable)
{
    using T = typename std::remove_reference<decltype(*iterable.begin())>::type;
    return observable<T>([iterable](const observer<T>& obs) {
        for(auto i : iterable)
        {
            obs.next(i);
            //if(obs.next(i) != retval::kNext)
            //{
            //    break;
            //}
        }
        return subscription{};
    });
}

// template <typename T>
// struct subscriber : public subscription, public observer<T> {};

template <typename T>
class subject
  : public observable<T>
  , public observer<T>
{
    std::vector<observer<T>> _sub;

    subscription int_subscribe(const observer<T>& obs)
    {
        _sub.push_back(obs);
        std::cout << "[subscribe] obs=" << &obs << std::endl;
        size_t ix = _sub.size() - 1;

        return subscription([this, &obs, ix] {
            auto it = _sub.begin() + ix; // std::find(_sub.begin(), _sub.end(), obs);
            if(it != _sub.end())
            {
                std::cout << "[unsubscribe] obs=" << &obs << std::endl;
                _sub.erase(it);
            }
        });
    }

public:
    subject(std::function<void(const observer<T>&)>&& pre = nullptr)
      : observable<T>([this, pre = std::forward<decltype(pre)>(pre)](const observer<T>& obs) -> subscription {
          if(pre != nullptr)
          {
              pre(obs);
          }
          return int_subscribe(obs);
      })
    {
    }

    virtual void next(const T& value)
    {
        DEBUG_METHOD();
        for(auto& i : _sub)
        {
            i.next(value);
        }
    }
};

template <typename T> class behavior_subject : public subject<T>
{
    T _current;
    // std::vector<rx::observer<T>> _lst;

public:
    explicit behavior_subject(const T& t)
      : subject<T>([this, t](const observer<T>& obs) {
          obs.next(t);
      })
      , _current(t)
    {
    }
    virtual ~behavior_subject()
    {
    }
    virtual void on_next(const T& t)
    {
        _current = t;
        subject<T>::next(t);
    }
};

template <typename T> class replay_subject : public subject<T>
{
    size_t _len;
    std::deque<T> _q;

public:
    replay_subject(size_t buf_len)
      : subject<T>([this](const observer<T>& obs) {
          for(auto& item : _q)
          {
              obs.next(item);
          }
      })
      , _len(buf_len)
    {
    }

    virtual ~replay_subject()
    {
    }

    virtual void next(const T& t)
    {
        DEBUG_METHOD();
        _q.push_back(t);
        if(_q.size() > _len)
        {
            _q.pop_front();
        }
        subject<T>::next(t);
    }
};
} // namespace rx

int main(int, char**) {
    auto source = rx::of(1,2,3,4,5).subscribe({
        .on_next = [] (int value) { std::cout << value << ","; return rx::retval::kNext;},
        .on_complete = [] { std::cout << std::endl; } 
    });
    return 0;
    
}
