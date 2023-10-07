#pragma once

#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <stdint.h>

#define CLRED "\033[31m"
#define CLGRAY "\033[90m"
#define CLRESET "\033[39m"
/*
#define AV_ERR_P(E) { \
  fprintf(stderr, \
    "%s\n%s:%d%s %s%s -> %s%s\n", \
    CLGRAY, __FILE__, __LINE__, CLRESET, #A, CLGRAY, av_err2str(E), CLRESET \
  ); \
}

#define TRY_OR(FN, IF_NONZERO) { \
  if ((ret___ = A)) \
    IF_NONZERO \
}

#define SDL_ERR_P { \
  fprintf(stderr, \
    "%s\n%s:%d%s %s%s -> %s%s\n", \
    CLGRAY, __FILE__, __LINE__, CLRESET, #A, CLGRAY, SDL_GetError(), CLRESET \
  ); \
}
*/

