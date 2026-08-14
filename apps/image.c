#include "mizu.h"

/* A picture, in a window.
 *
 * BMP, and only the uncompressed forms - 24 and 32 bits, BI_RGB. That is the
 * one image format a system can read without a decoder: no compression to
 * undo, no entropy coding, no tables. PNG needs an inflate and JPEG needs a
 * cosine transform, and neither belongs in the first thing that puts a picture
 * on a screen. When there is an inflate, this file gains a branch.
 *
 * It scales to fit rather than cropping, and never enlarges past the window:
 * a viewer that shows the top-left corner of a photograph has answered a
 * question nobody asked. Nearest neighbour, because the wallpaper does the
 * same and a smoother one is a table of weights this does not need yet.
 *
 * Which file it shows is the command line the desktop hands it - see
 * MIZU_APP.open_with in mizu.h. That is new: until now an application was
 * opened, not opened *on* something, and a browser that can only launch things
 * is a browser that cannot show you a picture.
 */

#define IMAGE_CLOSE 1

#define IMAGE_MAX_WIDTH 2048

static const MIZU_API* mizu;
static WINDOW* window;

static koi_uint32* pixels;
static int image_width;
static int image_height;
static char shown[128];
static char trouble[80];

static koi_uint32 read32(const koi_uint8* at) {
    return (koi_uint32)at[0] | ((koi_uint32)at[1] << 8) |
           ((koi_uint32)at[2] << 16) | ((koi_uint32)at[3] << 24);
}

static koi_uint16 read16(const koi_uint8* at) {
    return (koi_uint16)((koi_uint32)at[0] | ((koi_uint32)at[1] << 8));
}

static int read_exactly(long handle, void* into, long length) {
    long done = 0;
    while (done < length) {
        long got = koi_read(handle, (char*)into + done, length - done);
        if (got <= 0) return 0;
        done += got;
    }
    return 1;
}

static void forget(void) {
    if (pixels) koi_free(pixels);
    pixels = (koi_uint32*)0;
    image_width = 0;
    image_height = 0;
}

static int load(const char* path) {
    koi_uint8 header[54];
    static koi_uint8 row[IMAGE_MAX_WIDTH * 4];
    long handle;
    koi_uint32 offset, info_size;
    long width, height;
    koi_uint16 depth;
    koi_uint32 compression;
    long padded, consumed = 54;
    int upside_down = 0;

    forget();
    trouble[0] = 0;
    handle = koi_open(path, OPEN_READ);
    if (handle < 0) { koi_snprintf(trouble, sizeof(trouble), "Cannot open it."); return 0; }
    if (!read_exactly(handle, header, 54) || header[0] != 'B' || header[1] != 'M') {
        koi_snprintf(trouble, sizeof(trouble), "That is not a BMP.");
        koi_close(handle);
        return 0;
    }

    offset = read32(header + 10);
    info_size = read32(header + 14);
    width = (long)(int)read32(header + 18);
    height = (long)(int)read32(header + 22);
    depth = read16(header + 28);
    compression = read32(header + 30);
    if (height < 0) { height = -height; upside_down = 1; }

    if (compression != 0 || (depth != 24 && depth != 32)) {
        koi_snprintf(trouble, sizeof(trouble),
                     "Only uncompressed 24 or 32-bit BMP, and this is %u-bit.",
                     (unsigned int)depth);
        koi_close(handle);
        return 0;
    }
    if (width <= 0 || width > IMAGE_MAX_WIDTH || height <= 0 || height > 4096) {
        koi_snprintf(trouble, sizeof(trouble), "That size is out of range.");
        koi_close(handle);
        return 0;
    }

    padded = (width * (depth / 8) + 3) & ~3L;
    pixels = (koi_uint32*)koi_alloc(width * height * 4);
    if (!pixels) {
        koi_snprintf(trouble, sizeof(trouble), "Not enough memory for it.");
        koi_close(handle);
        return 0;
    }

    /* Whatever sits between the header and the pixels - a longer info header,
       colour masks, a palette nobody asked for - is skipped rather than
       assumed absent. */
    while (consumed < (long)offset) {
        long chunk = (long)offset - consumed;
        if (chunk > (long)sizeof(row)) chunk = (long)sizeof(row);
        if (!read_exactly(handle, row, chunk)) break;
        consumed += chunk;
    }

    for (long line = 0; line < height; line++) {
        long target = upside_down ? line : height - 1 - line;
        koi_uint32* into = pixels + target * width;

        if (!read_exactly(handle, row, padded)) {
            koi_snprintf(trouble, sizeof(trouble), "It ends before its pixels do.");
            koi_close(handle);
            forget();
            return 0;
        }
        for (long column = 0; column < width; column++) {
            const koi_uint8* pixel = row + column * (depth / 8);
            into[column] = koi_gfx_color(pixel[2], pixel[1], pixel[0]);
        }
        /* A large picture is a moment's work and the desktop should not stop
           for it. Not every row: a turn costs the desktop a repaint, and a
           repaint of a half-read picture is work thrown away. */
        if ((line & 127) == 0) mizu->yield();
    }
    koi_close(handle);
    image_width = (int)width;
    image_height = (int)height;
    return 1;
}

static void paint(WINDOW* self, int x, int y, int width, int height) {
    (void)self;
    koi_gfx_fill(x, y, width, height, mizu->color(MIZU_COLOR_PAPER));

    if (!pixels || !image_width || !image_height) {
        mizu->label(x + 8, y + 8, trouble[0] ? trouble : "No picture.",
                    mizu->color(MIZU_COLOR_TEXT));
        return;
    }

    {
        /* Fit, and never enlarge: the same picture at the same size in a
           bigger window, centred, which is what somebody expects. */
        int drawn_w = image_width;
        int drawn_h = image_height;
        int left, top;

        if (drawn_w > width) {
            drawn_h = drawn_h * width / drawn_w;
            drawn_w = width;
        }
        if (drawn_h > height) {
            drawn_w = drawn_w * height / drawn_h;
            drawn_h = height;
        }
        if (drawn_w < 1) drawn_w = 1;
        if (drawn_h < 1) drawn_h = 1;
        left = x + (width - drawn_w) / 2;
        top = y + (height - drawn_h) / 2;

        /* One row of the picture as it will appear, then one call to put it
           on the screen. A pixel at a time is two hundred thousand system
           calls for one photograph, which is not slow but stopped - and it is
           what this did until the kernel grew a blit. */
        for (int row = 0; row < drawn_h; row++) {
            static koi_uint32 line[IMAGE_MAX_WIDTH];
            const koi_uint32* source =
                pixels + (long)(row * image_height / drawn_h) * image_width;
            int span = drawn_w > IMAGE_MAX_WIDTH ? IMAGE_MAX_WIDTH : drawn_w;

            for (int column = 0; column < span; column++)
                line[column] = source[column * image_width / drawn_w];
            koi_gfx_blit(left, top + row, span, 1, line, span);
        }
    }
}

static void menu(WINDOW* self, int id) {
    (void)self;
    if (id == IMAGE_CLOSE) {
        forget();
        mizu->window_delete(window);
        window = (WINDOW*)0;
    }
}

static void closing(WINDOW* self) {
    if (self == window) { forget(); window = (WINDOW*)0; }
}

static WINDOW* open_with(const char* path) {
    if (!window) {
        window = mizu->window_new("Picture", 200, 120, 520, 400);
        if (!window) return (WINDOW*)0;
        window->paint = paint;
        window->menu_count = 1;
        window->menus[0] = (WINDOW_MENU){ "File",
            { { "Close", IMAGE_CLOSE } }, 1 };
    }
    window->minimised = 0;
    mizu->window_raise(window);

    if (path && path[0]) {
        long at = 0;
        while (path[at] && at + 1 < (long)sizeof(shown)) { shown[at] = path[at]; at++; }
        shown[at] = 0;
        load(shown);
        koi_snprintf(window->title, WINDOW_TITLE_MAX, "Picture - %s", shown);
    }
    mizu->repaint();
    return window;
}

static WINDOW* open(void) {
    return open_with(shown[0] ? shown : (const char*)0);
}

/* 2: it has open_with, which is the whole reason it exists. */
static MIZU_APP me = { "Picture", 2, open, menu, closing, open_with };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < 4) return (MIZU_APP*)0;   /* 4 brought open_with */
    mizu = api;
    return &me;
}
