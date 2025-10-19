#include "ImageFlowProcessor.h"
//----------------------------
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
    cleanup();
}

int ImageFlowProcessor::processImage(
    std::string const &inputPath,
    std::string const &outputPath,
    ProcessConfig const &config)
{
    // 打开输入文件并解码
    if (openInputFile(inputPath))
    {
        std::cerr << "打开输入文件失败" << std::endl;
        return 1001;
    }

    // 读取第一帧
    if (!readFrame())
    {
        std::cerr << "读取帧失败" << std::endl;
        return 1002;
    }

    FilterGraphPool pool(20, std::chrono::minutes(10));
    int ret = pool.processFrame(
        mFrame,
        "hue=h=30:s=1,scale=800:600",
        &mFilteredFrame);

    // 编码并保存输出文件
    if (!encodeAndSave(outputPath, config.outputFmt, config.targetWidth, config.targetHeight))
    {
        std::cerr << "编码和保存失败" << std::endl;
        return 1005;
    }

    std::cout << "已成功处理图像：" << inputPath
              << " -> " << outputPath << std::endl;
    return 0;
}

//---------------------------------------------------------------------

int ImageFlowProcessor::openInputFile(std::string const &filename)
{
    // 打开输入文件
    auto utf8Filename = Utils::localToUtf8(filename);
    if (avformat_open_input(&mInputFormatCtx, utf8Filename.c_str(), nullptr, nullptr) < 0)
    {
        std::cerr << "无法打开输入文件：" << filename << std::endl;
        return 1001;
    }

    // 查找流信息
    if (avformat_find_stream_info(mInputFormatCtx, nullptr) < 0)
    {
        std::cerr << "找不到流信息" << std::endl;
        return 1002;
    }

    // 查找视频流  图片也按视频流处理  图片只有一帧
    int videoStreamIdx = -1;
    for (unsigned int i = 0; i < mInputFormatCtx->nb_streams; i++)
    {
        if (mInputFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStreamIdx = i;
            break;
        }
    }

    if (videoStreamIdx == -1)
    {
        std::cerr << "找不到视频流" << std::endl;
        return 1003;
    }

    // 获取解码器
    AVCodecParameters *codecpar = mInputFormatCtx->streams[videoStreamIdx]->codecpar;
    auto codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        std::cerr << "不支持的编解码器" << std::endl;
        return 1004;
    }

    // 创建解码器上下文
    mCodecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(mCodecCtx, codecpar);

    // 打开解码器
    if (avcodec_open2(mCodecCtx, codec, nullptr) < 0)
    {
        std::cerr << "无法打开编解码器" << std::endl;
        return 1005;
    }

    // 分配帧
    mFrame = av_frame_alloc();
    mFilteredFrame = av_frame_alloc();

    return 0;
}

bool ImageFlowProcessor::readFrame()
{
    AVPacket packet;

    while (av_read_frame(mInputFormatCtx, &packet) >= 0)
    {
        // 只处理视频包
        if (packet.stream_index == 0)
        { // 假设第一个流是视频流
            int ret = avcodec_send_packet(mCodecCtx, &packet);
            if (ret < 0)
            {
                av_packet_unref(&packet);
                continue;
            }

            ret = avcodec_receive_frame(mCodecCtx, mFrame);
            if (ret == 0)
            {
                av_packet_unref(&packet);
                return true; // 成功读取一帧
            }
        }
        av_packet_unref(&packet);
    }

    return false;
}

bool ImageFlowProcessor::initFilters(std::string const &filterDesc, int newWidth, int newHeight)
{
    char args[512];
    int ret;

    // 创建滤镜图
    mFilterGraph = avfilter_graph_alloc();
    if (!mFilterGraph)
    {
        std::cerr << "无法创建滤镜图" << std::endl;
        return false;
    }

    // 创建缓冲区源
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    // 配置缓冲区源
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
             mFrame->width, mFrame->height, mFrame->format);

    ret = avfilter_graph_create_filter(&mBuffersrcCtx, buffersrc, "in",
                                       args, nullptr, mFilterGraph);
    if (ret < 0)
    {
        std::cerr << "无法创建缓冲区源" << std::endl;
        return false;
    }

    // 配置缓冲区接收器
    ret = avfilter_graph_create_filter(&mBuffersinkCtx, buffersink, "out",
                                       nullptr, nullptr, mFilterGraph);
    if (ret < 0)
    {
        std::cerr << "无法创建缓冲区接收器" << std::endl;
        return false;
    }

    // 解析滤镜描述
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();

    outputs->name = av_strdup("in");
    outputs->filter_ctx = mBuffersrcCtx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = mBuffersinkCtx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    // 构建默认滤镜链
    std::string final_filter_desc = filterDesc.empty() ? "scale=" + std::to_string(newWidth) + ":" + std::to_string(newHeight) : filterDesc + ",scale=" + std::to_string(newWidth) + ":" + std::to_string(newHeight);

    ret = avfilter_graph_parse_ptr(mFilterGraph, final_filter_desc.c_str(),
                                   &inputs, &outputs, nullptr);
    if (ret < 0)
    {
        std::cerr << "无法解析滤镜图" << std::endl;
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        return false;
    }

    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    // 配置滤镜图
    ret = avfilter_graph_config(mFilterGraph, nullptr);
    if (ret < 0)
    {
        std::cerr << "无法配置滤镜图" << std::endl;
        return false;
    }
    return true;
}

bool ImageFlowProcessor::applyFilters()
{
    // 将帧发送到滤镜图
    int ret = av_buffersrc_add_frame(mBuffersrcCtx, mFrame);
    if (ret < 0)
    {
        std::cerr << "输入滤镜图时出错" << std::endl;
        return false;
    }

    // 从滤镜图接收处理后的帧
    ret = av_buffersink_get_frame(mBuffersinkCtx, mFilteredFrame);
    if (ret < 0)
    {
        std::cerr << "获取筛选帧时出错" << std::endl;
        return false;
    }

    return true;
}

bool ImageFlowProcessor::encodeAndSave(
    std::string const &outputPath,
    std::string const &format,
    int width, int height)
{
    // 根据格式确定输出编码器
    const char *codecName = "png"; // 默认PNG
    if (format == "jpg" || format == "jpeg")
    {
        codecName = "mjpeg";
    }
    else if (format == "bmp")
    {
        codecName = "bmp";
    }
    else if (format == "webp")
    {
        codecName = "libwebp";
    }

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
    outputCodecCtx->width = width;
    outputCodecCtx->height = height;
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

    if (mFilteredFrame->format != outputCodecCtx->pix_fmt)
    {
        conversionCtx = sws_getContext(
            mFilteredFrame->width, mFilteredFrame->height, (AVPixelFormat)mFilteredFrame->format,
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
                  mFilteredFrame->data, mFilteredFrame->linesize, 0, mFilteredFrame->height,
                  convertedFrame->data, convertedFrame->linesize);
    }

    // 发送帧到编码器
    AVFrame *frameToEncode = convertedFrame ? convertedFrame : mFilteredFrame;
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

void ImageFlowProcessor::cleanup()
{
    if (mFilterGraph)
        avfilter_graph_free(&mFilterGraph);
    if (mFrame)
        av_frame_free(&mFrame);
    if (mFilteredFrame)
        av_frame_free(&mFilteredFrame);
    if (mCodecCtx)
        avcodec_free_context(&mCodecCtx);
    if (mInputFormatCtx)
        avformat_close_input(&mInputFormatCtx);
    if (mSwsCtx)
        sws_freeContext(mSwsCtx);
}
