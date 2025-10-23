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

/* �˾�ͼ���� */
class FilterGraphCacheItem
{
    friend class FilterGraphPool;

private:
    AVFilterGraph *mGraph;                           // �˾�ͼ
    AVFilterContext *mBufferSrcVtx;                  // ���뻺�����˾�
    AVFilterContext *mBufferSinkCtx;                 // ����������˾�
    std::atomic<int> mUseCount = 1;                  // ���ü���
    std::atomic<bool> mInUse = false;                // �Ƿ�����ʹ��
    std::chrono::steady_clock::time_point mLastUsed; // �ϴ�ʹ��ʱ��

public:
    FilterGraphCacheItem(
        AVFilterGraph *g,
        AVFilterContext *src,
        AVFilterContext *sink);

    ~FilterGraphCacheItem();

private:
    // ���Ի�ȡʹ��Ȩ
    bool acquire();

    // �ͷ�ʹ��Ȩ
    void release();

    // ����Ƿ������
    bool canCleanup(std::chrono::seconds timeout = std::chrono::seconds(300)) const;

public:
    // ��ȡʹ��ͳ��
    bool isInUse() const;
    int getUseCount() const;
    auto getLastUsed() const;

    // ��ȡ�˾�ͼ���
    AVFilterGraph *getGraph() const;
    AVFilterContext *getBufferSrc() const;
    AVFilterContext *getBufferSink() const;
};

using FilterGraph = FilterGraphCacheItem;

/* �˾�ͼ�� */
class FilterGraphPool
{
public:
    using FilterGraphPtr = std::shared_ptr<FilterGraphCacheItem>;

public:
    FilterGraphPool(size_t maxSize = 100, std::chrono::seconds cleanupTimeout = std::chrono::seconds(300));
    ~FilterGraphPool();

    // ���ÿ���
    FilterGraphPool(FilterGraphPool const &) = delete;
    FilterGraphPool &operator=(FilterGraphPool const &) = delete;

public:
    // ��ȡ�˾�ͼ  ������ڱ�ʹ�û�ȴ��򷵻�nullptr
    FilterGraphPtr getFilterGraph(
        AVFrame const *inputFrame,
        std::string const &filterDesc,
        bool waitIfBusy = false);

    // ����֡  �򵥴���
    int processFrame(
        AVFrame *inputFrame,
        std::string const &filterDesc,
        AVFrame **outputFrame);

    // ����ʱ��δʹ�õ��˾�ͼ
    size_t cleanupUnused();

    // ǿ���������л���
    void clear();

    // ��ȡ����ͳ����Ϣ
    size_t getCacheSize() const;
    size_t getMaxSize() const;
    bool setMaxSize(size_t maxSize);

    void setCleanupTimeout(std::chrono::seconds timeout);
    std::chrono::seconds getCleanupTimeout() const;

    // ��ӡ����״̬�������ã�
    void printCacheStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mPimpl;
};

} // namespace ImageFlow
