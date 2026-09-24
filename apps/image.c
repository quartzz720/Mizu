#include "mizu.h"
#include "png.h"
#include "gif.h"

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

/* Reading, in pieces large enough to be worth the asking.
 *
 * A picture was read one row at a time, which for a photograph is a thousand
 * system calls and a thousand trips through the file system to move three
 * kilobytes each. Measured on the same file: 212 ms a row at a time against
 * 130 ms in thirty-two kilobyte pieces from the disk, and 106 against 84 from
 * a USB stick. Half again, for a buffer and twenty lines.
 *
 * What is left after this is the size of the file itself. An uncompressed BMP
 * of 1920 by 1080 is six megabytes and every one of them has to arrive; no
 * amount of buffering makes that instant, and the answer to it is a format
 * that compresses. */
#define READ_CHUNK 32768

static unsigned char buffered[READ_CHUNK];
static long buffered_have;
static long buffered_at;

static void reading_begins(void) {
    buffered_have = 0;
    buffered_at = 0;
}

static int read_exactly(long handle, void* into, long length) {
    unsigned char* out = (unsigned char*)into;
    long done = 0;

    while (done < length) {
        long available = buffered_have - buffered_at;
        long take;

        if (available <= 0) {
            long got = koi_read(handle, buffered, READ_CHUNK);

            if (got <= 0) return 0;
            buffered_have = got;
            buffered_at = 0;
            available = got;
        }
        take = length - done;
        if (take > available) take = available;
        for (long index = 0; index < take; index++)
            out[done + index] = buffered[buffered_at + index];
        buffered_at += take;
        done += take;
    }
    return 1;
}

/* The picture as it appears in the window, kept.
 *
 * Scaling used to happen inside paint - every repaint, and one system call per
 * row of it. A window seven hundred rows tall was seven hundred calls and a
 * full resample of the picture every time the clock ticked, which is what
 * "the system lags while a picture is open" was.
 *
 * So it is scaled once, when the picture is loaded or the window is resized,
 * and painting is one blit of the result. The test for "is it still valid" is
 * the size it was made for: a window that has not changed size does not need
 * it made again. */
static koi_uint32* scaled;
static int scaled_width;
static int scaled_height;
static int scaled_for_width;
static int scaled_for_height;

static void forget_scaled(void) {
    if (scaled) koi_free(scaled);
    scaled = (koi_uint32*)0;
    scaled_width = 0;
    scaled_height = 0;
    scaled_for_width = 0;
    scaled_for_height = 0;
}

static void forget(void) {
    forget_scaled();
    if (pixels) koi_free(pixels);
    pixels = (koi_uint32*)0;
    image_width = 0;
    image_height = 0;
}

/* A PNG, which needs the whole file in memory and room for the rows it
 * expands into - both borrowed for the length of the call and given back.
 *
 * The transparent parts are composited onto the window's paper colour rather
 * than kept: this viewer draws opaque pixels, and a picture handed over with
 * an alpha channel would leave every caller inventing its own answer. Paper
 * is the right answer here because that is what is behind it. */
/* A GIF, of which the first frame is shown. An animated one has its later
 * frames read and ignored here: this window paints when it is told to, and
 * making it paint on a timer belongs with the browser's animation rather than
 * beside it. */
static int load_gif(long handle, long size) {
    koi_uint8* file = (koi_uint8*)koi_alloc(size);
    koi_uint8* scratch;
    GIF gif;
    long got = 0;

    if (!file) {
        koi_snprintf(trouble, sizeof(trouble), "Not enough memory for it.");
        return 0;
    }
    while (got < size) {
        long step = koi_read(handle, file + got, size - got);

        if (step <= 0) break;
        got += step;
    }
    if (got != size || !gif_open(file, got, &gif)) {
        koi_snprintf(trouble, sizeof(trouble), "%s", gif_trouble());
        koi_free(file);
        return 0;
    }

    pixels = (koi_uint32*)koi_alloc((long)gif.width * gif.height * 4);
    scratch = (koi_uint8*)koi_alloc((long)gif.width * gif.height + 64);
    if (!pixels || !scratch) {
        koi_snprintf(trouble, sizeof(trouble),
                     "Not enough memory for a picture that size.");
        if (scratch) koi_free(scratch);
        forget();
        koi_free(file);
        return 0;
    }

    if (!gif_frame(&gif, 0, pixels, mizu->color(MIZU_COLOR_PAPER), scratch,
                   (long)gif.width * gif.height + 64)) {
        koi_snprintf(trouble, sizeof(trouble), "%s", gif_trouble());
        koi_free(scratch);
        forget();
        koi_free(file);
        return 0;
    }

    image_width = gif.width;
    image_height = gif.height;
    koi_free(scratch);
    koi_free(file);
    return 1;
}

static int load_png(long handle, long size) {
    koi_uint8* file = (koi_uint8*)koi_alloc(size);
    koi_uint8* work;
    long work_size;
    long got = 0;
    int width, height;
    PNG picture;

    if (!file) {
        koi_snprintf(trouble, sizeof(trouble), "Not enough memory for it.");
        return 0;
    }
    while (got < size) {
        long step = koi_read(handle, file + got, size - got);

        if (step <= 0) break;
        got += step;
    }
    if (got != size || !png_size(file, got, &width, &height)) {
        koi_snprintf(trouble, sizeof(trouble), "%s", png_trouble());
        koi_free(file);
        return 0;
    }

    pixels = (koi_uint32*)koi_alloc((long)width * height * 4);
    /* Four bytes a pixel plus a filter byte a row is the most the rows can
       expand to, and the file itself sits in front of that because inflate
       reads its input while writing its output. */
    work_size = size + ((long)width * 4 + 1) * height + 64;
    work = (koi_uint8*)koi_alloc(work_size);
    if (!pixels || !work) {
        koi_snprintf(trouble, sizeof(trouble),
                     "Not enough memory for a picture that size.");
        if (work) koi_free(work);
        forget();
        koi_free(file);
        return 0;
    }

    /* The file is copied to the front of the working buffer, because the
       decoder wants its input and its output in one region it was given. */
    for (long at = 0; at < got; at++) work[at] = file[at];
    koi_free(file);
    png_scratch(work, work_size);

    if (!png_decode(work, got, pixels, (long)width * height,
                    mizu->color(MIZU_COLOR_PAPER), &picture)) {
        koi_snprintf(trouble, sizeof(trouble), "%s", png_trouble());
        koi_free(work);
        forget();
        return 0;
    }
    koi_free(work);

    image_width = picture.width;
    image_height = picture.height;
    return 1;
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
    reading_begins();
    if (!read_exactly(handle, header, 8)) {
        koi_snprintf(trouble, sizeof(trouble), "There is nothing in that file.");
        koi_close(handle);
        return 0;
    }
    /* Which kind by what is in it rather than by what it is called. A picture
       saved with the wrong ending is somebody else's mistake and not a reason
       to refuse it. */
    if (gif_is_gif(header, 8)) {
        long size = koi_filesize(handle);
        int ok;

        koi_seek(handle, 0, 0);
        ok = load_gif(handle, size);
        koi_close(handle);
        if (ok) {
            koi_snprintf(shown, sizeof(shown), "%s", path);
            forget_scaled();
        }
        return ok;
    }
    if (png_is_png(header, 8)) {
        long size = koi_filesize(handle);
        int ok;

        koi_seek(handle, 0, 0);
        ok = load_png(handle, size);
        koi_close(handle);
        if (ok) {
            koi_snprintf(shown, sizeof(shown), "%s", path);
            forget_scaled();
        }
        return ok;
    }
    if (!read_exactly(handle, header + 8, 46) || header[0] != 'B' ||
        header[1] != 'M') {
        koi_snprintf(trouble, sizeof(trouble),
                     "That is not a picture this reads - BMP, PNG and GIF.");
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

        /* A turn for the desktop, every so often.
         *
         * A large picture is a second or two of reading, and this loop used to
         * take all of it without once letting go - so the clock stopped, the
         * pointer stuck where it was, and the whole machine appeared to seize
         * until the window opened. It was not seized; it was being polite to
         * nobody. Cooperative multitasking means exactly this line, and a loop
         * that does not have one is the cost being paid in full.
         *
         * Every sixteenth row rather than every row: yielding is a pass of the
         * desktop's loop, and doing that a thousand times would cost more than
         * the reading does. Sixteen rows is a few milliseconds, which is under
         * what a hand can notice. */
        if ((line & 15) == 0) mizu->yield();

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

    /* Made to fit this window, if it has not been already. */
    if (!scaled || scaled_for_width != width || scaled_for_height != height) {
        int drawn_w = image_width;
        int drawn_h = image_height;

        /* Fit, and never enlarge: the same picture at the same size in a
           bigger window, centred, which is what somebody expects. */
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

        forget_scaled();
        scaled = (koi_uint32*)koi_alloc((long)drawn_w * drawn_h * 4);
        if (!scaled) {
            mizu->label(x + 8, y + 8, "Not enough memory to show it.",
                        mizu->color(MIZU_COLOR_TEXT));
            return;
        }
        /* No yielding in here, and that is deliberate.
         *
         * This runs inside paint, which the desktop calls while it is drawing
         * a frame. Letting go in the middle of that lets the desktop start
         * another frame on top of the half-finished one, and what somebody
         * sees is the wallpaper tearing for an instant whenever the window is
         * resized. Reading the file yields, because reading is slow and
         * happens outside painting; scaling is a few milliseconds and happens
         * inside it. */
        for (int row = 0; row < drawn_h; row++) {
            const koi_uint32* source =
                pixels + (long)(row * image_height / drawn_h) * image_width;
            koi_uint32* into = scaled + (long)row * drawn_w;

            for (int column = 0; column < drawn_w; column++)
                into[column] = source[column * image_width / drawn_w];
        }
        scaled_width = drawn_w;
        scaled_height = drawn_h;
        scaled_for_width = width;
        scaled_for_height = height;
    }

    /* And painting is this: one call. */
    koi_gfx_blit(x + (width - scaled_width) / 2,
                 y + (height - scaled_height) / 2,
                 scaled_width, scaled_height, scaled, scaled_width);
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
