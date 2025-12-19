// 头文件保护宏：防止头文件被重复包含，避免重定义错误（跨平台性优于 #pragma once）
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// 核心头文件说明：
#include <vector>      // 存储工作线程（std::thread）的容器
#include <queue>       // 任务队列，存储待执行的任务
#include <memory>      // 智能指针（std::shared_ptr），管理动态任务对象
#include <thread>      // std::thread 类，创建/管理操作系统线程
#include <mutex>       // 互斥锁，保护共享资源（任务队列/停止标志）的线程安全访问
#include <condition_variable>  // 条件变量，实现线程间同步（等待/唤醒）
#include <future>      // std::future/std::packaged_task，处理异步任务的返回值
#include <functional>  // std::function 函数包装器，统一存储任意可调用对象
#include <stdexcept>   // 标准异常类，用于抛出运行时错误

// 线程池类定义
class ThreadPool {
public:
    // 构造函数：参数为线程池的工作线程数量
    ThreadPool(size_t);

    // 模板成员函数：提交任务到线程池
    // F：可调用对象类型（函数、lambda、函数对象等）
    // Args：参数包，对应可调用对象的参数类型
    // 返回值：std::future，用于异步获取任务执行结果
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;

    // 析构函数：负责停止线程池并等待所有工作线程退出
    ~ThreadPool();

private:
    // 工作线程容器：存储线程池中的所有工作线程
    std::vector<std::thread> workers;

    // 任务队列：存储待执行的任务，std::function<void()> 统一包装无参可调用对象
    std::queue<std::function<void()>> tasks;

    // 同步相关成员：
    std::mutex queue_mutex;          // 保护任务队列/stop标志的互斥锁
    std::condition_variable condition;  // 条件变量，用于唤醒等待的工作线程
    bool stop;                       // 线程池停止标志（true表示需要终止所有线程）
};

// 构造函数实现（inline：建议编译器内联，提升头文件函数的性能）
inline ThreadPool::ThreadPool(size_t threads)
    : stop(false)  // 初始化列表：停止标志默认false（线程池运行中）
{
    // 循环创建指定数量的工作线程
    for (size_t i = 0; i < threads; ++i) {
        // emplace_back：直接在vector中构造线程对象（std::thread不可拷贝，只能移动/直接构造）
        workers.emplace_back(
            // 工作线程的核心逻辑：lambda函数（捕获this指针，访问类成员）
            [this]() {
                // 无限循环：工作线程持续等待/执行任务，直到收到停止信号
                for (;;) {  // 等价于 while(true)
                    // 定义任务对象：存储从队列中取出的待执行任务
                    std::function<void()> task;

                    {  // 作用域：限制锁的生命周期，执行任务时解锁，提升并发
                        // 加锁：std::unique_lock 支持条件变量的wait操作（可解锁/重加锁）
                        std::unique_lock<std::mutex> lock(this->queue_mutex);

                        // 等待条件变量：
                        // 1. 原子性解锁并阻塞线程，直到满足条件（stop=true 或 任务队列非空）
                        // 2. 条件满足时，重新加锁并返回
                        this->condition.wait(lock,
                            [this]() { return this->stop || !this->tasks.empty(); });

                        // 退出条件：线程池停止 且 任务队列为空 → 工作线程可以退出
                        if (this->stop && this->tasks.empty()) {
                            return;  // 退出lambda，工作线程结束
                        }

                        // 取出任务：std::move 转移所有权（避免std::function的拷贝开销）
                        task = std::move(this->tasks.front());
                        this->tasks.pop();  // 从队列移除已取出的任务
                    }  // 解锁：unique_lock超出作用域，自动释放互斥锁

                    // 执行任务：此时已解锁，多个线程可并行执行任务
                    task();
                }
            }
        );
    }
}

// 模板函数：提交任务到线程池
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
-> std::future<typename std::result_of<F(Args...)>::type>
{
    // 推导任务的返回类型：std::result_of<F(Args...)>::type → C++17可替换为std::invoke_result_t<F, Args...>
    using return_type = typename std::result_of<F(Args...)>::type;

    // 包装任务：
    // 1. std::bind：将可调用对象f和参数args绑定为无参函数（完美转发保持参数属性）
    // 2. std::packaged_task：包装无参任务，关联std::future（用于获取结果）
    // 3. std::make_shared：packaged_task不可拷贝，只能移动；而任务队列需要拷贝std::function，
    //    因此用shared_ptr包装，实现间接拷贝（拷贝指针而非任务本身）
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // 获取future对象：通过packaged_task关联，调用者可通过该对象等待/获取任务结果
    std::future<return_type> res = task->get_future();

    {  // 作用域：限制锁的生命周期
        // 加锁：保护任务队列的写入操作
        std::unique_lock<std::mutex> lock(queue_mutex);

        // 检查：线程池已停止时，禁止提交任务，抛出运行时异常
        if (stop) {
            throw std::runtime_error("enqueue on stopped ThreadPool");
        }

        // 将任务加入队列：lambda捕获shared_ptr<task>，执行时调用(*task)()
        tasks.emplace([task]() { (*task)(); });
    }  // 解锁：unique_lock超出作用域，自动释放锁

    // 通知一个等待的工作线程：有新任务加入，可执行
    condition.notify_one();

    // 返回future对象，供调用者使用
    return res;
}

// 析构函数：安全停止线程池
inline ThreadPool::~ThreadPool()
{
    {  // 作用域：限制锁的生命周期
        // 加锁：修改stop标志需要线程安全
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;  // 设置停止标志为true，通知所有工作线程退出
    }  // 解锁

    // 唤醒所有等待的工作线程：让它们检查stop标志并退出
    condition.notify_all();

    // 等待所有工作线程退出：join() 阻塞当前线程，直到工作线程执行完毕（避免僵尸线程）
    for (std::thread& worker : workers) {
        worker.join();
    }
}

#endif  // 结束头文件保护宏