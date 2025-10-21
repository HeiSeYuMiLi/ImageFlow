#pragma once

#include <string>
#include <vector>

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
    // ProcessConfig mConfig;

public:
    ~ImageFlowProcessor();

public:
    int processImages(
        std::vector<std::string> const &imagePaths,
        std::string const &outputFolder,
        ProcessConfig const &config);

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
