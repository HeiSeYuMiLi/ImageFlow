#pragma once

#include <memory>
#include <string>

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace ImageFlow
{

/* 滤镜图对象 */
class FilterGraphCacheItem
{
    friend class FilterGraphPool;

public:
    AVFilterGraph *graph;
    AVFilterContext *bufferSrcVtx;
    AVFilterContext *bufferSinkCtx;

private:
    int refCount = 1; // 引用计数，延时清理

public:
    FilterGraphCacheItem(
        AVFilterGraph *g,
        AVFilterContext *src,
        AVFilterContext *sink);

    ~FilterGraphCacheItem();
};

using FilterGraph = FilterGraphCacheItem;

/* 滤镜图池 */
class FilterGraphPool
{
public:
    using FilterGraphPtr = std::shared_ptr<FilterGraphCacheItem>;

public:
    FilterGraphPool(size_t maxSize = 100);

    // 禁用拷贝
    FilterGraphPool(FilterGraphPool const &) = delete;
    FilterGraphPool &operator=(FilterGraphPool const &) = delete;

public:
    // 获取滤镜图
    FilterGraphPtr getFilterGraph(
        int width, int height,
        AVPixelFormat pixelFormat,
        std::string const &filterDesc);

    // 处理帧  简单处理
    int processFrame(
        FilterGraphPtr filterItem,
        AVFrame *inputFrame,
        AVFrame **outputFrame);

    // 清理缓存
    void clear();

    // 获取缓存统计信息
    size_t getCacheSize() const;
    size_t getMaxSize() const;
    void setMaxSize(size_t maxSize);

    // 打印缓存状态（调试用）
    void printCacheStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mPimpl;
};

} // namespace ImageFlow
