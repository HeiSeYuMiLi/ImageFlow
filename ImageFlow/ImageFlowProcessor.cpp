#include "ImageFlowProcessor.h"
//----------------------------
#include <cerrno>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
//----------------------------
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
//----------------------------
#include "Defer.hpp"
#include "FilterGraphPool.h"
#include "Utils.h"

using namespace ImageFlow;

ImageFlowProcessor::ImageFlowProcessor(ProcessConfig const &config)
    : mConfig(config)
{
    mFilterDesc = toFilterDesc(mConfig);
    if (mFilterDesc.empty())
        throw std::exception("传入的参数无效");
}

ImageFlowProcessor::~ImageFlowProcessor()
{
    mThreadPool.shutdownGraceful();
}

int ImageFlowProcessor::processImage(
    std::string const &inputPath,
    std::string const &outputFolder)
{
    auto inputFrame = decodeImage(inputPath);
    if (!inputFrame)
        return 1001;

    AVFrame *outputFrame = nullptr;
    int ret = mFilterGraphPool.processFrame(inputFrame, mFilterDesc, &outputFrame);
    if (ret >= 0 && outputFrame)
    {
        auto outputPath = geneOutputPath(outputFolder, inputPath, mConfig.outputFmt);
        encodeImage(outputFrame, outputPath, mConfig.outputFmt);
        av_frame_free(&outputFrame);
    }
    av_frame_free(&inputFrame);
    return 0;
}

int ImageFlowProcessor::processImages(
    std::vector<std::string> const &imagePaths,
    std::string const &outputFolder)
{
    for (auto &&imagePath : imagePaths)
    {
        mThreadPool.submit([this, imagePath, outputFolder]()
                           { this->processImage(imagePath, outputFolder); });
    }
    mThreadPool.waitAll();
    mFilterGraphPool.printCacheStatus();
    return 0;
}

//---------------------------------------------------------------------

AVFrame *ImageFlowProcessor::decodeImage(std::string const &inputPath)
{
    AVFormatContext *formatCtx = nullptr;
    AVCodecContext *codecCtx = nullptr;
    DEFER({
        if (formatCtx)
            avformat_close_input(&formatCtx);
        if (codecCtx)
            avcodec_free_context(&codecCtx);
    });

    // 打开输入文件
    auto utf8Filename = Utils::localToUtf8(inputPath);
    if (avformat_open_input(&formatCtx, utf8Filename.c_str(), nullptr, nullptr) < 0)
    {
        std::cerr << "无法打开输入文件：" << inputPath << std::endl;
        return nullptr;
    }

    // 查找流信息
    if (avformat_find_stream_info(formatCtx, nullptr) < 0)
    {
        std::cerr << "找不到流信息" << std::endl;
        return nullptr;
    }

    // 查找视频流  图片也按视频流处理  图片只有一帧
    int videoStreamIdx = -1;
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++)
    {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIdx == -1)
    {
        std::cerr << "找不到视频流" << std::endl;
        return nullptr;
    }

    // 获取解码器
    AVCodecParameters *codecpar = formatCtx->streams[videoStreamIdx]->codecpar;
    auto codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        std::cerr << "不支持的编解码器" << std::endl;
        return nullptr;
    }

    // 创建解码器上下文
    if (!(codecCtx = avcodec_alloc_context3(codec)))
    {
        std::cerr << "无法分配解码器上下文" << std::endl;
        return nullptr;
    }
    if (avcodec_parameters_to_context(codecCtx, codecpar) < 0)
    {
        std::cerr << "无法拷贝编解码器参数" << std::endl;
        return nullptr;
    }
    // 打开解码器
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        std::cerr << "无法打开编解码器" << std::endl;
        return nullptr;
    }

    // 分配帧
    auto frame = av_frame_alloc();
    if (!frame)
    {
        std::cerr << "无法分配 AVFrame" << std::endl;
        return nullptr;
    }

    AVPacket packet;
    AVFrame *cloned = nullptr;
    while (av_read_frame(formatCtx, &packet) >= 0)
    {
        // 只处理视频包
        if (packet.stream_index == videoStreamIdx)
        {
            int ret = avcodec_send_packet(codecCtx, &packet);
            if (ret < 0)
            {
                av_packet_unref(&packet);
                continue;
            }

            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == 0)
            {
                // 克隆一份帧数据，解除与 codecCtx 的生命周期耦合
                cloned = av_frame_clone(frame);
                av_frame_unref(frame);
                av_packet_unref(&packet);
                break;
            }
        }
        av_packet_unref(&packet);
    }

    // 清理本地资源
    av_frame_free(&frame);

    return cloned;
}

bool ImageFlowProcessor::encodeImage(
    AVFrame *frame,
    std::string const &outputPath,
    std::string const &format)
{
    // 根据格式确定输出编码器
    const char *codecName = "png"; // 默认PNG
    if (format == "jpg" || format == "jpeg")
        codecName = "mjpeg";
    else if (format == "bmp")
        codecName = "bmp";
    else if (format == "webp")
        codecName = "libwebp";

    auto outputCodec = avcodec_find_encoder_by_name(codecName);
    if (!outputCodec)
    {
        std::cerr << "未找到编解码器：" << codecName << std::endl;
        return false;
    }

    AVCodecContext *outputCodecCtx = avcodec_alloc_context3(outputCodec);
    if (!outputCodecCtx)
    {
        std::cerr << "无法分配视频编解码器上下文" << std::endl;
        return false;
    }

    // 配置编码器参数
    outputCodecCtx->width = frame->width;
    outputCodecCtx->height = frame->height;
    outputCodecCtx->time_base = {1, 25};

    // 根据编码器类型设置合适的像素格式
    if (strcmp(codecName, "mjpeg") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P; // JPEG 常用格式
        // 设置 JPEG 质量参数
        outputCodecCtx->qmin = 2;  // 最好质量
        outputCodecCtx->qmax = 31; // 最差质量
        av_opt_set_int(outputCodecCtx->priv_data, "qscale", 2, 0);
    }
    else if (strcmp(codecName, "libwebp") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        // 设置 WebP 质量
        av_opt_set_int(outputCodecCtx->priv_data, "quality", 90, 0);
    }
    else if (strcmp(codecName, "png") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_RGBA; // PNG 支持透明通道
        // 设置 PNG 压缩级别
        outputCodecCtx->compression_level = 6;
    }
    else if (strcmp(codecName, "bmp") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_BGR24; // BMP 常用格式
    }
    else
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // 默认格式
    }

    // 设置通用编码参数
    outputCodecCtx->flags |= AV_CODEC_FLAG_QSCALE;

    // 打开编码器
    if (avcodec_open2(outputCodecCtx, outputCodec, nullptr) < 0)
    {
        std::cerr << "无法打开输出编解码器" << std::endl;
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    // 打开输出文件
    std::filesystem::path outPath{std::string(outputPath)};
    FILE *outputFile = nullptr;
#if defined(_WIN32)
    // 使用宽字符路径打开文件，支持 Unicode 路径
    std::wstring wpath = outPath.wstring();
    if (_wfopen_s(&outputFile, wpath.c_str(), L"wb") != 0)
        outputFile = nullptr;
#else
    std::string outPathStr = outPath.string();
    outputFile = fopen(outPathStr.c_str(), "wb");
#endif

    if (!outputFile)
    {
        std::cerr << "无法打开输出文件：" << outPath << std::endl;
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    // 像素格式转换
    SwsContext *conversionCtx = nullptr;
    AVFrame *convertedFrame = nullptr;

    if (frame->format != outputCodecCtx->pix_fmt)
    {
        conversionCtx = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,
            outputCodecCtx->width, outputCodecCtx->height, outputCodecCtx->pix_fmt,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!conversionCtx)
        {
            std::cerr << "无法创建转换上下文" << std::endl;
            fclose(outputFile);
            avcodec_free_context(&outputCodecCtx);
            return false;
        }

        // 创建转换后的帧
        convertedFrame = av_frame_alloc();
        convertedFrame->format = outputCodecCtx->pix_fmt;
        convertedFrame->width = outputCodecCtx->width;
        convertedFrame->height = outputCodecCtx->height;

        if (av_frame_get_buffer(convertedFrame, 0) < 0)
        {
            std::cerr << "无法分配转换后的帧" << std::endl;
            sws_freeContext(conversionCtx);
            fclose(outputFile);
            avcodec_free_context(&outputCodecCtx);
            av_frame_free(&convertedFrame);
            return false;
        }

        // 执行格式转换
        sws_scale(conversionCtx,
                  frame->data, frame->linesize, 0, frame->height,
                  convertedFrame->data, convertedFrame->linesize);
    }

    // 发送帧到编码器
    AVFrame *frameToEncode = convertedFrame ? convertedFrame : frame;
    int ret = avcodec_send_frame(outputCodecCtx, frameToEncode);
    if (ret < 0)
    {
        std::cerr << "向编码器发送帧时出错" << std::endl;
        if (conversionCtx)
            sws_freeContext(conversionCtx);
        if (convertedFrame)
            av_frame_free(&convertedFrame);
        fclose(outputFile);
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    // 接收编码后的包
    AVPacket *pkt = av_packet_alloc();

    // 在发送真实帧并读完后，flush 编码器以确保所有包都写出
    // pkt 已在函数中 av_packet_alloc()
    int recvRet = 0;
    while (true)
    {
        recvRet = avcodec_receive_packet(outputCodecCtx, pkt);
        if (recvRet == AVERROR(EAGAIN))
            break;
        else if (recvRet < 0)
            break;

        fwrite(pkt->data, 1, pkt->size, outputFile);
        av_packet_unref(pkt);
    }

    // 发送空帧以触发 flush
    avcodec_send_frame(outputCodecCtx, nullptr);
    while (true)
    {
        recvRet = avcodec_receive_packet(outputCodecCtx, pkt);
        if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF)
            break;
        else if (recvRet < 0)
            break;

        fwrite(pkt->data, 1, pkt->size, outputFile);
        av_packet_unref(pkt);
    }

    // 清理资源
    if (conversionCtx)
        sws_freeContext(conversionCtx);
    if (convertedFrame)
        av_frame_free(&convertedFrame);
    fclose(outputFile);
    avcodec_free_context(&outputCodecCtx);
    av_packet_free(&pkt);

    return true;
}

std::string ImageFlowProcessor::toFilterDesc(ProcessConfig const &config)
{
    std::string desc{config.filterDesc};
    std::string sizeStr;
    if (config.targetWidth > 0 && config.targetHeight > 0)
    {
        sizeStr = "scale=" + std::to_string(config.targetWidth) + ":" + std::to_string(config.targetHeight);
    }

    if (!desc.empty() && !sizeStr.empty())
        desc = sizeStr + "," + desc;
    else if (!sizeStr.empty())
        desc = sizeStr;
    return desc;
}

std::string ImageFlowProcessor::geneOutputPath(
    std::string const &outputFolder,
    std::string const &inputPath,
    std::string const &format)
{
    namespace fs = std::filesystem;

    fs::path inputFile{inputPath};
    std::string stem{inputFile.stem().string()};

    fs::path outputPath{outputFolder};
    outputPath /= stem + "." + format;
    return outputPath.string();
}
