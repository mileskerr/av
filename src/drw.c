#include "drw.h"

#define CL(COLOR) COLOR.r, COLOR.g, COLOR.b, COLOR.a

static SDL_Color cl_mul(SDL_Color * cl, double m) {
    return (SDL_Color) { 
        (uint8_t) (((double) cl->r) * m),
        (uint8_t) (((double) cl->g) * m),
        (uint8_t) (((double) cl->b) * m),
        0xff
    };
    
}


struct ColorScheme default_colors(void) {
    SDL_Color base_fg = { 0xcd, 0xd6, 0xf4, 0xff };
    SDL_Color base_bg = { 0x31, 0x32, 0x44, 0xff };
    return (struct ColorScheme) {
        .bg = {
            cl_mul(&base_bg, 1.4), cl_mul(&base_bg, 1.2), base_bg,
            cl_mul(&base_bg, 0.7), cl_mul(&base_bg, 0.4)
        },
        .fg = {
            cl_mul(&base_fg, 0.6), cl_mul(&base_fg, 0.8), base_fg,
            cl_mul(&base_fg, 1.1), cl_mul(&base_fg, 1.2)
        },
        .highl_bg = { 0xb4, 0xbe, 0xfe, 0xff },
        .highl_fg = { 0x18, 0x18, 0x25, 0xff },
        .acc_bg = { 0xf3, 0x8b, 0xa8, 0xff },
        .acc_fg = { 0x18, 0x18, 0x25, 0xff }
    };
}


struct Layout get_layout(
    const int window_w, const int window_h, 
    const int pic_w, const int pic_h,
    const int timeline_h,
    const int progress_h
) {
    struct Layout layout;
    SDL_Rect view_bounds = (SDL_Rect) {
        0, 0, window_w, window_h - timeline_h - progress_h
    };
    SDL_Rect viewer_rect = (SDL_Rect) {
        .w = MIN(view_bounds.w, view_bounds.h * pic_h / pic_w),
        .h = MIN(view_bounds.h, view_bounds.w * pic_w / pic_h),
    };
    viewer_rect.x = (view_bounds.w - viewer_rect.w) / 2 + view_bounds.x;
    viewer_rect.y = (view_bounds.h - viewer_rect.h) / 2 + view_bounds.y;

    layout.viewer_rect = viewer_rect;

    layout.timeline_rect = (SDL_Rect) {
        0, (window_h - timeline_h), window_w, timeline_h
    };
    layout.progress_rect = (SDL_Rect) {
        0, (window_h - timeline_h - progress_h), window_w, progress_h
    };
    return layout;
}


void draw_text(
    SDL_Renderer * renderer, const char * message, 
    TTF_Font * font, const SDL_Color color, 
    enum TextAlignment alignment, int x, int y
) {

	SDL_Surface * surf = TTF_RenderText_Blended(font, message, color);
	SDL_Texture * texture = SDL_CreateTextureFromSurface(renderer, surf);

	SDL_FreeSurface(surf);
   
    int w, h;
    SDL_QueryTexture(texture, NULL, NULL, &w, &h);
    switch (alignment) {
        case ALIGN_RIGHT:
            x -= w;
            break;
        case ALIGN_CENTER:
            x -= w/2;            
            break;
        case ALIGN_LEFT:;
    };
    SDL_Rect dstrect = {
        .x = x,
        .y = y,
        .w = w,
        .h = h
    };
    SDL_RenderCopy(renderer, texture, NULL, &dstrect);

}


void draw_progress(
    SDL_Renderer * renderer, const SDL_Rect rect,
    double timestamp, double duration,
    const struct ColorScheme * colors
) {
    int label_w = 0;
    int bar_h = 4;
    int line_w = 2;

    /* fill background */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[2]));
    SDL_RenderFillRect(renderer, &rect);

    /* draw bar bounds */
    SDL_Rect bar_bounds = (SDL_Rect) {
        .w = rect.w - (label_w * 2.0),
        .h = bar_h
    };

    bar_bounds.x = (rect.w - bar_bounds.w) / 2.0 + rect.x;
    bar_bounds.y = rect.y + rect.h - bar_h; //(rect.h - bar_bounds.h) / 2.0 + rect.y;

    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderFillRect(renderer, &bar_bounds);

    /* draw bar */
    int position = timestamp * bar_bounds.w / duration;
    SDL_Rect bar = (SDL_Rect) {
        .x = bar_bounds.x, .y = bar_bounds.y,
        .w = position, .h = bar_bounds.h
    };
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(renderer, &bar);

    /* draw line */
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(
        renderer,
        &(SDL_Rect) { position + bar_bounds.x - line_w/2, rect.y, line_w, rect.h }
    );

}


/* converts a duration in seconds to string in format: hh:mm:ss.ss
 * writes maximum of 12 bytes including null terminator. */
static void dur_to_str(double duration, char * dst) {
    duration = MAX(duration, 0.0);
    int hours = ((int)duration / 3600) % 24;
    int minutes = ((int)duration / 60) % 60;
    int seconds = ((int) duration) % 60;
    int hundredths = (int)(duration * 100) % 100;

    char src[12] = {
        (hours / 10) % 10 + '0',
        hours % 10 + '0',
        ':',
        (minutes / 10) % 10 + '0',
        minutes % 10 + '0',
        ':',
        (seconds / 10) % 10 + '0',
        seconds % 10 + '0',
        '.',
        (hundredths / 10) % 10 + '0',
        (hundredths % 10) + '0',
        '\0'
    };

    int i;
    for (i = 0; 
        (i < 7) && ((src[i] == '0') || (src[i] == ':'));
        i++);

    memcpy(dst, src+i, 12-i);
}


void draw_timeline(
    SDL_Renderer * renderer, TTF_Font * font, SDL_Rect rect,
    double start_time, double timestamp, double duration,
    const struct ColorScheme * colors
) {
    int label_h = 20;
    int label_w = 100;
    int pixels_per_sec = 120;
    int mnr_tms_per_mjr = 4;
    int mjr_tm_w = 2;
    int mnr_tm_w = 2;
    double halfwidth = ((double) rect.w)/2.0;
    char timestamp_str[12];


    /* draw tickmark area */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[1]));
    SDL_RenderFillRect(
        renderer,
        &(SDL_Rect) { rect.x, rect.y + label_h, rect.w, rect.h - label_h }
    );

    /* draw darker area left of the video start point & right of video end*/
    double tlend= timestamp - halfwidth/pixels_per_sec;
    double trend = timestamp + halfwidth/pixels_per_sec;
    if (tlend < start_time) {
        SDL_Rect r = { 
            rect.x, rect.y,
            (start_time - timestamp) * pixels_per_sec + halfwidth,
            rect.h, 
        };
        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(renderer, &r);
    }
    if (trend > duration) {
        int w = 
            halfwidth - (duration - timestamp) * pixels_per_sec;
        SDL_Rect r = { 
            rect.x + rect.w - w, rect.y,
            w, rect.h,
        };
        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(renderer, &r);
    }

    /* draw label area */
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect) { rect.x, rect.y, rect.w, label_h }
    );

    /* draw tickmarks */
    int nmaj_tms = (int) ceil(((double) rect.w) / pixels_per_sec);

    double ts_intg, ts_frac;
    ts_frac = modf(timestamp, &ts_intg);

    for (int i = -nmaj_tms/2; i < nmaj_tms/2 + 2; i++) {

        int time = ts_intg + i;
        if (time < 0)
            continue;
        if (time > duration)
            continue;

        int x = (time - timestamp) * pixels_per_sec + halfwidth;

        SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
        SDL_RenderFillRect(
            renderer, 
            &(SDL_Rect) {
                x + rect.x - ((float)mjr_tm_w/2), rect.y + label_h,
                mjr_tm_w, rect.h - label_h
            }
        );
        dur_to_str(time, timestamp_str);
        draw_text(
            renderer, timestamp_str, font,
            colors->fg[1], ALIGN_CENTER, x,
            rect.y
        );
        double mnr_spacing = ((double)pixels_per_sec)/mnr_tms_per_mjr;
        for (int j = 1; j < mnr_tms_per_mjr; j++) {
            double mnr_x = x + j * mnr_spacing;
            SDL_SetRenderDrawColor(renderer, CL(colors->bg[2]));
            SDL_RenderFillRect(
                renderer, 
                &(SDL_Rect) {
                    mnr_x + rect.x - ((float)mnr_tm_w/2), rect.y + label_h,
                    mnr_tm_w, rect.h - label_h
                }
            );
        }
    }
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[3]));
    SDL_RenderDrawLine(renderer, rect.x, rect.y + label_h, rect.w + rect.x, rect.y + label_h);

    /* draw current frame */
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_bg));
    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect){
            halfwidth - ((float)mjr_tm_w/2),
            rect.y, mjr_tm_w, rect.h
        }
    );

    SDL_RenderFillRect(
        renderer, 
        &(SDL_Rect){
            halfwidth - label_w,
            rect.y, label_w, label_h - 2
        }
    );
    SDL_SetRenderDrawColor(renderer, CL(colors->highl_fg));

    dur_to_str(timestamp, timestamp_str);
    draw_text(
        renderer, timestamp_str, font,
        colors->highl_fg, ALIGN_RIGHT, halfwidth,
        rect.y
    );
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[4]));
    SDL_RenderDrawLine(renderer, rect.x, rect.y, rect.w + rect.x, rect.y);
}

void draw_background( SDL_Renderer * renderer, struct ColorScheme * colors) {
    SDL_SetRenderDrawColor(renderer, CL(colors->bg[4]));
    SDL_RenderClear(renderer);
}

