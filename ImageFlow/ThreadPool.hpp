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

/* �������ȼ� */
enum class TaskPriority
{
    LOW,
    NORMAL,
    HIGH,
    URGENT, // ����
};

/* �ܾ����� */
enum class RejectPolicy
{
    THROW,   // �׳��쳣
    BLOCK,   // �����ȴ�
    DISCARD, // ��Ĭ����
};

/* �����װ�� �������� ����洢 */
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

/* ���ȼ��Ƚ��� */
struct TaskComparator
{
    bool operator()(const TaskWrapper &a, const TaskWrapper &b) const
    {
        // ���ȼ��ߵ���ִ��  �ύ�����ִ��
        if (a.getPriority() != b.getPriority())
        {
            return static_cast<int>(a.getPriority()) < static_cast<int>(b.getPriority());
        }
        return a.getSubmitTime() > b.getSubmitTime();
    }
};

/* �̳߳� */
class ThreadPool
{
private:
    using TaskQueue = std::priority_queue<TaskWrapper, std::vector<TaskWrapper>, TaskComparator>;

public:
    /* ����״̬ */
    struct TaskStats
    {
        size_t submitted = 0; // �ύ��
        size_t completed = 0; // �����
        size_t failed = 0;    // ʧ����
    };

    /* �̳߳�״̬ */
    struct PoolStatus
    {
        size_t queueSize;    // ��ǰ���д�С
        size_t activeTasks;  // ��Ծ������
        size_t totalThreads; // �߳�����
        size_t maxQueueSize; // �����д�С
    };

public:
    explicit ThreadPool(
        size_t numThreads = std::thread::hardware_concurrency(),
        size_t maxQueueSize = 1000,
        RejectPolicy policy = RejectPolicy::BLOCK // Ĭ������
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
            std::chrono::milliseconds(0), // Ĭ�ϲ���ʱ
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

    // ��Ա����֧�� �ύ��Ա��������
    template <typename R, typename C, typename... Args>
    auto submitMember(C *obj, R (C::*memberFunc)(Args...), Args &&...args)
    {
        return submit(
            [obj, memberFunc, args...] mutable -> R
            { return (obj->*memberFunc)(args...); });
    }

    // const��Ա����֧��
    template <typename R, typename C, typename... Args>
    auto submitMemberConst(const C *obj, R (C::*memberFunc)(Args...) const, Args &&...args)
    {
        return submit(
            [obj, memberFunc, args...] mutable -> R
            { return (obj->*memberFunc)(args...); });
    }

    // lambda���ʽ֧�֣���ʽָ���������ͣ�
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

        // ֪ͨ���еȴ����߳�
        mCondition.notify_all();

        // �ȴ����й����߳̽���
        for (std::thread &worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }

        mWorkers.clear();
        std::cout << "ThreadPool shutdown completed." << std::endl;
    }

    // ���Źر� �ȴ������������
    void shutdownGraceful()
    {
        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            if (mStop.load())
                return;
            mStop.store(true);
        }

        // �ȴ��������ύ���������
        waitAll();

        // ֪ͨ���еȴ����߳�
        mCondition.notify_all();

        // �ȴ����й����߳̽���
        for (std::thread &worker : mWorkers)
        {
            if (worker.joinable())
                worker.join();
        }

        mWorkers.clear();
        std::cout << "ThreadPool graceful shutdown completed." << std::endl;
    }

    /**
     * @brief �����̳߳أ���������������´��������̡߳�
     * @brief �÷��������̰߳�ȫ�ģ�ȷ���ڵ���ʱû�������߳���ʹ���̳߳ء�
     */
    void restart(size_t numThreads = std::thread::hardware_concurrency())
    {
        shutdown(); // �ȹر�

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);
            mStop.store(false);
            // ����������
            while (!mTasks.empty())
                mTasks.pop();
            mActiveTasks.store(0);
        }

        // �����µĹ����߳�
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

    // �ȴ�����������ɣ����������еĺ�����ִ�еģ�
    void waitAll()
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mAllDoneCondition.wait(lock, [this]
                               { return mTasks.empty() && mActiveTasks.load() == 0; });
    }

    // ����ʱ�ĵȴ�
    bool waitAllFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        return mAllDoneCondition.wait_for(lock, timeout, [this]
                                          { return mTasks.empty() && mActiveTasks.load() == 0; });
    }

    // �ȴ�ֱ��ָ��ʱ��
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
    std::vector<std::thread> mWorkers;                          // �����߳�
    TaskQueue mTasks;                                           // �������
    mutable std::mutex mQueueMutex;                             // ������л�����
    std::condition_variable mCondition;                         // ���������������
    std::condition_variable mAllDoneCondition;                  // �������������������
    std::condition_variable mNotFullCondition;                  // ����δ����������
    std::atomic<bool> mStop;                                    // �̳߳�ֹͣ��־
    std::atomic<size_t> mActiveTasks;                           // ��Ծ������
    size_t mMaxQueueSize;                                       // �����д�С
    std::unordered_map<std::string, TaskStats> mTaskStatistics; // ����ͳ��
    RejectPolicy mRejectPolicy;                                 // ������ʱ�ľܾ�����

private:
    template <typename F, typename... Args>
    auto submitImpl(
        TaskPriority priority,
        std::string const &taskName,
        std::chrono::milliseconds timeout,
        F &&f, Args &&...args)
    {
        using ResultType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        // ����packaged_task
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

            // ���ݾܾ����Դ�������������
            if (mTasks.size() >= mMaxQueueSize)
            {
                switch (mRejectPolicy)
                {
                case RejectPolicy::THROW:
                    throw std::runtime_error("submit timeout: queue is full");

                case RejectPolicy::BLOCK:
                    if (timeout.count() > 0)
                    {
                        // ����ʱ�ĵȴ�
                        if (!mNotFullCondition.wait_for(lock, timeout, [this]
                                                        { return mTasks.size() < mMaxQueueSize || mStop.load(); }))
                        {
                            throw std::runtime_error("submit timeout: queue is full");
                        }
                    }
                    else
                    {
                        // �����ڵȴ�
                        mNotFullCondition.wait(lock, [this]
                                               { return mTasks.size() < mMaxQueueSize || mStop.load(); });
                    }
                    if (mStop.load())
                        throw std::runtime_error("submit on stopped ThreadPool");
                    break;

                case RejectPolicy::DISCARD:
                    mTaskStatistics[taskName + "_discarded"].submitted++;
                    // ��������ɵ�future�������쳣
                    std::promise<ResultType> promise;
                    promise.set_exception(std::make_exception_ptr(
                        std::runtime_error("Task discarded due to full queue")));
                    return promise.get_future();
                }
            }

            // ����TaskWrapper����װʵ�ʵ�ִ���߼�
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

        // ֪ͨ��������пռ���
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
