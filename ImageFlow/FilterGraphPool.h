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

/* �˾�ͼ���� */
class FilterGraphCacheItem
{
    friend class FilterGraphPool;

public:
    AVFilterGraph *graph;
    AVFilterContext *bufferSrcVtx;
    AVFilterContext *bufferSinkCtx;

private:
    int refCount = 1; // ���ü�������ʱ����

public:
    FilterGraphCacheItem(
        AVFilterGraph *g,
        AVFilterContext *src,
        AVFilterContext *sink);

    ~FilterGraphCacheItem();
};

using FilterGraph = FilterGraphCacheItem;

/* �˾�ͼ�� */
class FilterGraphPool
{
public:
    using FilterGraphPtr = std::shared_ptr<FilterGraphCacheItem>;

public:
    FilterGraphPool(size_t maxSize = 100);

    // ���ÿ���
    FilterGraphPool(FilterGraphPool const &) = delete;
    FilterGraphPool &operator=(FilterGraphPool const &) = delete;

public:
    // ��ȡ�˾�ͼ
    FilterGraphPtr getFilterGraph(
        int width, int height,
        AVPixelFormat pixelFormat,
        std::string const &filterDesc);

    // ����֡  �򵥴���
    int processFrame(
        FilterGraphPtr filterItem,
        AVFrame *inputFrame,
        AVFrame **outputFrame);

    // ������
    void clear();

    // ��ȡ����ͳ����Ϣ
    size_t getCacheSize() const;
    size_t getMaxSize() const;
    void setMaxSize(size_t maxSize);

    // ��ӡ����״̬�������ã�
    void printCacheStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mPimpl;
};

} // namespace ImageFlow
