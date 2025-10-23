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
    int width;              // ͼ����
    int height;             // ͼ��߶�
    AVPixelFormat pixelFmt; // ���ظ�ʽ
    std::string filterDesc; // �˾������ַ���

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
        // bufferSrcVtx��bufferSinkCtx�ᱻgraphһ���ͷ�
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
    return false; // ���ڱ������߳�ʹ��
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
    // �����µ��˾�ͼ
    FilterGraphPtr createFilterGraph(
        AVFrame const *frame,
        std::string const &filterDesc)
    {
        // �����˾�ͼ
        auto filterGraph = avfilter_graph_alloc();
        if (!filterGraph)
        {
            // std::cerr << "�޷������˾�ͼ" << std::endl;
            return nullptr;
        }

        AVFilterContext *bufferSrcCtx = nullptr;
        AVFilterContext *bufferSinkCtx = nullptr;

        //----------------------------------------------------

        // ����������Դ
        auto bufferSrc = avfilter_get_by_name("buffer");
        if (!bufferSrc)
        {
            avfilter_graph_free(&filterGraph);
            return nullptr;
        }

        // ���û�����Դ
        char args[512];
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
                 frame->width, frame->height, frame->format);

        if (avfilter_graph_create_filter(
                &bufferSrcCtx, bufferSrc, "in",
                args, nullptr, filterGraph) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "�޷�����������Դ" << std::endl;
            return nullptr;
        }

        //----------------------------------------------------

        // ����������������
        auto bufferSink = avfilter_get_by_name("buffersink");
        if (!bufferSink)
        {
            avfilter_graph_free(&filterGraph);
            return nullptr;
        }

        // ���û�����������
        if (avfilter_graph_create_filter(
                &bufferSinkCtx, bufferSink, "out",
                nullptr, nullptr, filterGraph) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "�޷�����������������" << std::endl;
            return nullptr;
        }

        //----------------------------------------------------

        // �����˾���
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
            // std::cerr << "�޷������˾�ͼ" << std::endl;
            return nullptr;
        }

        // �����˾�ͼ
        if (avfilter_graph_config(filterGraph, nullptr) < 0)
        {
            avfilter_graph_free(&filterGraph);
            // std::cerr << "�޷������˾�ͼ" << std::endl;
            return nullptr;
        }

        return std::make_shared<FilterGraphCacheItem>(filterGraph, bufferSrcCtx, bufferSinkCtx);
    }

    // ���ɻ����
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

    // ���һ���
    if (auto it = mPimpl->mCache.find(key); it != mPimpl->mCache.end())
    {
        // ���Ի�ȡʹ��Ȩ
        if (it->second->acquire())
            return it->second;
        else if (waitIfBusy)
        {
            // ����ʵ��һ���򵥵ĵȴ����� - ָ���˱� + �ȴ�

            int maxRetries = 5;   // ������Դ���
            int baseBelayMs = 10; // 10ms�����ӳ�
            for (int retry = 0; retry < maxRetries; ++retry)
            {
                lock.unlock();
                {
                    // ָ���˱��ӳ�
                    int delayMs = baseBelayMs * (1 << retry); // 10, 20, 40, 80, 160ms
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
                lock.lock();

                // ���¼��
                auto it = mPimpl->mCache.find(key);
                if (it != mPimpl->mCache.end() &&
                    it->second->acquire())
                    return it->second;
            }
        }
        return nullptr;
    }

    // ����δ���У���黺���С
    if (mPimpl->mCache.size() >= mPimpl->mMaxSize)
    {
        cleanupUnused();

        // ����������ģ��Ƴ�һ�����δʹ�õ�
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
            { // �Ƴ����δʹ�õ�
                mPimpl->mCache.erase(oldestIt);
            }
            else
            { // �޿��ÿռ�
                return nullptr;
            }
        }
    }

    // �����µ��˾�ͼ
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

    // �ڲ���ȡ�˾�ͼ ȷ����ʹ������ͷ�
    auto filterItem = getFilterGraph(inputFrame, filterDesc, true);
    if (!filterItem)
    {
        return AVERROR(ENOMEM);
    }

    // ʹ��RAIIȷ���ͷ�
    struct FilterGuard
    {
        std::shared_ptr<FilterGraphCacheItem> item;
        ~FilterGuard()
        {
            if (item)
                item->release();
        }
    } guard{filterItem};

    // ����֡���˾�ͼ
    int ret = av_buffersrc_add_frame_flags(filterItem->mBufferSrcVtx,
                                           inputFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0)
    {
        return ret;
    }

    // �������֡
    *outputFrame = av_frame_alloc();
    if (!*outputFrame)
    {
        return AVERROR(ENOMEM);
    }

    // ���˾�ͼ����֡
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
    { // ����µĴ�СС�ڵ�ǰ�����С���Ƴ��������
        cleanupUnused();

        // �������̫��ǿ���Ƴ�һЩ�������ʹ��ʱ�䣩
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
                return false; // ��ʹ���У��޷�����
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
    std::cout << "=== �˾�ͼ�����״̬ ===" << std::endl;
    std::cout << "  �ܻ���ͼ����" << mPimpl->mCache.size() << std::endl;
    std::cout << "  ��󻺴�����" << mPimpl->mMaxSize << std::endl;
    std::cout << " ����ʱ�ޣ�" << currentTimeout.count() << " ��" << std::endl;

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
                  << " ��ʽ:" << (pixFmtName ? pixFmtName : "unknown")
                  << " ������:" << value->getUseCount()
                  << " ʹ����:" << (value->isInUse() ? "��" : "��")
                  << " �ϴ�ʹ��:" << timeSinceUse.count() << "s ֮ǰ"
                  << std::endl;
    }
    std::cout << "��ǰ����ʹ�ã�" << inUseCount << std::endl;
    std::cout << "ʹ��������" << totalUseCount << std::endl;
    std::cout << "=================================" << std::endl;
}