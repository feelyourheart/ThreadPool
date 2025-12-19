#include <iostream>
#include <vector>
#include <chrono>
#include "thrdPool.h"

//int main() {
//	std::function<int(int, int)> f = [](int a, int b) -> int {
//		return a + b;
//	};
//	ThreadPool tp(10);
//	std::future<int> result = tp.enqueue(f, 1, 1);
//
//	std::cout << result.get() << std::endl;
//	return 0;
//}

int main()
{
    ThreadPool pool(4);
    std::vector< std::future<int> > results;

    for (int i = 0; i < 8; ++i) {
        results.emplace_back(
            pool.enqueue([i] {
                std::cout << "hello " << i << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                std::cout << "world " << i << std::endl;
                return i * i;
                })
        );
    }

    for (auto&& result : results)
        std::cout << result.get() << ' ';
    std::cout << std::endl;

    return 0;
}