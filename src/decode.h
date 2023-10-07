#pragma once
#include "av.h"

int init_dec(const char *);
int dec_get_pixels(uint8_t * pixels, int w, int h, int pitch);
int dec_get_frame(AVFrame *, int, int);
