#include "FilterGraphPool.h"
//--------------------------
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
//--------------------------
extern "C"
{
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

using namespace ImageFlow;

struct FilterGraphCacheKey
{
public:
    int width;
    int height;
    AVPixelFormat pixelFmt;
    std::string filterDesc;

public:
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
    : graph(g), bufferSrcVtx(src), bufferSinkCtx(sink) {}

FilterGraphCacheItem::~FilterGraphCacheItem()
{
    if (graph)
    {
        avfilter_graph_free(&graph);
        // bufferSrcVtx和bufferSinkCtx会被graph一起释放
    }
}

//----------------------------------------------------------------

struct FilterGraphPool::Impl
{
public:
    size_t mMaxSize;
    std::mutex mMutex;
    std::unordered_map<FilterGraphCacheKey, FilterGraphPtr> mCache;

public:
    Impl(size_t maxSize)
        : mMaxSize(maxSize) {}

    ~Impl()
    {
        std::lock_guard<std::mutex> _(mMutex);
        mCache.clear();
    }

public:
    // 创建新的滤镜图
    FilterGraphPtr createFilterGraph(
        int width, int height,
        AVPixelFormat pixelFmt,
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
                 width, height, pixelFmt);

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
        int width, int height,
        AVPixelFormat pixelFmt,
        std::string const &filterDesc) const
    {
        return FilterGraphCacheKey{width, height, pixelFmt, filterDesc};
    }
};

//----------------------------------------------------------------

FilterGraphPool::FilterGraphPool(size_t maxSize)
    : mPimpl(new Impl(maxSize)) {}

FilterGraphPool::FilterGraphPtr
FilterGraphPool::getFilterGraph(
    int width, int height,
    AVPixelFormat pixelFmt,
    std::string const &filterDesc)
{
    std::lock_guard<std::mutex> _(mPimpl->mMutex);

    auto key = mPimpl->makeKey(width, height, pixelFmt, filterDesc);

    // 查找缓存
    if (auto it = mPimpl->mCache.find(key); it != mPimpl->mCache.end())
    {
        // 找到缓存，增加引用计数
        it->second->refCount++;
        return it->second;
    }

    // 检查缓存大小
    if (mPimpl->mCache.size() >= mPimpl->mMaxSize)
    {
        // 简单的LRU策略：移除第一个元素（实际生产环境可以用更复杂的策略）
        mPimpl->mCache.erase(mPimpl->mCache.begin());
    }

    // 创建新的滤镜图
    auto newItem = mPimpl->createFilterGraph(width, height, pixelFmt, filterDesc);
    if (newItem)
    {
        mPimpl->mCache[key] = newItem;
    }

    return newItem;
}

int FilterGraphPool::processFrame(
    FilterGraphPtr filterItem,
    AVFrame *inputFrame,
    AVFrame **outputFrame)
{

    if (!filterItem || !inputFrame)
    {
        return AVERROR(EINVAL);
    }

    // 发送帧到滤镜图
    int ret = av_buffersrc_add_frame_flags(filterItem->bufferSrcVtx,
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
    ret = av_buffersink_get_frame(filterItem->bufferSinkCtx, *outputFrame);
    if (ret < 0)
    {
        av_frame_free(outputFrame);
        return ret;
    }

    return 0;
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

void FilterGraphPool::setMaxSize(size_t max_size)
{
    std::lock_guard<std::mutex> lock(mPimpl->mMutex);
    mPimpl->mMaxSize = max_size;

    // 如果新的大小小于当前缓存大小，移除多余的项
    while (mPimpl->mCache.size() > mPimpl->mMaxSize)
    {
        mPimpl->mCache.erase(mPimpl->mCache.begin());
    }
}

void FilterGraphPool::printCacheStatus() const
{
    std::lock_guard<std::mutex> lock(mPimpl->mMutex);

    std::cout << "滤镜图缓存的状态：" << std::endl;
    std::cout << "  总缓存图数：" << mPimpl->mCache.size() << std::endl;
    std::cout << "  最大缓存数：" << mPimpl->mMaxSize << std::endl;

    for (const auto &item : mPimpl->mCache)
    {
        const auto &key = item.first;
        const auto &value = item.second;

        std::cout << "  - " << key.width << "x" << key.height
                  << " 格式：" << key.pixelFmt
                  << " 引用数：" << value->refCount
                  << " 滤镜：" << key.filterDesc.substr(0, 30)
                  << (key.filterDesc.length() > 30 ? "..." : "") << std::endl;
    }
}