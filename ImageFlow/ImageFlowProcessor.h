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

class ImageFlowProcessor
{
private:
    AVFrame *mFrame = nullptr;                  // ԭʼ֡
    AVFrame *mFilteredFrame = nullptr;          // ������֡
    AVFormatContext *mInputFormatCtx = nullptr; // �����ļ�������
    AVCodecContext *mCodecCtx = nullptr;        // ������������
    SwsContext *mSwsCtx = nullptr;              // ͼ������������

    // �˾����
    AVFilterGraph *mFilterGraph = nullptr;     // �˾�ͼ
    AVFilterContext *mBuffersrcCtx = nullptr;  // ������Դ
    AVFilterContext *mBuffersinkCtx = nullptr; // ������������

public:
    ~ImageFlowProcessor();

public:
    bool processImage(
        std::string const &inputPath,
        std::string const &outputPath,
        std::string const &filterDesc,
        int newWidth, int newHeight,
        std::string const &outputFormat);

private:
    bool openInputFile(std::string const &filename);

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
