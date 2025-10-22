#pragma once

#include <string>
#include <string_view>
#include <vector>
//--------------------------
extern "C"
{
#include <libavutil/frame.h>
}
//--------------------------
#include "FilterGraphPool.h"
#include "ThreadPool.hpp"

namespace ImageFlow
{

// ¥¶¿Ì≈‰÷√
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
    ProcessConfig mConfig;
    FilterGraphPool mFilterGraphPool;
    ThreadPool mThreadPool;
    std::string mFilterDesc;

public:
    ImageFlowProcessor(ProcessConfig const &config);

    ~ImageFlowProcessor();

public:
    int processImage(
        std::string_view inputPath,
        std::string_view outputPath);

    int processImages(
        std::vector<std::string> const &imagePaths,
        std::string_view outputFolder);

private:
    AVFrame *decodeImage(std::string_view inputPath);

    bool encodeImage(
        AVFrame *frame,
        std::string_view outputPath,
        std::string_view format);

    std::string toFilterDesc(ProcessConfig const &config);

    std::string geneOutputPath(
        std::string_view outputFolder,
        std::string_view inputPath,
        std::string_view format);
};

} // namespace ImageFlow
