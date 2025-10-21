#include "ImageFlowProcessor.h"
//----------------------------
#include <filesystem>
#include <iostream>
//----------------------------
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
//----------------------------
#include "FilterGraphPool.h"
#include "Utils.h"

using namespace ImageFlow;

ImageFlowProcessor::~ImageFlowProcessor()
{
    // cleanup();
}

int ImageFlowProcessor::processImages(
    std::vector<std::string> const &imagePaths,
    std::string const &outputFolder,
    ProcessConfig const &config)
{
    FilterGraphPool pool(20, std::chrono::minutes(10));

    auto filterDesc = toFilterDesc(config);
    for (const auto &imagePath : imagePaths)
    {
        AVFrame *inputFrame = decodeImage(imagePath);
        if (!inputFrame)
            continue;

        AVFrame *outputFrame = nullptr;
        int ret = pool.processFrame(inputFrame, filterDesc, &outputFrame);
        if (ret >= 0 && outputFrame)
        {
            auto outputPath = geneOutputPath(outputFolder, imagePath, config.outputFmt);
            encodeImage(outputFrame, outputPath, config.outputFmt);
            av_frame_free(&outputFrame);
        }
        av_frame_free(&inputFrame);
    }
    pool.printCacheStatus();
    return 0;
}

//---------------------------------------------------------------------

AVFrame *ImageFlowProcessor::decodeImage(std::string const &inputPath)
{
    AVFormatContext *formatCtx = nullptr;

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
            videoStreamIdx = i;
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
    auto codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecpar);
    // 打开解码器
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        std::cerr << "无法打开编解码器" << std::endl;
        return nullptr;
    }

    // 分配帧
    auto frame = av_frame_alloc();

    AVPacket packet;
    while (av_read_frame(formatCtx, &packet) >= 0)
    {
        // 只处理视频包
        if (packet.stream_index == 0)
        { // 假设第一个流是视频流
            int ret = avcodec_send_packet(codecCtx, &packet);
            if (ret < 0)
            {
                av_packet_unref(&packet);
                continue;
            }

            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == 0)
            {
                av_packet_unref(&packet);
                return frame; // 成功读取一帧
            }
        }
        av_packet_unref(&packet);
    }
    return nullptr;
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

    FILE *outputFile = nullptr;
    fopen_s(&outputFile, outputPath.c_str(), "wb");
    // 创建输出文件
    if (!outputFile)
    {
        std::cerr << "无法打开输出文件：" << outputPath << std::endl;
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

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(outputCodecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            std::cerr << "编码时出错" << std::endl;
            break;
        }

        // 写入文件
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

    fs::path inputFile(inputPath);
    std::string stem = inputFile.stem().string();

    fs::path outputPath(outputFolder);
    outputPath /= stem + "." + format;
    return outputPath.string();
}
