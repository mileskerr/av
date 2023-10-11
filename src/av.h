#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#define MIN(A, B) (A < B) ? (A) : (B)
#define MAX(A, B) (A > B) ? (A) : (B)
