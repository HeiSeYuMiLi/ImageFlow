#pragma once

#include <string>
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
        std::string const &inputPath,
        std::string const &outputPath);

    int processImages(
        std::vector<std::string> const &imagePaths,
        std::string const &outputFolder);

private:
    AVFrame *decodeImage(std::string const &inputPath);

    bool encodeImage(
        AVFrame *frame,
        std::string const &outputPath,
        std::string const &format);

    std::string toFilterDesc(ProcessConfig const &config);

    std::string geneOutputPath(
        std::string const &outputFolder,
        std::string const &inputPath,
        std::string const &format);
};

} // namespace ImageFlow
