#include <stdio.h>
#include "QSVTranscode.h"


int main(int argc, char **argv)
{
    if ((argc != 4) && (argc != 5))
    {
        fprintf(stderr, "Usage: %s <input file> <encode codec> <output file> <output type>\n", argv[0]);
        return -1;
    }
    OutputInfo videoinfo;
    videoinfo.VideoWidth = 1280;
    videoinfo.VideoHeight = 720;
    videoinfo.VideoBitrate = 2000000;
    videoinfo.VideoProfile = FF_PROFILE_H264_HIGH_422;
    videoinfo.OutputUrl = argv[3];
    videoinfo.OutputType = argv[4];
    videoinfo.VideoEncoderName = argv[2];

    AudioEncodeInfo audioinfo;
    audioinfo.ChannelLayOut = AV_CH_LAYOUT_STEREO;
    audioinfo.SampleRate = 16000;
    audioinfo.BitRate = 48000;
    audioinfo.SampleFmt = AV_SAMPLE_FMT_S16;

    QSVTranscode* transcoder = new QSVTranscode(argv[1], &videoinfo, &audioinfo);
    while(true)
    {
        av_usleep(1000000);
    }
    return 0;
}
