#include "rx/rx.hpp"

int main(int, char**)
{

    rx::behavior_subject<int> bs(1338);
    bs.subscribe({
        .on_next =
            [](int n) {
                std::cout << n << std::endl;
            },
    });
    bs.next(1337);
    bs.complete();

    rx::replay_subject<int> rs(15);
    rs.subscribe({
        .on_next =
            [](int n) {
                std::cout << "sub1, n=" << n << std::endl;
            },
    });
    rs.next(1);
    rs.next(2);
    rs.next(3);
    rs.complete();

    rs.subscribe({
        .on_next =
            [](int n) {
                std::cout << "sub2, n=" << n << std::endl;
            },
    });

    rx::subject<int> on_foo;
    on_foo.subscribe({
        .on_next =
            [](int n) {
                std::cout << n;
            },
        .on_complete =
            [] {
                std::cout << std::endl;
            },
    });
    on_foo.next(10);
    on_foo.complete();

    auto fetchData = rx::make_observable<std::string>([](const rx::observer<std::string>& obs) {
        std::cout << "Making network request..." << std::endl; // Side effect
        // Simulate async network call
        std::thread([obs]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            obs.next("Data from server");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            obs.next("Data from server");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            obs.next("Data from server");
            obs.complete();
        }).detach();
        return rx::subscription([] { /* cleanup */ });
    });

    fetchData
        .map([](const std::string& str) -> std::string {
            static int i = 0;
            return str + " " + std::to_string(i++);
        })
        .subscribe({
            .on_next =
                [](const std::string& str) {
                    std::cout << str << std::endl;
                },
        });

    auto source = rx::of(1, 2, 3, 3, 4, 5, 6, 6)
                      .distinct()
                      .map([](int x) {
                          return x * x;
                      })
                      .reduce([](int seed, int x) {
                          return seed + x;
                      })
                      .subscribe({
                          .on_next =
                              [](int value) {
                                  std::cout << value << ";";
                              },
                          .on_complete =
                              [] {
                                  std::cout << std::endl;
                              },
                      });

    rx::of(1, 1, 2, 2, 2, 3, 4, 5, 5, 6)
        .distinct_until_changed()
        .subscribe({
            .on_next =
                [](int value) {
                    std::cout << "changed: " << value << std::endl;
                },
        });

    rx::range(0, 10)
        .skip(3)
        .take(5)
        .filter([](int n) {
            return n & 1;
        })
        .subscribe({
            .on_next =
                [](int i) {
                    std::cout << "odd=" << i << ",";
                },
            .on_complete =
                [] {
                    std::cout << std::endl;
                },
        });
    std::this_thread::sleep_for(std::chrono::seconds(4));
    return 0;
}
