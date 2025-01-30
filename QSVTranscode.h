#ifndef QSVTRANSCODE_H
#define QSVTRANSCODE_H

#include <boost/thread.hpp>

extern "C"
{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/fifo.h>
    #include <libavutil/time.h>
    #include <libavutil/audio_fifo.h>
}


struct OutputInfo
{
    int VideoWidth;
    int VideoHeight;
    int VideoBitrate;
    int VideoProfile;
    char* OutputUrl;
    char* OutputType;
    char* VideoEncoderName;
};

struct AudioEncodeInfo
{
    int64_t             ChannelLayOut;
    int                 SampleRate;
    int                 BitRate;
    AVSampleFormat      SampleFmt;
};

class QSVTranscode
{
    public:
        QSVTranscode(char* inputurl, OutputInfo* outset, AudioEncodeInfo* audioset);
        virtual ~QSVTranscode();
    public:
        AVBufferRef*        QSV_hw_device_ctx;
        AVBufferRef*        qsv_hw_frames_ctx;
    protected:
        bool OpenInput();
        bool OpenOutput();

        void ReadPacketProc();
        void WritePacketProc();

        void DecodeVideo(AVPacket* pkt);
        void DecodeAudio(AVPacket* pkt);
        int encode_write(AVFrame *frame);

        void init_filters();
        void openencoder();
        void Check();
        void WriteOutHead();
        void CloseOutput();
        void CloseInPut();
    private:
        AVFilterGraph*      filter_graph;
        AVFilterContext*    buffersrc_ctx;
        AVFilterContext*    buffersink_ctx;
    private:
        int64_t             AudioPts;
        OutputInfo*         OutputSet;
        AudioEncodeInfo*    AudioSet;
        bool                Runing;
        bool                InputOpend;
        bool                OutputOpend;
        bool                OutHeadWrited;
        char*               InputUrl;

        AVFormatContext*    InFmtCtx;
        AVFormatContext*    OutFmtCtx;
        AVCodecContext*     VideoDecoderCtx;
        AVCodecContext*     VideoEncoderCtx;

        AVCodecContext*     AudioDecoderCtx;
        AVCodecContext*     AudioEncoderCtx;

        AVStream*           InAudioStream;
        AVStream*           InVideoStream;
        AVStream*           OutAudioStream;
        AVStream*           OutVideoStream;
        bool                VEncInited;
        bool                VFilterInited;

        AVCodec*            VideoEncCodec;
        AVCodec*            AudioEncCodec;
        struct SwrContext*  SwrCtx;

        AVFifoBuffer*       PktBuffer;
        AVAudioFifo*        PcmBuffer;

        boost::thread*      ReadThread;
        boost::thread*      WriteThread;
};

#endif // QSVTRANSCODE_H
