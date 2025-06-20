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
#include <mutex>

#define DEBUG_METHOD() std::cout << __PRETTY_FUNCTION__ << "(" << __FILE__ << ":" << __LINE__ << ")" << std::endl
#define DEBUG_INFO(...) std::cout << __VA_ARGS__ << std::endl
using namespace std::literals;

template <typename T> std::ostream& operator<<(std::ostream& os, const std::vector<T>& v)
{
    std::copy(v.begin(), v.end(), std::ostream_iterator<T>(os, ","));
    return os;
}

namespace rx
{

template <typename Iterable> auto from_iterable(Iterable iterable);

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

class subscription
{
    using unsubscribe_fun = std::function<void(void)>;
    unsubscribe_fun _fun;
public:
    subscription()
      : _fun(nullptr)
    {
    }
    
    subscription(unsubscribe_fun&& fun)
      : _fun(std::move(fun))
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

template <typename T>
struct observer_impl {
    
};


template <typename T> struct observer
{
    using fun_next     = std::function<void(const T&)>;
    using fun_complete = std::function<void(void)>;
    using fun_error    = std::function<void(std::exception_ptr)>;

    fun_next on_next         = nullptr;
    fun_complete on_complete = nullptr;
    fun_error on_error       = nullptr;
    
public:
    

    void next(const T& value) const
    {
        if(on_next != nullptr)
        {
             on_next(value);
        }
    }

    void complete() const
    {
        if(on_complete != nullptr)
        {
            on_complete();
        }
    }

    void error(std::exception_ptr ep) const {}
};

template <typename T> class observable
{
    using subscribe_fun = std::function<subscription(const observer<T>&)>;
    subscribe_fun _fun;

public:
    observable(subscribe_fun&& fun)
      : _fun(std::move(fun))
    {
    }
    virtual ~observable()
    {
    }

    virtual subscription subscribe(const observer<T>& obs)
    {
        //observer<T> obs(std::move(impl));
        auto ret = _fun(obs);
        obs.complete();
        return ret;
    }

    template <typename F, typename U = std::invoke_result_t<F,T>> 
    auto map(F&& fun) {
    
        return observable<U>([&](const observer<U>& obs) {
            return this->subscribe({
                .on_next = [&](const T& t) {
                    return obs.next(fun(t));
                }
            });
        });
    }
    template <typename F, typename U = std::invoke_result_t<F, T, T>> 
    auto reduce(F&& fun, T seed = T{0})
    {
        return observable<U>([this, fun = std::forward<F>(fun), seed](const observer<U>& obs) -> subscription{
            
            struct reduce_state {
                U current;
                std::mutex mx;

            reduce_state(U seed) : current(seed) {}
            };
            
            auto state = std::make_shared<reduce_state> (seed); // make async compatible (no real way around heap here...)
            
            return this->subscribe({
                .on_next = [&, state](const T& value) {
                    std::lock_guard<std::mutex> lock(state->mx);
                    state->current = fun(state->current, value);
                },
                .on_complete = [&, state] {
                    std::lock_guard<std::mutex> lock(state->mx);
                    obs.next(state->current);
                    obs.complete();
                },
                .on_error = [&] (const std::exception_ptr ep) {
                    obs.error(ep);
                }
            });
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
            return this->subscribe({
                .on_next = [&](const T& value) {
                    if(seen.insert(value).second)
                    {
                        obs.next(value);
                    }
                }
            });
        });
    }

    auto first()
    {
        return observable<T>([&] (const observer<T>& obs) {
           bool has_taken_first = false;
            subscription sub = this->subscribe({
                .on_next = [&] (const T& value) {
                    
                    if(!has_taken_first) {
                        has_taken_first = true;
                        
                        obs.next(value);
                        obs.complete();
                        sub.unsubscribe();
                    }
                },
                .on_complete = [&] {
                    obs.complete();  
                }
            });

            
            return sub;
        });
    }

    auto last()
    {
        return observable<T>([this](const observer<T>& obs) {
            T last;
            return this->subscribe({.on_next =
                                        [&](const T& value) {
                                            last = value;
                                        },
                                    .on_complete =
                                        [&] {
                                            obs.next(last);
                                        }});
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
               
                if(remain > 0) {
                    obs.next(value);
               
                    remain--;
                    if(remain == 0) {
                       obs.complete();
                       sub.unsubscribe();
                    }
                }
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

    template <typename Container, typename std::enable_if_t<is_iterable<Container>, bool> = true> 
    auto to_iterable() const
    {
        return observable<Container>([this](const observer<Container>& obs) {
            Container res = {};
            auto o_first  = std::back_inserter(res);

            return this->subscribe({.on_next =
                                        [&o_first](const T& t) {
                                            *o_first++ = t;
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
            obs.next(i);
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
        }
        return subscription{};
    });
}

// template <typename T>
// struct subscriber : public subscription, public observer<T> {};

template <typename T>
class subject
: public observable<T>
, public observer<T> {
    //std::unordered_map<observer<T>, bool> subscribers;
    struct observer_context {
        observer<T> obs;
        bool is_active;

        explicit observer_context(observer<T> obs) : obs(std::move(obs)) {}
    };
    std::vector<observer_context> subscribers;
    
    std::mutex mtx;
    bool is_terminated = false;

    void cleanup_inactive_subscribers() {
        std::lock_guard<std::mutex> lock(mtx);
        subscribers.erase(
            std::remove_if(subscribers.begin(), subscribers.end(), 
            [] (const auto& context) {
                return !context.is_active;
            }), subscribers.end()
        );
    }

    subscription subscribe_impl(const observer<T>& obs) {
        auto context = observer_context(obs);
        {
            std::lock_guard<std::mutex> lock(mtx);
            subscribers.push_back(context);
        }
        return subscription([this, context] () mutable {
            context.is_active = false; 
        });
    }
    using pre_subscribe_hook = std::function<void(const observer<T>&)>;
public:
    explicit subject(pre_subscribe_hook&& hook = nullptr) 
    : observable<T>([this, hook=std::forward<decltype(hook)>(hook)] (const observer<T>& obs) -> subscription {
        if(hook) {
            hook(obs);
        }
        return subscribe_impl(obs);
    })
    {
    
    }

    void next(const T& value) {
        if(is_terminated) {
            return;   
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            for(const auto& context : subscribers) {
                if(context.is_active) {
                    context.obs.next(value);
                }
            }
        }
        cleanup_inactive_subscribers();
    }

    void complete() {
        if(is_terminated) {
            return;   
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            for(auto& context : subscribers) {
                if(context.is_active) {
                    context.obs.complete();
                    context.is_active = false;
                }
            }
        }
        subscribers.clear();
    }
};
    
template <typename T> 
class behavior_subject : public subject<T>
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
    template <typename T>
    static observable<T> make_observable(std::function<subscription(const observer<T>&)> on_subscribe_func) {
        return observable<T>(std::move(on_subscribe_func));
    }
} // namespace rx

int main(int, char**) {
    rx::subject<int> on_foo;
    on_foo.subscribe( {
        .on_next = [] (int n) { std::cout << n; },
        .on_complete = [] { std::cout << std::endl; }
    });
    on_foo.next(10);
    on_foo.complete();
    
    auto source = rx::of(1,2,3,3,4,5,6,6)
        .distinct()
        .map([] (int x) { return x * x; })
        .reduce([] (int seed, int x) { return seed + x; })
        .subscribe({
            .on_next = [] (int value) { std::cout << value << ","; },
            .on_complete = [] { std::cout << std::endl; }
        });

    rx::range(0,10)
        .skip(3)
        .take(5)
        .subscribe({
        .on_next = [] (int i) {
            std::cout << i << ",";
        },
        .on_complete = [] { std::cout << std::endl; }
    });
    return 0;
   
}
