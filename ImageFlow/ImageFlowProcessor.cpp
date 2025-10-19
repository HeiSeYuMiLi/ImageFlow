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
    // �������ļ�������
    if (openInputFile(inputPath))
    {
        std::cerr << "�������ļ�ʧ��" << std::endl;
        return 1001;
    }

    // ��ȡ��һ֡
    if (!readFrame())
    {
        std::cerr << "��ȡ֡ʧ��" << std::endl;
        return 1002;
    }

    FilterGraphPool pool(20, std::chrono::minutes(10));
    int ret = pool.processFrame(
        mFrame,
        "hue=h=30:s=1,scale=800:600",
        &mFilteredFrame);

    // ���벢��������ļ�
    if (!encodeAndSave(outputPath, config.outputFmt, config.targetWidth, config.targetHeight))
    {
        std::cerr << "����ͱ���ʧ��" << std::endl;
        return 1005;
    }

    std::cout << "�ѳɹ�����ͼ��" << inputPath
              << " -> " << outputPath << std::endl;
    return 0;
}

//---------------------------------------------------------------------

int ImageFlowProcessor::openInputFile(std::string const &filename)
{
    // �������ļ�
    auto utf8Filename = Utils::localToUtf8(filename);
    if (avformat_open_input(&mInputFormatCtx, utf8Filename.c_str(), nullptr, nullptr) < 0)
    {
        std::cerr << "�޷��������ļ���" << filename << std::endl;
        return 1001;
    }

    // ��������Ϣ
    if (avformat_find_stream_info(mInputFormatCtx, nullptr) < 0)
    {
        std::cerr << "�Ҳ�������Ϣ" << std::endl;
        return 1002;
    }

    // ������Ƶ��  ͼƬҲ����Ƶ������  ͼƬֻ��һ֡
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
        std::cerr << "�Ҳ�����Ƶ��" << std::endl;
        return 1003;
    }

    // ��ȡ������
    AVCodecParameters *codecpar = mInputFormatCtx->streams[videoStreamIdx]->codecpar;
    auto codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
        std::cerr << "��֧�ֵı������" << std::endl;
        return 1004;
    }

    // ����������������
    mCodecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(mCodecCtx, codecpar);

    // �򿪽�����
    if (avcodec_open2(mCodecCtx, codec, nullptr) < 0)
    {
        std::cerr << "�޷��򿪱������" << std::endl;
        return 1005;
    }

    // ����֡
    mFrame = av_frame_alloc();
    mFilteredFrame = av_frame_alloc();

    return 0;
}

bool ImageFlowProcessor::readFrame()
{
    AVPacket packet;

    while (av_read_frame(mInputFormatCtx, &packet) >= 0)
    {
        // ֻ������Ƶ��
        if (packet.stream_index == 0)
        { // �����һ��������Ƶ��
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
                return true; // �ɹ���ȡһ֡
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

    // �����˾�ͼ
    mFilterGraph = avfilter_graph_alloc();
    if (!mFilterGraph)
    {
        std::cerr << "�޷������˾�ͼ" << std::endl;
        return false;
    }

    // ����������Դ
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    // ���û�����Դ
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
             mFrame->width, mFrame->height, mFrame->format);

    ret = avfilter_graph_create_filter(&mBuffersrcCtx, buffersrc, "in",
                                       args, nullptr, mFilterGraph);
    if (ret < 0)
    {
        std::cerr << "�޷�����������Դ" << std::endl;
        return false;
    }

    // ���û�����������
    ret = avfilter_graph_create_filter(&mBuffersinkCtx, buffersink, "out",
                                       nullptr, nullptr, mFilterGraph);
    if (ret < 0)
    {
        std::cerr << "�޷�����������������" << std::endl;
        return false;
    }

    // �����˾�����
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

    // ����Ĭ���˾���
    std::string final_filter_desc = filterDesc.empty() ? "scale=" + std::to_string(newWidth) + ":" + std::to_string(newHeight) : filterDesc + ",scale=" + std::to_string(newWidth) + ":" + std::to_string(newHeight);

    ret = avfilter_graph_parse_ptr(mFilterGraph, final_filter_desc.c_str(),
                                   &inputs, &outputs, nullptr);
    if (ret < 0)
    {
        std::cerr << "�޷������˾�ͼ" << std::endl;
        avfilter_inout_free(&outputs);
        avfilter_inout_free(&inputs);
        return false;
    }

    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    // �����˾�ͼ
    ret = avfilter_graph_config(mFilterGraph, nullptr);
    if (ret < 0)
    {
        std::cerr << "�޷������˾�ͼ" << std::endl;
        return false;
    }
    return true;
}

bool ImageFlowProcessor::applyFilters()
{
    // ��֡���͵��˾�ͼ
    int ret = av_buffersrc_add_frame(mBuffersrcCtx, mFrame);
    if (ret < 0)
    {
        std::cerr << "�����˾�ͼʱ����" << std::endl;
        return false;
    }

    // ���˾�ͼ���մ�����֡
    ret = av_buffersink_get_frame(mBuffersinkCtx, mFilteredFrame);
    if (ret < 0)
    {
        std::cerr << "��ȡɸѡ֡ʱ����" << std::endl;
        return false;
    }

    return true;
}

bool ImageFlowProcessor::encodeAndSave(
    std::string const &outputPath,
    std::string const &format,
    int width, int height)
{
    // ���ݸ�ʽȷ�����������
    const char *codecName = "png"; // Ĭ��PNG
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
        std::cerr << "δ�ҵ����������" << codecName << std::endl;
        return false;
    }

    AVCodecContext *outputCodecCtx = avcodec_alloc_context3(outputCodec);
    if (!outputCodecCtx)
    {
        std::cerr << "�޷�������Ƶ�������������" << std::endl;
        return false;
    }

    // ���ñ���������
    outputCodecCtx->width = width;
    outputCodecCtx->height = height;
    outputCodecCtx->time_base = {1, 25};

    // ���ݱ������������ú��ʵ����ظ�ʽ
    if (strcmp(codecName, "mjpeg") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUVJ420P; // JPEG ���ø�ʽ
        // ���� JPEG ��������
        outputCodecCtx->qmin = 2;  // �������
        outputCodecCtx->qmax = 31; // �������
        av_opt_set_int(outputCodecCtx->priv_data, "qscale", 2, 0);
    }
    else if (strcmp(codecName, "libwebp") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        // ���� WebP ����
        av_opt_set_int(outputCodecCtx->priv_data, "quality", 90, 0);
    }
    else if (strcmp(codecName, "png") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_RGBA; // PNG ֧��͸��ͨ��
        // ���� PNG ѹ������
        outputCodecCtx->compression_level = 6;
    }
    else if (strcmp(codecName, "bmp") == 0)
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_BGR24; // BMP ���ø�ʽ
    }
    else
    {
        outputCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P; // Ĭ�ϸ�ʽ
    }

    // ����ͨ�ñ������
    outputCodecCtx->flags |= AV_CODEC_FLAG_QSCALE;

    // �򿪱�����
    if (avcodec_open2(outputCodecCtx, outputCodec, nullptr) < 0)
    {
        std::cerr << "�޷�������������" << std::endl;
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    FILE *outputFile = nullptr;
    fopen_s(&outputFile, outputPath.c_str(), "wb");
    // ��������ļ�
    if (!outputFile)
    {
        std::cerr << "�޷�������ļ���" << outputPath << std::endl;
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    // ���ظ�ʽת��
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
            std::cerr << "�޷�����ת��������" << std::endl;
            fclose(outputFile);
            avcodec_free_context(&outputCodecCtx);
            return false;
        }

        // ����ת�����֡
        convertedFrame = av_frame_alloc();
        convertedFrame->format = outputCodecCtx->pix_fmt;
        convertedFrame->width = outputCodecCtx->width;
        convertedFrame->height = outputCodecCtx->height;

        if (av_frame_get_buffer(convertedFrame, 0) < 0)
        {
            std::cerr << "�޷�����ת�����֡" << std::endl;
            sws_freeContext(conversionCtx);
            fclose(outputFile);
            avcodec_free_context(&outputCodecCtx);
            av_frame_free(&convertedFrame);
            return false;
        }

        // ִ�и�ʽת��
        sws_scale(conversionCtx,
                  mFilteredFrame->data, mFilteredFrame->linesize, 0, mFilteredFrame->height,
                  convertedFrame->data, convertedFrame->linesize);
    }

    // ����֡��������
    AVFrame *frameToEncode = convertedFrame ? convertedFrame : mFilteredFrame;
    int ret = avcodec_send_frame(outputCodecCtx, frameToEncode);
    if (ret < 0)
    {
        std::cerr << "�����������֡ʱ����" << std::endl;
        if (conversionCtx)
            sws_freeContext(conversionCtx);
        if (convertedFrame)
            av_frame_free(&convertedFrame);
        fclose(outputFile);
        avcodec_free_context(&outputCodecCtx);
        return false;
    }

    // ���ձ����İ�
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
            std::cerr << "����ʱ����" << std::endl;
            break;
        }

        // д���ļ�
        fwrite(pkt->data, 1, pkt->size, outputFile);
        av_packet_unref(pkt);
    }

    // ������Դ
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
