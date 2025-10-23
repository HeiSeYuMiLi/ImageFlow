#include "FilterGraphPool.h"
//--------------------------
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
//--------------------------
extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

using namespace ImageFlow;

struct FilterGraphCacheKey
{
public:
    int width;              // 图像宽度
    int height;             // 图像高度
    AVPixelFormat pixelFmt; // 像素格式
    std::string filterDesc; // 滤镜描述字符串

public:
    static FilterGraphCacheKey fromFrame(AVFrame const *frame, std::string const &descr)
    {
        return FilterGraphCacheKey{
            frame->width,
            frame->height,
            static_cast<AVPixelFormat>(frame->format),
            descr.c_str()};
    }

    bool operator==(FilterGraphCacheKey const &other) const
    {
        return width == other.width &&
               height == other.height &&
               pixelFmt == other.pixelFmt &&
               filterDesc == other.filterDesc;
    }
};

namespace std
{
template <>
struct hash<::FilterGraphCacheKey>
{
    size_t operator()(::FilterGraphCacheKey const &key) const
    {
        return hash<int>()(key.width) ^
               (hash<int>()(key.height) << 1) ^
               (hash<int>()(key.pixelFmt) << 2) ^
               (hash<string>()(key.filterDesc) << 3);
    }
};
} // namespace std

//----------------------------------------------------------------

FilterGraphCacheItem::FilterGraphCacheItem(
    AVFilterGraph *g,
    AVFilterContext *src,
    AVFilterContext *sink)
    : mGraph(g), mBufferSrcVtx(src), mBufferSinkCtx(sink)
{
    mLastUsed = std::chrono::steady_clock::now();
}

FilterGraphCacheItem::~FilterGraphCacheItem()
{
    if (mGraph)
    {
        avfilter_graph_free(&mGraph);
        // bufferSrcVtx和bufferSinkCtx会被graph一起释放
    }
}

bool FilterGraphCacheItem::acquire()
{
    bool expected = false;
    if (mInUse.compare_exchange_strong(expected, true))
    {
        mUseCount++;
        mLastUsed = std::chrono::steady_clock::now();
        return true;
    }
    return false; // 正在被其他线程使用
}

void FilterGraphCacheItem::release()
{
    mInUse.store(false);
    mLastUsed = std::chrono::steady_clock::now();
}

bool FilterGraphCacheItem::canCleanup(std::chrono::seconds timeout) const
{
    return !mInUse.load() && (std::chrono::steady_clock::now() - mLastUsed) > timeout;
}

int FilterGraphCacheItem::getUseCount() const
{
    return mUseCount.load();
}

bool FilterGraphCacheItem::isInUse() const
{
    return mInUse.load();
}

auto FilterGraphCacheItem::getLastUsed() const
{
    return mLastUsed;
}

AVFilterGraph *FilterGraphCacheItem::getGraph() const
{
    return mGraph;
}

AVFilterContext *FilterGraphCacheItem::getBufferSrc() const
{
    return mBufferSrcVtx;
}

AVFilterContext *FilterGraphCacheItem::getBufferSink() const
{
    return mBufferSinkCtx;
}

//----------------------------------------------------------------

struct FilterGraphPool::Impl
{
public:
    size_t mMaxSize;
    std::mutex mMutex;
    std::atomic<std::chrono::seconds::rep> mCleanupTimeout;
    std::unordered_map<FilterGraphCacheKey, FilterGraphPtr> mCache;

public:
    Impl(size_t maxSize, std::chrono::seconds cleanupTimeout)
        : mMaxSize(maxSize)
    {
        mCleanupTimeout.store(cleanupTimeout.count());
    }

    ~Impl()
    {
        std::lock_guard<std::mutex> _(mMutex);
        mCache.clear();
    }

public:
    // 创建新的滤镜图
    FilterGraphPtr createFilterGraph(
        AVFrame const *frame,
        std::string const &filterDesc)
    {
        // 创建滤镜图
        auto filterGraph = avfilter_graph_alloc();
        if (!filterGraph)
        {
            // std::cerr << "无法创建滤镜图" << std::endl;
            return nullptr;
        }

        AVFilterContext *bufferSrcCtx = nullptr;
        AVFilterContext *bufferSinkCtx = nullptr;

        //----------------------------------------------------

        // 创建缓冲区源
        auto bufferSrc = avfilter_get_by_name("buffer");
        if (!bufferSrc)
        {
            avfilter_graph_free(&filterGraph);
            return nullptr;
        }

        // 配置缓冲区源
        char args[512];
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
                 frame->width, frame->height, frame->format);

        if (avfilter_graph_create_filter(
                &bufferSrcCtx, bufferSrc, "in",
                args, nullptr, filterGraph) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "无法创建缓冲区源" << std::endl;
            return nullptr;
        }

        //----------------------------------------------------

        // 创建缓冲区接收器
        auto bufferSink = avfilter_get_by_name("buffersink");
        if (!bufferSink)
        {
            avfilter_graph_free(&filterGraph);
            return nullptr;
        }

        // 配置缓冲区接收器
        if (avfilter_graph_create_filter(
                &bufferSinkCtx, bufferSink, "out",
                nullptr, nullptr, filterGraph) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "无法创建缓冲区接收器" << std::endl;
            return nullptr;
        }

        //----------------------------------------------------

        // 配置滤镜链
        AVFilterInOut *outputs = avfilter_inout_alloc();
        AVFilterInOut *inputs = avfilter_inout_alloc();

        if (!outputs || !inputs)
        {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            avfilter_graph_free(&filterGraph);
            return nullptr;
        }

        outputs->name = av_strdup("in");
        outputs->filter_ctx = bufferSrcCtx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = bufferSinkCtx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        auto ret = avfilter_graph_parse_ptr(
            filterGraph, filterDesc.c_str(),
            &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (ret < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "无法解析滤镜图" << std::endl;
            return nullptr;
        }

        // 配置滤镜图
        if (avfilter_graph_config(filterGraph, nullptr) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "无法配置滤镜图" << std::endl;
            return nullptr;
        }

        return std::make_shared<FilterGraphCacheItem>(filterGraph, bufferSrcCtx, bufferSinkCtx);
    }

    // 生成缓存键
    FilterGraphCacheKey makeKey(
        AVFrame const *frame,
        std::string const &filterDesc) const
    {
        return FilterGraphCacheKey::fromFrame(frame, filterDesc);
    }
};

//----------------------------------------------------------------

FilterGraphPool::FilterGraphPool(size_t maxSize, std::chrono::seconds cleanupTimeout)
    : mPimpl(new Impl(maxSize, cleanupTimeout)) {}

FilterGraphPool::~FilterGraphPool() = default;

FilterGraphPool::FilterGraphPtr
FilterGraphPool::getFilterGraph(
    AVFrame const *frame,
    std::string const &filterDesc,
    bool waitIfBusy)
{
    if (!frame)
        return nullptr;

    auto key = mPimpl->makeKey(frame, filterDesc);
    std::unique_lock<std::mutex> lock(mPimpl->mMutex);

    // 查找缓存
    if (auto it = mPimpl->mCache.find(key); it != mPimpl->mCache.end())
    {
        // 尝试获取使用权
        if (it->second->acquire())
            return it->second;
        else if (waitIfBusy)
        {
            // 这里实现一个简单的等待机制 - 指数退避 + 等待

            int maxRetries = 5;   // 最大重试次数
            int baseBelayMs = 10; // 10ms基础延迟
            for (int retry = 0; retry < maxRetries; ++retry)
            {
                lock.unlock();
                {
                    // 指数退避延迟
                    int delayMs = baseBelayMs * (1 << retry); // 10, 20, 40, 80, 160ms
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
                lock.lock();

                // 重新检查
                auto it = mPimpl->mCache.find(key);
                if (it != mPimpl->mCache.end() &&
                    it->second->acquire())
                    return it->second;
            }
        }
        return nullptr;
    }

    // 缓存未命中，检查缓存大小
    if (mPimpl->mCache.size() >= mPimpl->mMaxSize)
    {
        cleanupUnused();

        // 如果还是满的，移除一个最久未使用的
        if (mPimpl->mCache.size() >= mPimpl->mMaxSize)
        {
            auto oldestIt = mPimpl->mCache.begin();
            for (auto current = mPimpl->mCache.begin(); current != mPimpl->mCache.end(); ++current)
            {
                if (current->second->getLastUsed() < oldestIt->second->getLastUsed() &&
                    !current->second->isInUse())
                    oldestIt = current;
            }
            if (!oldestIt->second->isInUse())
            { // 移除最久未使用的
                mPimpl->mCache.erase(oldestIt);
            }
            else
            { // 无可用空间
                return nullptr;
            }
        }
    }

    // 创建新的滤镜图
    auto newItem = mPimpl->createFilterGraph(frame, filterDesc);
    if (newItem)
    {
        if (newItem->acquire())
        {
            mPimpl->mCache[key] = newItem;
            return newItem;
        }
    }
    return nullptr;
}

int FilterGraphPool::processFrame(
    AVFrame *inputFrame,
    std::string const &filterDesc,
    AVFrame **outputFrame)
{
    if (!inputFrame)
    {
        return AVERROR(EINVAL);
    }

    // 内部获取滤镜图 确保在使用完后释放
    auto filterItem = getFilterGraph(inputFrame, filterDesc, true);
    if (!filterItem)
    {
        return AVERROR(ENOMEM);
    }

    // 使用RAII确保释放
    struct FilterGuard
    {
        std::shared_ptr<FilterGraphCacheItem> item;
        ~FilterGuard()
        {
            if (item)
                item->release();
        }
    } guard{filterItem};

    // 发送帧到滤镜图
    int ret = av_buffersrc_add_frame_flags(filterItem->mBufferSrcVtx,
                                           inputFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
    {
        return ret;
    }

    // 分配输出帧
    *outputFrame = av_frame_alloc();
    if (!*outputFrame)
    {
        return AVERROR(ENOMEM);
    }

    // 从滤镜图接收帧
    ret = av_buffersink_get_frame(filterItem->mBufferSinkCtx, *outputFrame);
    if (ret < 0)
    {
        av_frame_free(outputFrame);
        return ret;
    }

    return 0;
}

size_t FilterGraphPool::cleanupUnused()
{
    std::lock_guard<std::mutex> _(mPimpl->mMutex);

    std::chrono::seconds currentTimeout(mPimpl->mCleanupTimeout.load());
    std::vector<FilterGraphCacheKey> toRemove;
    for (auto &&[key, value] : mPimpl->mCache)
    {
        if (value->canCleanup(currentTimeout))
            toRemove.push_back(key);
    }

    for (auto &&key : toRemove)
        mPimpl->mCache.erase(key);
    return toRemove.size();
}

void FilterGraphPool::clear()
{
    std::lock_guard<std::mutex> _(mPimpl->mMutex);
    mPimpl->mCache.clear();
}

size_t FilterGraphPool::getCacheSize() const
{
    std::lock_guard<std::mutex> lock(mPimpl->mMutex);
    return mPimpl->mCache.size();
}

size_t FilterGraphPool::getMaxSize() const
{
    return mPimpl->mMaxSize;
}

bool FilterGraphPool::setMaxSize(size_t maxSize)
{
    std::lock_guard<std::mutex> lock(mPimpl->mMutex);

    if (mPimpl->mCache.size() > mPimpl->mMaxSize)
    { // 如果新的大小小于当前缓存大小，移除多余的项
        cleanupUnused();

        // 如果还是太大，强制移除一些（按最后使用时间）
        while (mPimpl->mCache.size() > mPimpl->mMaxSize)
        {
            auto oldestIt = mPimpl->mCache.begin();
            for (auto it = mPimpl->mCache.begin(); it != mPimpl->mCache.end(); ++it)
            {
                if (it->second->getLastUsed() < oldestIt->second->getLastUsed() &&
                    !it->second->isInUse())
                    oldestIt = it;
            }
            if (!oldestIt->second->isInUse())
                mPimpl->mCache.erase(oldestIt);
            else
                return false; // 在使用中，无法清理
        }
    }
    mPimpl->mMaxSize = maxSize;
    return true;
}

void FilterGraphPool::setCleanupTimeout(std::chrono::seconds timeout)
{
    mPimpl->mCleanupTimeout.store(timeout.count());
}

std::chrono::seconds FilterGraphPool::getCleanupTimeout() const
{
    return std::chrono::seconds(mPimpl->mCleanupTimeout.load());
}

void FilterGraphPool::printCacheStatus() const
{
    std::lock_guard<std::mutex> lock(mPimpl->mMutex);

    std::chrono::seconds currentTimeout(mPimpl->mCleanupTimeout.load());
    std::cout << "=== 滤镜图缓存的状态 ===" << std::endl;
    std::cout << "  总缓存图数：" << mPimpl->mCache.size() << std::endl;
    std::cout << "  最大缓存数：" << mPimpl->mMaxSize << std::endl;
    std::cout << " 清理时限：" << currentTimeout.count() << " 秒" << std::endl;

    int inUseCount = 0;
    int totalUseCount = 0;
    for (auto &&[key, value] : mPimpl->mCache)
    {
        if (value->isInUse())
            inUseCount++;
        totalUseCount += value->getUseCount();

        auto timeSinceUse = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - value->getLastUsed());

        const char *pixFmtName = av_get_pix_fmt_name(key.pixelFmt);
        std::cout << "  - " << key.width << "x" << key.height
                  << " 格式:" << (pixFmtName ? pixFmtName : "unknown")
                  << " 引用数:" << value->getUseCount()
                  << " 使用中:" << (value->isInUse() ? "是" : "否")
                  << " 上次使用:" << timeSinceUse.count() << "s 之前"
                  << std::endl;
    }
    std::cout << "当前正在使用：" << inUseCount << std::endl;
    std::cout << "使用总数：" << totalUseCount << std::endl;
    std::cout << "=================================" << std::endl;
}