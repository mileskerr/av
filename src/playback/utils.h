#pragma once
#include "../av.h"

AVChannelLayout nb_ch_to_av_ch_layout(int n);

enum AVSampleFormat sample_fmt_sdl_to_av(int sdl_fmt);

int get_texture_pitch(uint32_t format, int w);
