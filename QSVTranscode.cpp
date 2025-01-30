#include "QSVTranscode.h"
extern "C"
{
    #include <libavutil/hwcontext_qsv.h>
    #include <libavutil/error.h>
    #include <libavfilter/buffersink.h>
    #include <libavutil/opt.h>
    #include <libavutil/time.h>
    #include <libswresample/swresample.h>
}

static AVPixelFormat get_qsv_format(AVCodecContext *avctx, const enum AVPixelFormat *pix_fmts)
{
    while (*pix_fmts != AV_PIX_FMT_NONE)
    {
        if (*pix_fmts == AV_PIX_FMT_QSV)
        {
            AVHWFramesContext  *frames_ctx;
            AVQSVFramesContext *frames_hwctx;
            int ret;
            QSVTranscode* obj = (QSVTranscode*)avctx->opaque;
            /* create a pool of surfaces to be used by the decoder */
            avctx->hw_frames_ctx = av_hwframe_ctx_alloc(obj->QSV_hw_device_ctx);
            if (!avctx->hw_frames_ctx)
                return AV_PIX_FMT_NONE;
            frames_ctx   = (AVHWFramesContext*)avctx->hw_frames_ctx->data;
            frames_hwctx = (AVQSVFramesContext*)frames_ctx->hwctx;

            frames_ctx->format            = AV_PIX_FMT_QSV;
            frames_ctx->sw_format         = avctx->sw_pix_fmt;
            frames_ctx->width             = FFALIGN(avctx->coded_width,  32);
            frames_ctx->height            = FFALIGN(avctx->coded_height, 32);
            frames_ctx->initial_pool_size = 32;

            frames_hwctx->frame_type = MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;

            ret = av_hwframe_ctx_init(avctx->hw_frames_ctx);
            if (ret < 0)
                return AV_PIX_FMT_NONE;

            return AV_PIX_FMT_QSV;
        }

        pix_fmts++;
    }

    fprintf(stderr, "The QSV pixel format not offered in get_format()\n");

    return AV_PIX_FMT_NONE;
}

QSVTranscode::QSVTranscode(char* inputurl,  OutputInfo* outset, AudioEncodeInfo* audioset)
    : QSV_hw_device_ctx(nullptr)
    , filter_graph(nullptr)
    , buffersrc_ctx(nullptr)
    , buffersink_ctx(nullptr)
    , AudioPts(0)
    , Runing(true)
    , InputOpend(false)
    , OutputOpend(false)
    , OutHeadWrited(false)
    , InFmtCtx(nullptr)
    , OutFmtCtx(nullptr)
    , VideoDecoderCtx(nullptr)
    , VideoEncoderCtx(nullptr)
    , AudioDecoderCtx(nullptr)
    , AudioEncoderCtx(nullptr)
    , InAudioStream(nullptr)
    , InVideoStream(nullptr)
    , OutAudioStream(nullptr)
    , OutVideoStream(nullptr)
    , VEncInited(false)
    , VFilterInited(false)
    , VideoEncCodec(nullptr)
    , AudioEncCodec(nullptr)
    , SwrCtx(nullptr)
    , PcmBuffer(nullptr)
{
    PktBuffer = av_fifo_alloc(sizeof(AVPacket**) * 10);
    av_fifo_reset(PktBuffer);

    OutputSet = outset;
    AudioSet = audioset;

    int len = strlen(inputurl);
    InputUrl = (char*)malloc(len + 1);
    memset(InputUrl, 0, len + 1);
    memcpy(InputUrl, inputurl, len);

    ReadThread = new boost::thread(&QSVTranscode::ReadPacketProc, this);
    WriteThread = new boost::thread(&QSVTranscode::WritePacketProc, this);
}

QSVTranscode::~QSVTranscode()
{
    Runing = false;
    ReadThread->join();
    WriteThread->join();
    if(InFmtCtx)
        avformat_close_input(&InFmtCtx);
    if (OutFmtCtx)
    {
        if (OutHeadWrited)
            av_write_trailer(OutFmtCtx);
        avformat_close_input(&OutFmtCtx);
    }
    if (VideoDecoderCtx)
        avcodec_free_context(&VideoDecoderCtx);
    if (VideoEncoderCtx)
        avcodec_free_context(&VideoEncoderCtx);
    if (QSV_hw_device_ctx)
        av_buffer_unref(&QSV_hw_device_ctx);
    if (filter_graph)
        avfilter_graph_free(&filter_graph);
    if (QSV_hw_device_ctx)
        av_buffer_unref(&QSV_hw_device_ctx);
    if (PktBuffer)
        av_fifo_freep(&PktBuffer);
    if (PcmBuffer)
    {
        av_audio_fifo_free(PcmBuffer);
        PcmBuffer = nullptr;
    }
    free(InputUrl);
}

bool QSVTranscode::OpenInput()
{
    int ret;
    AVCodec *decoder = NULL;

    ret = av_hwdevice_ctx_create(&QSV_hw_device_ctx, AV_HWDEVICE_TYPE_QSV, "auto", NULL, 0);
    if (ret < 0)
    {
        printf("Failed to create a qsv device. Error code: %d\n", ret);
        return -1;
    }
    InFmtCtx = avformat_alloc_context();
    if(!InFmtCtx)
    {
        avformat_free_context(InFmtCtx);
        InFmtCtx = nullptr;
        return -2;
    }
    AVDictionary *dco = NULL;
    av_dict_set(&dco, "rtsp_transport", "tcp", 0);
    av_dict_set(&dco, "stimeout", "3000000", 0);
    if ((ret = avformat_open_input(&InFmtCtx, InputUrl, NULL,  &dco)) < 0)
    {
	av_dict_free(&dco);
        printf("Cannot open input file '%s', Error code: %d\n",InputUrl, ret);
        return false;
    }
    av_dict_free(&dco);
    //InFmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    //av_format_inject_global_side_data(InFmtCtx);
    InFmtCtx->max_analyze_duration = 3 * AV_TIME_BASE;
    InFmtCtx->probesize = 1024 * 1024 * 5;
    if ((ret = avformat_find_stream_info(InFmtCtx, NULL)) < 0)
    {
        printf("Cannot find input stream information. Error code: %d\n", ret);
        return ret;
    }

    for (unsigned int i = 0; i < InFmtCtx->nb_streams; i ++)
    {
        if ((InFmtCtx->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264)
            || (InFmtCtx->streams[i]->codecpar->codec_id == AV_CODEC_ID_HEVC)
            || (InFmtCtx->streams[i]->codecpar->codec_id == AV_CODEC_ID_VP8)
            || (InFmtCtx->streams[i]->codecpar->codec_id == AV_CODEC_ID_VP9)
            || (InFmtCtx->streams[i]->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO))
        {
            InVideoStream = InFmtCtx->streams[i];
        }
        if (InFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            InAudioStream = InFmtCtx->streams[i];
        }
    }

    if (!InVideoStream )
    {
        printf("Cannot find a video stream in the input file. \n");
        return false;
    }
    if (!VideoDecoderCtx)
    {
        switch (InVideoStream->codecpar->codec_id)
        {
            case AV_CODEC_ID_H264:
                decoder = avcodec_find_decoder_by_name("h264_qsv");
                break;
            case AV_CODEC_ID_HEVC:
                decoder = avcodec_find_decoder_by_name("hevc_qsv");
                break;
            case AV_CODEC_ID_VP8:
                decoder = avcodec_find_decoder_by_name("vp8_qsv");
                break;
            case AV_CODEC_ID_VP9:
                decoder = avcodec_find_decoder_by_name("vp9_qsv");
                break;
            case AV_CODEC_ID_MPEG2VIDEO:
                decoder = avcodec_find_decoder_by_name("mpeg2_qsv");
                break;
            default:
                break;
        }
        if (!decoder) {
            printf("The QSV decoder is not present in libavcodec\n");
            return -1;
        }

        if (!(VideoDecoderCtx = avcodec_alloc_context3(decoder)))
            return AVERROR(ENOMEM);

        if ((ret = avcodec_parameters_to_context(VideoDecoderCtx, InVideoStream->codecpar)) < 0)
        {
            printf("avcodec_parameters_to_context error. Error code: %d\n", ret);
            return false;
        }

        VideoDecoderCtx->hw_device_ctx = av_buffer_ref(QSV_hw_device_ctx);
        if (!VideoDecoderCtx->hw_device_ctx)
        {
            printf("A hardware device reference create failed.\n");
            return false;
        }
        VideoDecoderCtx->opaque = this;
        VideoDecoderCtx->get_format    = get_qsv_format;

        if ((ret = avcodec_open2(VideoDecoderCtx, decoder, NULL)) < 0)
        {
            printf("Failed to open codec for decoding. Error code: %d\n", ret);
            return false;
        }
    }
    return true;
}

bool QSVTranscode::OpenOutput()
{
    int ret;
    if (!(VideoEncCodec = avcodec_find_encoder_by_name(OutputSet->VideoEncoderName)))
    {
        printf("Could not find encoder '%s'\n", OutputSet->VideoEncoderName);
        return false;
    }

    if ((ret = (avformat_alloc_output_context2(&OutFmtCtx, NULL, OutputSet->OutputType, OutputSet->OutputUrl))) < 0)
    {
        printf("Failed to deduce output format from file extension. Error code: %d\n", ret);
        return false;
    }

    if (InAudioStream)
    {
        if (((InAudioStream->codecpar->codec_id != AV_CODEC_ID_AAC) && (InAudioStream->codecpar->codec_id != AV_CODEC_ID_AAC_LATM)) || (AudioSet != nullptr))
        {
            if (AudioDecoderCtx)
            {
                avcodec_free_context(&AudioDecoderCtx);
            }
            AVCodec *decoder = avcodec_find_decoder(InAudioStream->codecpar->codec_id);
            if (!decoder)
            {
                InAudioStream = nullptr;
            }
            else
            {
                AudioDecoderCtx = avcodec_alloc_context3(decoder);
                if (!AudioDecoderCtx)
                {
                    InAudioStream = nullptr;
                }
                else
                {
                    if(avcodec_parameters_to_context(AudioDecoderCtx, InAudioStream->codecpar) < 0)
                    {
                        avcodec_free_context(&AudioDecoderCtx);
                        InAudioStream = nullptr;
                    }
                    else
                    {
                        if (avcodec_open2(AudioDecoderCtx, decoder, NULL) < 0)
                        {
                            avcodec_free_context(&AudioDecoderCtx);
                            InAudioStream = nullptr;
                        }
                    }
                }
            }
        }
    }
    if (AudioDecoderCtx)
    {
        AudioEncCodec = avcodec_find_encoder_by_name("libfdk_aac");
        if (!AudioEncCodec)
        {
            avcodec_free_context(&AudioDecoderCtx);
            InAudioStream = nullptr;
        }
        if (!AudioEncoderCtx)
        {
            AudioEncoderCtx = avcodec_alloc_context3(AudioEncCodec);
            if (!AudioEncoderCtx)
            {
                avcodec_free_context(&AudioDecoderCtx);
                InAudioStream = nullptr;
            }
            if(AudioSet)
            {
                AudioEncoderCtx->channels       = av_get_channel_layout_nb_channels(AudioSet->ChannelLayOut);
                AudioEncoderCtx->channel_layout = AudioSet->ChannelLayOut;
                AudioEncoderCtx->sample_rate    = AudioSet->SampleRate;
                AudioEncoderCtx->sample_fmt     = AudioSet->SampleFmt;
                AudioEncoderCtx->bit_rate       = AudioSet->BitRate;
            }
            else
            {
                AudioEncoderCtx->channels       = AudioDecoderCtx->channels;
                AudioEncoderCtx->channel_layout = AudioDecoderCtx->channel_layout;
                AudioEncoderCtx->sample_rate    = AudioDecoderCtx->sample_rate;
                AudioEncoderCtx->sample_fmt     = AudioSet->SampleFmt;
                AudioEncoderCtx->bit_rate       = AudioDecoderCtx->bit_rate;
            }
            AudioEncoderCtx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
            if (OutFmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
                AudioEncoderCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (avcodec_open2(AudioEncoderCtx, AudioEncCodec, NULL) < 0)
            {
                avcodec_free_context(&AudioDecoderCtx);
                avcodec_free_context(&AudioEncoderCtx);
                InAudioStream = nullptr;
            }
        }
        if (AudioEncoderCtx)
        {
            if ((AudioEncoderCtx->channel_layout != AudioDecoderCtx->channel_layout)
                || (AudioEncoderCtx->sample_rate != AudioDecoderCtx->sample_rate)
                || (AudioEncoderCtx->sample_fmt != AudioDecoderCtx->sample_fmt))
            {
                SwrCtx = swr_alloc();
                if (!SwrCtx)
                {
                    avcodec_free_context(&AudioDecoderCtx);
                    InAudioStream = nullptr;
                }
                else
                {
                    av_opt_set_int(SwrCtx, "in_channel_layout",    AudioDecoderCtx->channel_layout, 0);
                    av_opt_set_int(SwrCtx, "in_sample_rate",       AudioDecoderCtx->sample_rate, 0);
                    av_opt_set_sample_fmt(SwrCtx, "in_sample_fmt", AudioDecoderCtx->sample_fmt, 0);

                    av_opt_set_int(SwrCtx, "out_channel_layout",    AudioEncoderCtx->channel_layout, 0);
                    av_opt_set_int(SwrCtx, "out_sample_rate",       AudioEncoderCtx->sample_rate, 0);
                    av_opt_set_sample_fmt(SwrCtx, "out_sample_fmt", AudioEncoderCtx->sample_fmt, 0);
                    if (swr_init(SwrCtx) < 0)
                    {
                        swr_free(&SwrCtx);
                        avcodec_free_context(&AudioDecoderCtx);
                        InAudioStream = nullptr;
                    }
                }
            }
        }

        if ((!PcmBuffer) && (InAudioStream))
        {
            if (AudioSet)
            {
                PcmBuffer = av_audio_fifo_alloc(AudioSet->SampleFmt
                                                , av_get_channel_layout_nb_channels(AudioSet->ChannelLayOut)
                                                , 1);
            }
            else
            {
                PcmBuffer = av_audio_fifo_alloc(AudioDecoderCtx->sample_fmt
                                                , AudioDecoderCtx->channels
                                                , 1);
            }
        }
    }
    else
    {

    }
    return true;
}

void QSVTranscode::init_filters()
{
    char filter_descr[100] = {0};
    sprintf(filter_descr,"scale_qsv=w=%d:h=%d:mode=hq", OutputSet->VideoWidth, OutputSet->VideoHeight);
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = InVideoStream->time_base;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph || !par)
    {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            VideoDecoderCtx->width, VideoDecoderCtx->height, VideoDecoderCtx->pix_fmt,
            time_base.num, time_base.den,
            VideoDecoderCtx->sample_aspect_ratio.num, VideoDecoderCtx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
    if (ret < 0)
    {
        fprintf(stderr, "Cannot create buffer source\n");
        goto end;
    }
    par->hw_frames_ctx = av_buffer_ref(VideoDecoderCtx->hw_frames_ctx);
    ret = av_buffersrc_parameters_set(buffersrc_ctx, par);
    if (ret < 0)
        goto end;
    av_freep(&par);

    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
    if (ret < 0) {
        fprintf(stderr, "Cannot create buffer sink\n");
        goto end;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL)) < 0)
        goto end;

    for (unsigned int i = 0; i < filter_graph->nb_filters; i++)
    {
        filter_graph->filters[i]->hw_device_ctx = av_buffer_ref(VideoDecoderCtx->hw_device_ctx);
        if (!filter_graph->filters[i]->hw_device_ctx)
        {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    VFilterInited = (ret == 0);
    return;

}

void QSVTranscode::WriteOutHead()
{
    int ret;
    if (!OutFmtCtx)
    {
        return;
    }
    if (InAudioStream)
    {
        if (AudioEncoderCtx)
        {
            if (!(OutAudioStream = avformat_new_stream(OutFmtCtx, AudioEncCodec)))
            {
                printf("Failed to allocate audio stream for output format.\n");
                CloseOutput();
                return;
            }
            OutAudioStream->time_base = AudioEncoderCtx->time_base;
            ret = avcodec_parameters_from_context(OutAudioStream->codecpar, AudioEncoderCtx);
            if (ret < 0)
            {
                printf("Failed to copy the stream parameters. Error code: %d\n", ret);
                CloseOutput();
                return;
            }
        }
        else
        {
            if (!(OutAudioStream = avformat_new_stream(OutFmtCtx, nullptr)))
            {
                printf("Failed to allocate audio stream for output format.\n");
                CloseOutput();
                return;
            }
            ret = avcodec_parameters_copy(OutAudioStream->codecpar, InAudioStream->codecpar);
            if (ret < 0)
            {
                printf("Failed to copy audio codec parameters\n");
                CloseOutput();
                return;
            }
            OutAudioStream->codecpar->codec_tag = 0;
            OutAudioStream->time_base = InAudioStream->time_base;
        }
    }

    if (!(OutVideoStream = avformat_new_stream(OutFmtCtx, VideoEncCodec)))
    {
        printf("Failed to allocate video stream for output format.\n");
        CloseOutput();
        return;
    }
    OutVideoStream->time_base = VideoEncoderCtx->time_base;
    ret = avcodec_parameters_from_context(OutVideoStream->codecpar, VideoEncoderCtx);
    if (ret < 0)
    {
        printf("Failed to copy the stream parameters. Error code: %d\n", ret);
        CloseOutput();
        return;
    }

    if (!(OutFmtCtx->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&OutFmtCtx->pb, OutputSet->OutputUrl, AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            printf("Could not open output file '%s'", OutputSet->OutputUrl);
            CloseOutput();
            return;
        }
    }
    if (ret < 0)
    {
        printf( "Cannot open output file. Error code: %d\n", ret);
        CloseOutput();
        return;
    }
    OutFmtCtx->oformat->video_codec = VideoEncoderCtx->codec_id;
    OutFmtCtx->max_interleave_delta = 1000000;
    AVDictionary* opt = nullptr;
    //av_dict_set(&opt, "stimeout", "1000000", 0);
    av_dict_set(&opt, "flvflags", "no_duration_filesize+add_keyframe_index", 0);
    if ((ret = avformat_write_header(OutFmtCtx, &opt)) < 0)
    {
        printf("Error while writing stream header. Error code: %d\n", ret);
        av_dict_free(&opt);
        CloseOutput();
        return;
    }
    av_dict_free(&opt);
    OutHeadWrited = true;
}

void QSVTranscode::CloseOutput()
{
    if (OutFmtCtx)
    {
        AVFormatContext* CloseFmtCtx =  OutFmtCtx;
        OutFmtCtx = nullptr;
        if (OutHeadWrited)
            av_write_trailer(CloseFmtCtx);
        avformat_close_input(&CloseFmtCtx);
    }
    OutAudioStream = nullptr;
    OutVideoStream = nullptr;
    OutputOpend = false;
    OutHeadWrited = false;
}

void QSVTranscode::CloseInPut()
{
    InputOpend = false;
    if (InFmtCtx)
    {
        AVFormatContext* CloseFmtCtx =  InFmtCtx;
        InFmtCtx = nullptr;
        avformat_close_input(&CloseFmtCtx);
    }
    InAudioStream = nullptr;
    InVideoStream = nullptr;
    if (VideoDecoderCtx)
    {
        avcodec_free_context(&VideoDecoderCtx);
        VideoDecoderCtx = nullptr;
    }
    if (AudioDecoderCtx)
    {
        avcodec_free_context(&AudioDecoderCtx);
        AudioDecoderCtx = nullptr;
    }
}

void QSVTranscode::openencoder()
{
    int ret;
    AVFilterContext* filter_ctxn = filter_graph->filters[2];
    if (!VideoEncoderCtx)
    {
        if (!(VideoEncoderCtx = avcodec_alloc_context3(VideoEncCodec)))
        {
            printf( "Cannot open alloc encoder\n");
            return ;
        }
        VideoEncoderCtx->hw_frames_ctx = av_buffer_ref(filter_ctxn->outputs[0]->hw_frames_ctx);
        if (!VideoEncoderCtx->hw_frames_ctx)
        {
            printf( "Failed to create a qsv device\n");
            return;
        }

        int VFrameRate = InVideoStream->avg_frame_rate.num / InVideoStream->avg_frame_rate.den;
        VideoEncoderCtx->time_base = av_make_q(1, VFrameRate);
        VideoEncoderCtx->pix_fmt   = VideoDecoderCtx->pix_fmt;
        VideoEncoderCtx->width     = OutputSet->VideoWidth;
        VideoEncoderCtx->height    = OutputSet->VideoHeight;
        VideoEncoderCtx->profile   = OutputSet->VideoProfile;
        VideoEncoderCtx->level     = 4;
        VideoEncoderCtx->gop_size  = VFrameRate;

        VideoEncoderCtx->bit_rate = OutputSet->VideoBitrate;
        VideoEncoderCtx->keyint_min = VFrameRate;
        VideoEncoderCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_CLOSED_GOP;

        AVDictionary* opt = NULL;
        av_dict_set(&opt, "preset", "veryfast",0);
        av_dict_set(&opt, "tune", "zerolatency", 0);
        if (VideoEncoderCtx->codec_id == AV_CODEC_ID_H264)
        {
            av_dict_set_int(&opt, "idr_interval",0,0);
        }
        if (VideoEncoderCtx->codec_id == AV_CODEC_ID_HEVC)
        {
            av_dict_set_int(&opt, "idr_interval",1,0);
        }
        av_dict_set_int(&opt, "look_ahead",0,0);
        if ((ret = avcodec_open2(VideoEncoderCtx, VideoEncCodec, &opt)) < 0)
        {
            printf("Failed to open encode codec. Error code: %d\n", ret);
            return;
        }
    }
    VEncInited = true;
}

void QSVTranscode::ReadPacketProc()
{
    while(Runing)
    {
        if (!InputOpend)
        {
            InputOpend = OpenInput();
        }
        else
        {
            while(Runing)
            {
                AVPacket* pkt = av_packet_alloc();
                av_init_packet(pkt);
                if (av_read_frame(InFmtCtx, pkt) < 0)
                {
                    av_packet_unref(pkt);
                    break;
                }
                int needTranslate = 0;
                if(InVideoStream){
                     if (pkt->stream_index == InVideoStream->index){
                        needTranslate = 1;
                     }
                      if (pkt->stream_index == InVideoStream->index)
                        pkt->stream_index = 0;
                }

                if(InAudioStream){
                    if((pkt->stream_index == InAudioStream->index)){
                        needTranslate = 1;
                    }
                       if (pkt->stream_index == InAudioStream->index)
                        pkt->stream_index = 1;
                }

                    pkt->dts = pkt->pts;
                    if (av_fifo_space(PktBuffer) < sizeof(AVPacket**))
                    {
                        av_fifo_realloc2(PktBuffer, av_fifo_space(PktBuffer) + av_fifo_size(PktBuffer) + sizeof(AVPacket**) * 10);
                    }
                    av_fifo_generic_write(PktBuffer, &pkt, sizeof(AVPacket**), nullptr);

            }
            CloseInPut();
        }
    }
}

void QSVTranscode::WritePacketProc()
{
    while (Runing)
    {
        if (!OutputOpend)
        {
            if (!InputOpend)
                continue;
            OutputOpend = OpenOutput();
            av_usleep(1000);
        }
        else
        {
            while(Runing)
            {
                if (av_fifo_size(PktBuffer) >= sizeof(AVPacket**))
                {
                    AVPacket* pkt = nullptr;
                    av_fifo_generic_read(PktBuffer, &pkt, sizeof(AVPacket**), nullptr);
                    if (!pkt)
                    {
                        continue;
                    }
                    if (pkt->stream_index == 0)
                    {
                        DecodeVideo(pkt);
                    }
                    if (pkt->stream_index == 1)
                    {
                        DecodeAudio(pkt);
                    }
                    av_packet_unref(pkt);
                }
                else
                {
                    av_usleep(1000);
                }
            }
            CloseOutput();
        }
    }
    while(av_fifo_size(PktBuffer) >= sizeof(AVPacket**))
    {
        AVPacket* pkt = nullptr;
        av_fifo_generic_read(PktBuffer, &pkt, sizeof(AVPacket**), nullptr);
        av_packet_unref(pkt);
    }
}

void QSVTranscode::DecodeVideo(AVPacket* pkt)
{
    AVFrame *frame;
    AVFrame *filt_frame;
    int ret = avcodec_send_packet(VideoDecoderCtx, pkt);
    if (ret < 0)
    {
        printf("Error during decoding. Error code: %d\n", ret);
        return;
    }
    while (ret >= 0)
    {
        if (!(frame = av_frame_alloc()))
            return;
        if (!(filt_frame = av_frame_alloc()))
            return;

        ret = avcodec_receive_frame(VideoDecoderCtx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_frame_free(&frame);
            av_frame_free(&filt_frame);
            return;
        }
        else
        {
            if (ret < 0)
            {
                printf("Error while decoding. Error code: %d\n", ret);
                goto fail;
            }
        }
        if (!VFilterInited)
        {
            init_filters();
            if (!VFilterInited)
               goto fail;
        }
        frame->pts = frame->best_effort_timestamp;
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
        //if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, 0) < 0)
        {
            av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
            break;
        }
        while (1)
        {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                goto fail;
            if (!VEncInited)
            {
                openencoder();
                if (!VEncInited)
                    goto fail;
            }
            if (!OutHeadWrited)
            {
                WriteOutHead();
                if (!OutHeadWrited)
                    goto fail;
            }
            filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
            filt_frame->pts = filt_frame->best_effort_timestamp;
            if ((ret = encode_write(filt_frame)) < 0)
                printf("Error during encoding and writing.\n");
        }
fail:
        av_frame_free(&frame);
        av_frame_free(&filt_frame);
        if (ret < 0)
            return;
    }
    return;
}

void QSVTranscode::DecodeAudio(AVPacket* pkt)
{
    if (OutHeadWrited)
    {
        if(!OutAudioStream)
            return;
        pkt->stream_index = OutAudioStream->index;
        av_packet_rescale_ts(pkt, InAudioStream->time_base, OutAudioStream->time_base);
        pkt->pos = 0;
        pkt->dts = AV_NOPTS_VALUE;
        if (OutHeadWrited && OutFmtCtx)
        {
            if (!AudioDecoderCtx)
            {
                int ret = av_interleaved_write_frame(OutFmtCtx, pkt);
                //int ret = av_write_frame(OutFmtCtx, pkt);
                if (ret  < 0)
                {
                    printf("Error during writing data to output file. Error code: %d\n", ret);
                    if(ret != -22)
                        CloseOutput();
                }
            }
            else
            {
                int ret = avcodec_send_packet(AudioDecoderCtx, pkt);
                if (ret < 0)
                {
                    return;
                }
                AVFrame *frame = av_frame_alloc();
                if (!frame)
                {
                    return;
                }
                while(ret >= 0)
                {
                    ret = avcodec_receive_frame(AudioDecoderCtx, frame);
                    if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF) || (ret < 0))
                    {
                        av_frame_free(&frame);
                        break;
                    }
                    if (!SwrCtx)
                    {
                        if (av_audio_fifo_space(PcmBuffer) < frame->nb_samples)
                        {
                            av_audio_fifo_realloc(PcmBuffer, av_audio_fifo_size(PcmBuffer) + frame->nb_samples);
                        }
                        av_audio_fifo_write(PcmBuffer, (void **)frame->data, frame->nb_samples);
                    }
                    else
                    {
                        uint8_t** pcmdata = nullptr;
                        av_samples_alloc_array_and_samples(&pcmdata
                                                           , NULL
                                                           , av_get_channel_layout_nb_channels(AudioSet->ChannelLayOut)
                                                           , frame->nb_samples
                                                           , AudioEncoderCtx->sample_fmt
                                                           , 1);
                        int convert_size = swr_convert(SwrCtx
                                                       , pcmdata
                                                       , frame->nb_samples
                                                       , (const uint8_t**)frame->extended_data
                                                       , frame->nb_samples);
                        if (av_audio_fifo_space(PcmBuffer) < convert_size)
                        {
                            av_audio_fifo_realloc(PcmBuffer, av_audio_fifo_size(PcmBuffer) + convert_size);
                        }
                        av_audio_fifo_write(PcmBuffer, (void **)pcmdata, convert_size);
                        av_freep(&pcmdata[0]);
                    }
                }

                while(av_audio_fifo_size(PcmBuffer) >= AudioEncoderCtx->frame_size)
                {
                    AVPacket output_packet;
                    av_init_packet(&output_packet);
                    output_packet.data = NULL;
                    output_packet.size = 0;
                    const int frame_size = FFMIN(av_audio_fifo_size(PcmBuffer), AudioEncoderCtx->frame_size);
                    AVFrame *output_frame = av_frame_alloc();
                    output_frame->nb_samples     = frame_size;
                    output_frame->channel_layout = AudioEncoderCtx->channel_layout;
                    output_frame->format         = AudioEncoderCtx->sample_fmt;
                    output_frame->sample_rate    = AudioEncoderCtx->sample_rate;
                    av_frame_get_buffer(output_frame, 0);
                    av_audio_fifo_read(PcmBuffer, (void **)output_frame->data, frame_size);
                    output_frame->pts = av_rescale_q(pkt->pts,InAudioStream->time_base,AudioEncoderCtx->time_base);
                    output_frame->pts -= av_audio_fifo_size(PcmBuffer);
                    AudioPts += output_frame->nb_samples;
                    int ret = avcodec_send_frame(AudioEncoderCtx, output_frame);
                    if ((ret == AVERROR(AVERROR_EOF)) || (ret < 0))
                    {
                        av_packet_unref(&output_packet);
                        break;
                    }
                    ret = avcodec_receive_packet(AudioEncoderCtx, &output_packet);
                    if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR(AVERROR_EOF)) || (ret < 0))
                    {
                        av_packet_unref(&output_packet);
                        break;
                    }
                    av_packet_rescale_ts(&output_packet,AudioEncoderCtx->time_base, OutVideoStream->time_base);
                    ret = av_interleaved_write_frame(OutFmtCtx, &output_packet);
                    if (ret < 0)
                    {
                        printf( "Error during writing data to output file. Error code: %d\n", ret);
                        if(ret != -22)
                        {
                            CloseOutput();
                            return ;
                        }
                    }
                }
            }
        }
    }
}

int QSVTranscode::encode_write(AVFrame *frame)
{
    int ret = 0;
    AVPacket enc_pkt;

    av_init_packet(&enc_pkt);
    enc_pkt.data = NULL;
    enc_pkt.size = 0;

    if ((ret = avcodec_send_frame(VideoEncoderCtx, frame)) < 0)
    {
        printf("Error during encoding. Error code: %d\n", ret);
        goto end;
    }
    while (1)
    {
        ret = avcodec_receive_packet(VideoEncoderCtx, &enc_pkt);
        if (ret != 0)
        {
            break;
        }
        //enc_pkt.pts = frame->pts;
        enc_pkt.stream_index = OutVideoStream->index;
        av_packet_rescale_ts(&enc_pkt,InVideoStream->time_base, OutVideoStream->time_base);

        enc_pkt.pos = 0;
        if (OutHeadWrited && OutFmtCtx)
        {
            ret = av_interleaved_write_frame(OutFmtCtx, &enc_pkt);
            //ret = av_write_frame(OutFmtCtx, &enc_pkt);
            if (ret < 0)
            {
                printf( "Error during writing data to output file. Error code: %d\n", ret);
                if(ret != -22)
                {
                    CloseOutput();
                    return -1;
                }
            }
        }
        av_packet_unref(&enc_pkt);
    }

end:
    if (ret == AVERROR_EOF)
        return 0;
    ret = ((ret == AVERROR(EAGAIN)) ? 0:-1);
    return ret;
}

