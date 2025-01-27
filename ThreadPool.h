#ifndef _THREAD_POOL_
#define _THREAD_POOL_

#include <chrono>
#include <climits>
#include <cstddef>
#include <thread>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <queue>
#include <atomic>
#include <future>

#include <iostream>
#include <utility>

const int THREAD_POOL_MAX_TASK_NUM = INT_MAX;
const int THREAD_POOL_MAX_THREAD_NUM = 4;
const int THREAD_MAX_IDLE_TIME = 60; // seconds

class Thread
{
    using ThreadFunc = std::function<void(int)>;

public:
    // Thread() = default;
    Thread(ThreadFunc func)
    {
        m_func_ = func;
        m_thread_id_ = generated_id_++;
    }
    ~Thread() = default;

    void start()
    {
        // 启动线程，然后分离主线程
        std::thread t(m_func_, m_thread_id_);
        t.detach();
    }

    uint get_thread_id() const
    {
        return m_thread_id_;
    }

private:
    static uint generated_id_;
    uint m_thread_id_; // unique id
    ThreadFunc m_func_;
};
uint Thread::generated_id_ = 0;

enum class ThreadPoolType
{
    FIEXED,
    CACHED
};

// ThreadPool class
class ThreadPool
{

public:
    ThreadPool(int init_thread_num = 4)
    {
        m_init_thread_num_ = init_thread_num;
        m_max_thread_num_ = THREAD_POOL_MAX_THREAD_NUM;
        m_thread_pool_type_ = ThreadPoolType::FIEXED;
        m_cur_thread_num_ = 0;
        m_idle_thread_num_ = 0;
        m_is_running_ = false;
        m_task_size_ = THREAD_POOL_MAX_TASK_NUM;
    }
    ~ThreadPool()
    {
        std::cout << "ThreadPool begin to destory." << std::endl;
        m_is_running_ = false;
        // 析构时，将目前正在执行/阻塞的所有线程执行完之后，才可以释放
        std::unique_lock<std::mutex> lock(m_task_mutex_);

        m_task_not_empty_.notify_all();                                            // 唤醒所有阻塞的线程
        m_pool_exit_.wait(lock, [&]() -> bool { return m_cur_thread_num_ == 0; }); // 等待所有的线程都释放
    }

    // ThreadPool operation

    void setPoolType(ThreadPoolType type)
    {
        if (m_is_running_)
        {
            return;
        }
        m_thread_pool_type_ = type;
    }

    ThreadPoolType getPoolType() const
    {
        return m_thread_pool_type_;
    }

    void setTaskQueMaxNum(uint max_task_num)
    {
        if (m_is_running_)
        {
            return;
        }
        m_max_task_que_num_ = max_task_num;
    }

    void setThreadPoolMaxThreadNum(uint max_thread_num)
    {
        if (m_is_running_)
        {
            return;
        }
        if (m_thread_pool_type_ == ThreadPoolType::CACHED) //// only in CACHED type
        {
            m_max_thread_num_ = max_thread_num;
        }
    }

    // 提交任务
    template <typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
    {
        // 萃取出来Task具体的返回值。
        using RType = decltype(func(args...));
        // 使用packaged_task包装任务，然后将参数通过forward完美转发，保留其左右值的类型并避免拷贝
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
        // 获取到该函数的返回值
        std::future<RType> result = task->get_future();
        // 将任务提交到任务队列中

        std::unique_lock<std::mutex> lock(m_task_mutex_);

        // 1. task队列已满，超时1s，任务提交失败
        if (!m_task_not_full_.wait_for(
                lock, std::chrono::seconds(1), [&]() { return m_task_queue_.size() < m_max_task_que_num_; }))
        {
            std::cout << " Submit failed!" << std::endl;
            auto task = std::make_shared<std::packaged_task<RType()>>([]() { return RType(); });
            (*task)();
            return task->get_future();
        }
        // 2. 队列未满，可以入队
        m_task_size_++; // atomic add
        // 注意这波包装，实际上的返回值我们已经通过future<>result返回了，这里将Task封装为了一个void()的function
        m_task_queue_.emplace([task]() { (*task)(); });

        // 唤醒Threads们(Consumer)
        m_task_not_empty_.notify_all();

        // 对于 CACHED 模式，Threads的个数是动态增长的，需要动态开启新的线程来完成过多的Task
        if (m_thread_pool_type_ == ThreadPoolType::CACHED && m_task_size_ > m_idle_thread_num_
            && m_cur_thread_num_ < m_max_thread_num_)
        {
            auto thread = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            auto thread_id = thread->get_thread_id();
            thread->start(); // 启动当前线程
            // 维护线程信息
            m_thread_pool_.emplace(thread_id, std::move(thread));
            m_cur_thread_num_++;  // 当前线程总数 ++
            m_idle_thread_num_++; // 空闲的线程++
        }

        return result; // 函数的返回值通过future来返回
    }

    // 开启线程池

    void start(uint init_thread_num)
    {
        // set 线程池相关信息
        m_is_running_ = true;
        m_init_thread_num_ = init_thread_num;

        // 创建线程并启动线程
        for (int i = 0; i < m_init_thread_num_; i++)
        {
            auto thread = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            auto thread_id = thread->get_thread_id();
            thread->start();
            m_thread_pool_.emplace(thread_id, std::move(thread));
            m_cur_thread_num_++;
            m_idle_thread_num_++;
        }
    }

private:
    // 线程函数： submitTask是生产者的话，threadFunc就是消费者。
    // 1. 从Task队列中获取Task
    // 2. 执行Task
    void threadFunc(uint thread_id)
    {
        // 用于在CACHED模式中记录太久未工作的线程
        auto last_time = std::chrono::high_resolution_clock::now();
        for (;;)
        { // 循环获取并处理任务
            Task task;
            {
                std::unique_lock<std::mutex> lock(m_task_mutex_); // 获取锁

                // 等待Task队列有任务，下面这个循环要干的事情就是要通过条件变量来合理地控制锁的释放
                while (m_task_queue_.size() == 0)
                {
                    // 如果目前线程池已经被终止了，可能是因为析构等原因。
                    // 而且当前线程没有任务执行，那么就直接释放这个线程
                    if (m_is_running_ == false)
                    {
                        m_thread_pool_.erase(thread_id);
                        m_cur_thread_num_--;
                        m_idle_thread_num_--;
                        m_pool_exit_.notify_all(); // 唤醒等待
                        return;
                    }
                    // 如果当前线程是CACHED模式，需要根据当前线程空闲的时间进行动态的释放调整
                    if (m_thread_pool_type_ == ThreadPoolType::CACHED)
                    {
                        auto now_time = std::chrono::high_resolution_clock::now();
                        auto duration = std::chrono::duration_cast<std::chrono::seconds>(now_time - last_time);
                        // 可以释放当前线程了
                        if (duration.count() >= THREAD_MAX_IDLE_TIME)
                        {
                            m_thread_pool_.erase(thread_id);
                            m_cur_thread_num_--;
                            m_idle_thread_num_--;
                            std::cout << "Idle too much time, thread : " << thread_id << " has been destroied"
                                      << std::endl;
                        }
                    }
                    // 目前虽然拿到Task队列的锁了，奈何队列中没有任务可以执行，所以通过条件变量来释放锁
                    // 一直等待被唤醒
                    m_task_not_empty_.wait(lock);
                }
                // 然后获取到任务了
                task = m_task_queue_.front();
                m_task_queue_.pop();
                m_idle_thread_num_--;
                m_task_size_--;
                std::cout << " Thread " << thread_id << " has got one task to execute." << std::endl;

                if (m_task_size_ > 0) // 如果还有任务继续唤醒一下其他的线程
                {
                    m_task_not_empty_.notify_all();
                }
                // 消耗掉一个任务之后，就唤醒生产者可以继续生产了
                m_task_not_full_.notify_all();
            }
            // 处理完之后，就可以安心释放掉锁，然后开始执行了
            if (task != nullptr)
            {
                task();
            }
            std::cout << "Thread " << thread_id << " finish its task." << std::endl;
            // 执行完之后继续空闲
            m_idle_thread_num_++;
            last_time = std::chrono::high_resolution_clock::now();
        }
    }

private:
    // threads management
    uint m_max_thread_num_;
    uint m_init_thread_num_; // for fixed type
    std::unordered_map<uint, std::unique_ptr<Thread>> m_thread_pool_;
    std::atomic_uint m_cur_thread_num_;  // 当前线程数
    std::atomic_uint m_idle_thread_num_; // 空闲线程数

    // task management
    using Task = std::function<void()>; // 将任务都封装成为void()类型的函数，方便调用
    uint m_max_task_que_num_;
    std::queue<Task> m_task_queue_;
    std::atomic_uint m_task_size_;
    std::mutex m_task_mutex_;
    std::condition_variable m_task_not_empty_; // 这里就等同于生产者和消费者模型中的两个条件变量
    std::condition_variable m_task_not_full_;  // 一个是任务队列为空，一个是任务队列满

    // ThreadPool setting
    ThreadPoolType m_thread_pool_type_;
    std::atomic_bool m_is_running_;
    std::condition_variable m_pool_exit_;
};

#endif