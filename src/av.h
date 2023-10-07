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

#define TRY(A) {int ret___; \
  if ((ret___ = A)) { \
    fprintf(stderr, \
      "%s\n%s:%d%s %s%s -> %s%s\n", \
      CLGRAY, __FILE__, __LINE__, CLRESET, #A, CLGRAY, av_err2str(ret___), CLRESET \
    ); \
    return ret___; \
  } \
}

