#include "av.h"

struct ColorScheme {
    SDL_Color bg[5]; /* backgrounds, lowest to highest contrast */
    SDL_Color fg[5]; /* foregrounds, lowest to highest contrast */
    SDL_Color highl_bg; /* bright color for selected items */
    SDL_Color highl_fg;
    SDL_Color acc_bg; /* bright color complimentary to highlight */
    SDL_Color acc_fg;
};

struct Layout {
    SDL_Rect viewer_rect;
    SDL_Rect timeline_rect;
    SDL_Rect progress_rect;
};

enum TextAlignment {
    ALIGN_LEFT,
    ALIGN_CENTER,
    ALIGN_RIGHT
};

/* draws text at x, y.
 * horizontal alignment is specified by alignment,
 * vertical algignment is always from top. */
void draw_text(
    SDL_Renderer * renderer, const char * message, 
    TTF_Font * font, const SDL_Color color, 
    enum TextAlignment alignment, int x, int y
);

/* get default color scheme (catppuccin mocha) */
struct ColorScheme default_colors(void);

/* +--+----------+--+
 * |  |          |  | <-view_bounds
 * |  |        <-|--|-viewer_rect
 * |  |          |  |
 * +--+----------+--+ <-progress_rect
 * +----------------+ 
 * |                | <-timeline_rect
 * +----------------+ 
 *
 * computes bounds of each rectangle comprising the layout shown above
 * based on original video dimensions (pic_w, pic_h), window dimensions
 * (window_w, window_h), and height of progress bar and timeline */
struct Layout get_layout(
    const int window_w, const int window_h, 
    const int pic_w, const int pic_h,
    const int timeline_h,
    const int progress_h
);

void draw_progress(
    SDL_Renderer * renderer, const SDL_Rect rect,
    double timestamp, double duration,
    const struct ColorScheme * colors
);

void draw_timeline(
    SDL_Renderer * renderer, TTF_Font * font, SDL_Rect rect,
    double start_time, double timestamp, double duration,
    const struct ColorScheme * colors
);

void draw_background(
    SDL_Renderer * renderer, struct ColorScheme * colors
);

