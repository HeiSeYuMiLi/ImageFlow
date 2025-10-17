#pragma once

#include <string>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace ImageFlow
{

// 处理配置
struct ProcessConfig
{
    int targetWidth = 0;
    int targetHeight = 0;
    std::string filterDesc;
    std::string outputFmt;
};

class ImageFlowProcessor
{
private:
    AVFrame *mFrame = nullptr;                  // 原始帧
    AVFrame *mFilteredFrame = nullptr;          // 处理后的帧
    AVFormatContext *mInputFormatCtx = nullptr; // 输入文件上下文
    AVCodecContext *mCodecCtx = nullptr;        // 解码器上下文
    SwsContext *mSwsCtx = nullptr;              // 图像缩放上下文

    // 滤镜相关
    AVFilterGraph *mFilterGraph = nullptr;     // 滤镜图
    AVFilterContext *mBuffersrcCtx = nullptr;  // 缓冲区源
    AVFilterContext *mBuffersinkCtx = nullptr; // 缓冲区接收器

public:
    ~ImageFlowProcessor();

public:
    int processImage(
        std::string const &inputPath,
        std::string const &outputPath,
        ProcessConfig const &config);

private:
    int openInputFile(std::string const &filename);

    bool readFrame();

    bool initFilters(std::string const &filterDesc, int newWidth, int newHeight);

    bool applyFilters();

    bool encodeAndSave(
        std::string const &outputPath,
        std::string const &format,
        int width, int height);

    void cleanup();
};

} // namespace ImageFlow
