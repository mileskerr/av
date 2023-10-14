#include "av.h"

AVChannelLayout nb_ch_to_av_ch_layout(int n) {
    switch (n) {
        case 1: default:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
        case 2:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
        case 4:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_QUAD;
        case 6:
            return (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
    }
}

enum AVSampleFormat sample_fmt_sdl_to_av(int sdl_fmt) {
    switch (sdl_fmt) {
        case AUDIO_U8: return AV_SAMPLE_FMT_U8;
        case AUDIO_S16SYS:
            return AV_SAMPLE_FMT_S16;
        case AUDIO_S32SYS:
            return AV_SAMPLE_FMT_S32;
        case AUDIO_F32SYS:
            return AV_SAMPLE_FMT_FLT;
        default: return -1;
    }    
}


static int get_texture_pitch(SDL_Texture * texture) {
    int w;

    uint32_t format;
    SDL_QueryTexture(texture, &format, NULL, &w, NULL);
    return (w * SDL_BYTESPERPIXEL(format) + 3) & ~3;
}
