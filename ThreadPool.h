#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>       // 从异步获取结果
#include <functional>   // 包装函数为对象
#include <stdexcept>

class ThreadPool {
public:
    ThreadPool(size_t);             // 构造函数
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)     
        -> std::future<typename std::result_of<F(Args...)>::type>;  // 模板
    /*
    这句话是一个函数模板，形参是F&&类型的f，和Args&&类型的多个args参数；
    函数的返回类型是： std::future<typename std::result_of<F(Args...)>::type>
    这个模板函数enqueue()的返回值类型就是F(Args...)的异步执行结果类型
    这个程序主要执行的任务即  F(Args...) F是函数， afgs是参数
    result_of<F(Args...)>::type 返回的是F(Args...) 的返回值的类型
    */
    
    
    ~ThreadPool();                  // 析构函数
private:
    // need to keep track of threads so we can join them
    std::vector< std::thread > workers;             // 线程数组
    // the task queue
    std::queue< std::function<void()> > tasks;      // 任务队列
    
    // synchronization
    std::mutex queue_mutex;                         // 互斥锁
    std::condition_variable condition;              // 条件变量
    bool stop;                                      // 停止或者开始任务
};
 
// the constructor just launches some amount of workers
// inline: 类似宏定义，会建议编译器把函数以直接展开的形式放入目标代码而不是以入栈调用的形式。
// 通常函数体内代码比较长或者体内出现循环时不宜使用内联，这样会造成代码膨胀。
inline ThreadPool::ThreadPool(size_t threads)   // 构造threads个线程的线程池
    :   stop(false)                             // 初始化stop为false,说明开始线程池启动了
{
    for(size_t i = 0;i<threads;++i)
        // workers线程数组中添加的是一个lambda表达式，主要是线程同步的内容，可以使用this指针来表示class内的变量
        workers.emplace_back(
            [this]
            {
                for(;;)
                {   
                    // 定义task是void类型的函数对象
                    // 整体流程如下：1.先定义task函数对象；2.上锁；3.条件变量等待阻塞；4.判断当前线程池运行情况和任务队列的情况；5.获取当前的任务；6.执行task任务。
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        // 使用条件变量，第二个参数为false时解锁并阻塞
                        // 阻塞条件为：线程池正在运行且任务队列为空
                        this->condition.wait(lock,
                            [this]{ return this->stop || !this->tasks.empty(); });
                        // 如果线程池停止运行并且任务队列为空，则退出循环
                        if(this->stop && this->tasks.empty())
                            return;
                        // move类似拷贝构造，将tasks.front赋给task，然后将它pop出列
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }

                    task();
                }
            }
        );
}

// add new work item to the pool
// 这个函数主要是向线程池内添加异步执行的任务
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type>
{
    // 给返回值类型赋予一个简单的名字 reutrn_type
    using return_type = typename std::result_of<F(Args...)>::type;
    /*
        将f函数的类型和args参数们的类型打包成模板内的task，方便后续的使用
        packaged_task定义于future头文件，用于包装可调用目标，将普通函数对象转为异步执行   packaged_task<F(Args...)>  函数
        bind函数是绑定， 绑定函数f和参数args 
        forward函数用于完美转发，保证传入下一个函数的值与原值相同，原来是左值还是左值，原来是右值还是右值
        make_shared类似于new, 但返回的是一个指向开辟的内存的shared_ptr指针，task即为shared_ptr
    */ 
    
    auto task = std::make_shared< std::packaged_task<return_type()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
    // 获得函数异步执行的结果，返回类型是return_type，即那一大串
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);

        // don't allow enqueueing after stopping the pool
        if(stop)
            throw std::runtime_error("enqueue on stopped ThreadPool");
        // 任务队列插入任务，task指针的值，即task函数
        tasks.emplace([task](){ (*task)(); });
    }
    // 解除一个正在等待唤醒的线程的阻塞态
    condition.notify_one();
    // 返回异步结果res
    return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool()
{
    {
        // 加锁，然后设置停止状态
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    // 唤醒所有阻塞的线程
    condition.notify_all();
    // 循环join所有的线程
    for(std::thread &worker: workers)
        worker.join();
}

#endif
