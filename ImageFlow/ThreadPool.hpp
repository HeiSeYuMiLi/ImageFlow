#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

/* 任务优先级 */
enum class TaskPriority
{
    LOW,
    NORMAL,
    HIGH,
    URGENT, // 紧急
};

/* 拒绝策略 */
enum class RejectPolicy
{
    THROW,   // 抛出异常
    BLOCK,   // 阻塞等待
    DISCARD, // 静默丢弃
};

/* 任务包装器 擦除类型 方便存储 */
class TaskWrapper
{
private:
    using TaskFunc = std::function<void()>;

private:
    std::string mName;
    TaskFunc mTaskFunc;
    TaskPriority mPriority;
    std::chrono::steady_clock::time_point mSubmitTime;

public:
    template <typename F>
    TaskWrapper(F &&func, TaskPriority p, std::string taskName)
        : mPriority(p), mName(std::move(taskName)),
          mTaskFunc(std::forward<F>(func)),
          mSubmitTime(std::chrono::steady_clock::now())
    {
    }

public:
    void execute() const
    {
        mTaskFunc();
    }

    TaskPriority getPriority() const
    {
        return mPriority;
    }

    auto getSubmitTime() const
    {
        return mSubmitTime;
    }

    const std::string &getName() const
    {
        return mName;
    }
};

/* 优先级比较器 */
struct TaskComparator
{
    bool operator()(const TaskWrapper &a, const TaskWrapper &b) const
    {
        // 优先级高的先执行  提交早的先执行
        if (a.getPriority() != b.getPriority())
        {
            return static_cast<int>(a.getPriority()) < static_cast<int>(b.getPriority());
        }
        return a.getSubmitTime() > b.getSubmitTime();
    }
};

/* 线程池 */
class ThreadPool
{
private:
    using TaskQueue = std::priority_queue<TaskWrapper, std::vector<TaskWrapper>, TaskComparator>;

public:
    /* 任务状态 */
    struct TaskStats
    {
        size_t submitted = 0; // 提交数
        size_t completed = 0; // 完成数
        size_t failed = 0;    // 失败数
    };

    /* 线程池状态 */
    struct PoolStatus
    {
        size_t queueSize;    // 当前队列大小
        size_t activeTasks;  // 活跃任务数
        size_t totalThreads; // 线程总数
        size_t maxQueueSize; // 最大队列大小
    };

public:
    explicit ThreadPool(
        size_t numThreads = std::thread::hardware_concurrency(),
        size_t maxQueueSize = 1000,
        RejectPolicy policy = RejectPolicy::BLOCK // 默认阻塞
        )
        : mStop(false), mMaxQueueSize(maxQueueSize),
          mActiveTasks(0), mRejectPolicy(policy)
    {
        for (size_t i = 0; i < numThreads; ++i)
        {
            // clang-format off
            mWorkers.emplace_back([this] { workerLoop(); });
            // clang-format on
        }
    }

    ~ThreadPool()
    {
        shutdown();
    }

public:
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args)
    {
        return submitImpl(
            TaskPriority::NORMAL,
            "unnamed_task",
            std::chrono::milliseconds(0), // 默认不超时
            std::forward<F>(f),
            std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submitWithPriority(
        TaskPriority priority,
        std::chrono::milliseconds timeout,
        F &&f, Args &&...args)
    {
        return submitImpl(
            priority,
            "unnamed_task",
            timeout,
            std::forward<F>(f),
            std::forward<Args>(args)...);
    }

    template <typename F, typename... Args>
    auto submitWithName(
        std::string const &taskName,
        TaskPriority priority,
        std::chrono::milliseconds timeout,
        F &&f, Args &&...args)
    {
        return submitImpl(
            priority,
            taskName,
            timeout,
            std::forward<F>(f),
            std::forward<Args>(args)...);
    }

    // 成员函数支持 提交成员函数任务
    template <typename R, typename C, typename... Args>
    auto submitMember(C *obj, R (C::*memberFunc)(Args...), Args &&...args)
    {
        return submit(
            [obj, memberFunc, args...] mutable -> R
            { return (obj->*memberFunc)(args...); });
    }

    // const成员函数支持
    template <typename R, typename C, typename... Args>
    auto submitMemberConst(const C *obj, R (C::*memberFunc)(Args...) const, Args &&...args)
    {
        return submit(
            [obj, memberFunc, args...] mutable -> R
            { return (obj->*memberFunc)(args...); });
    }

    // lambda表达式支持（显式指定返回类型）
    template <typename R, typename F, typename... Args>
    auto submitLambda(F &&f, Args &&...args)
    {
        return submit(std::forward<F>(f), std::forward<Args>(args)...);
    }

    void shutdown()
    {
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            if (mStop.load())
                return;
            mStop.store(true);
        }

        // 通知所有等待的线程
        mCondition.notify_all();

        // 等待所有工作线程结束
        for (std::thread &worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }

        mWorkers.clear();
        std::cout << "ThreadPool shutdown completed." << std::endl;
    }

    // 优雅关闭 等待所有任务完成
    void shutdownGraceful()
    {
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            if (mStop.load())
                return;
            mStop.store(true);
        }

        // 等待所有已提交的任务完成
        waitAll();

        // 通知所有等待的线程
        mCondition.notify_all();

        // 等待所有工作线程结束
        for (std::thread &worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }

        mWorkers.clear();
        std::cout << "ThreadPool graceful shutdown completed." << std::endl;
    }

    /**
     * @brief 重启线程池，清空所有任务并重新创建工作线程。
     * @brief 该方法不是线程安全的，确保在调用时没有其他线程在使用线程池。
     */
    void restart(size_t numThreads = std::thread::hardware_concurrency())
    {
        shutdown(); // 先关闭

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mStop.store(false);
            // 清空任务队列
            while (!mTasks.empty())
                mTasks.pop();
            mActiveTasks.store(0);
        }

        // 创建新的工作线程
        for (size_t i = 0; i < numThreads; ++i)
        {
            // clang-format off
            mWorkers.emplace_back([this] { workerLoop(); });
            // clang-format on
        }

        std::cout << "ThreadPool restarted with " << numThreads << " threads." << std::endl;
    }

    void setRejectPolicy(RejectPolicy policy)
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mRejectPolicy = policy;
    }

    // 等待所有任务完成（包括队列中的和正在执行的）
    void waitAll()
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mAllDoneCondition.wait(lock, [this]
                               { return mTasks.empty() && mActiveTasks.load() == 0; });
    }

    // 带超时的等待
    bool waitAllFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        return mAllDoneCondition.wait_for(lock, timeout, [this]
                                          { return mTasks.empty() && mActiveTasks.load() == 0; });
    }

    // 等待直到指定时间
    bool waitAllUntil(std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        return mAllDoneCondition.wait_until(lock, deadline, [this]
                                            { return mTasks.empty() && mActiveTasks.load() == 0; });
    }

    std::unordered_map<std::string, TaskStats> getTaskStatistics() const
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        return mTaskStatistics;
    }

    PoolStatus getStatus() const
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        return PoolStatus{mTasks.size(), mActiveTasks.load(), mWorkers.size(), mMaxQueueSize};
    }

private:
    std::vector<std::thread> mWorkers;                          // 工作线程
    TaskQueue mTasks;                                           // 任务队列
    mutable std::mutex mQueueMutex;                             // 任务队列互斥锁
    std::condition_variable mCondition;                         // 任务可用条件变量
    std::condition_variable mAllDoneCondition;                  // 所有任务完成条件变量
    std::condition_variable mNotFullCondition;                  // 队列未满条件变量
    std::atomic<bool> mStop;                                    // 线程池停止标志
    std::atomic<size_t> mActiveTasks;                           // 活跃任务数
    size_t mMaxQueueSize;                                       // 最大队列大小
    std::unordered_map<std::string, TaskStats> mTaskStatistics; // 任务统计
    RejectPolicy mRejectPolicy;                                 // 队列满时的拒绝策略

private:
    template <typename F, typename... Args>
    auto submitImpl(
        TaskPriority priority,
        std::string const &taskName,
        std::chrono::milliseconds timeout,
        F &&f, Args &&...args)
    {
        using ResultType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        // 创建packaged_task
        auto task = std::make_shared<std::packaged_task<ResultType()>>(
            [func = std::forward<F>(f), argsTuple = std::make_tuple(std::forward<Args>(args)...)]() mutable
            {
                return std::apply(func, std::move(argsTuple));
            });

        auto future = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);

            if (mStop.load())
                throw std::runtime_error("submit on stopped ThreadPool");

            // 根据拒绝策略处理队列满的情况
            if (mTasks.size() >= mMaxQueueSize)
            {
                switch (mRejectPolicy)
                {
                case RejectPolicy::THROW:
                    throw std::runtime_error("submit timeout: queue is full");

                case RejectPolicy::BLOCK:
                    if (timeout.count() > 0)
                    {
                        // 带超时的等待
                        if (!mNotFullCondition.wait_for(lock, timeout, [this]
                                                        { return mTasks.size() < mMaxQueueSize || mStop.load(); }))
                        {
                            throw std::runtime_error("submit timeout: queue is full");
                        }
                    }
                    else
                    {
                        // 无限期等待
                        mNotFullCondition.wait(lock, [this]
                                               { return mTasks.size() < mMaxQueueSize || mStop.load(); });
                    }
                    if (mStop.load())
                        throw std::runtime_error("submit on stopped ThreadPool");
                    break;

                case RejectPolicy::DISCARD:
                    mTaskStatistics[taskName + "_discarded"].submitted++;
                    // 创建已完成的future，包含异常
                    std::promise<ResultType> promise;
                    promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("Task discarded due to full queue")));
                    return promise.get_future();
                }
            }

            // 创建TaskWrapper，包装实际的执行逻辑
            TaskWrapper wrapper(
                [task]
                { (*task)(); },
                priority, taskName);

            mTasks.push(std::move(wrapper));
            mTaskStatistics[taskName].submitted++;
        }

        mCondition.notify_one();
        return future;
    }

    void workerLoop()
    {
        while (!mStop.load())
        {
            auto task = getNextTask();
            if (!task)
                break;

            executeTask(std::move(task));
        }
    }

    std::unique_ptr<TaskWrapper> getNextTask()
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);

        mCondition.wait(lock, [this]
                        { return mStop.load() || !mTasks.empty(); });

        if (mStop.load() && mTasks.empty())
            return nullptr;

        // auto task = std::make_unique<TaskWrapper>(mTasks.top());
        auto task = std::make_unique<TaskWrapper>(std::move(const_cast<TaskWrapper &>(mTasks.top())));
        mTasks.pop();
        mActiveTasks++;

        // 通知任务队列有空间了
        mNotFullCondition.notify_one();

        return task;
    }

    void executeTask(std::unique_ptr<TaskWrapper> task)
    {
        try
        {
            task->execute();

            std::unique_lock<std::mutex> lock(mQueueMutex);
            mTaskStatistics[task->getName()].completed++;
        }
        catch (...)
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mTaskStatistics[task->getName()].failed++;
        }

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mActiveTasks--;
            if (mTasks.empty() && mActiveTasks.load() == 0)
            {
                mAllDoneCondition.notify_all();
            }
        }
    }
};
