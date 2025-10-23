#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
//-------------------------
extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
}

namespace ImageFlow
{

/* 滤镜图对象 */
class FilterGraphCacheItem
{
    friend class FilterGraphPool;

private:
    AVFilterGraph *mGraph;                           // 滤镜图
    AVFilterContext *mBufferSrcVtx;                  // 输入缓冲区滤镜
    AVFilterContext *mBufferSinkCtx;                 // 输出缓冲区滤镜
    std::atomic<int> mUseCount = 1;                  // 引用计数
    std::atomic<bool> mInUse = false;                // 是否正在使用
    std::chrono::steady_clock::time_point mLastUsed; // 上次使用时间

public:
    FilterGraphCacheItem(
        AVFilterGraph *g,
        AVFilterContext *src,
        AVFilterContext *sink);

    ~FilterGraphCacheItem();

private:
    // 尝试获取使用权
    bool acquire();

    // 释放使用权
    void release();

    // 检查是否可清理
    bool canCleanup(std::chrono::seconds timeout = std::chrono::seconds(300)) const;

public:
    // 获取使用统计
    bool isInUse() const;
    int getUseCount() const;
    auto getLastUsed() const;

    // 获取滤镜图组件
    AVFilterGraph *getGraph() const;
    AVFilterContext *getBufferSrc() const;
    AVFilterContext *getBufferSink() const;
};

using FilterGraph = FilterGraphCacheItem;

/* 滤镜图池 */
class FilterGraphPool
{
public:
    using FilterGraphPtr = std::shared_ptr<FilterGraphCacheItem>;

public:
    FilterGraphPool(size_t maxSize = 100, std::chrono::seconds cleanupTimeout = std::chrono::seconds(300));
    ~FilterGraphPool();

    // 禁用拷贝
    FilterGraphPool(FilterGraphPool const &) = delete;
    FilterGraphPool &operator=(FilterGraphPool const &) = delete;

public:
    // 获取滤镜图  如果正在被使用会等待或返回nullptr
    FilterGraphPtr getFilterGraph(
        AVFrame const *inputFrame,
        std::string const &filterDesc,
        bool waitIfBusy = false);

    // 处理帧  简单处理
    int processFrame(
        AVFrame *inputFrame,
        std::string const &filterDesc,
        AVFrame **outputFrame);

    // 清理长时间未使用的滤镜图
    size_t cleanupUnused();

    // 强制清理所有缓存
    void clear();

    // 获取缓存统计信息
    size_t getCacheSize() const;
    size_t getMaxSize() const;
    bool setMaxSize(size_t maxSize);

    void setCleanupTimeout(std::chrono::seconds timeout);
    std::chrono::seconds getCleanupTimeout() const;

    // 打印缓存状态（调试用）
    void printCacheStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mPimpl;
};

} // namespace ImageFlow
