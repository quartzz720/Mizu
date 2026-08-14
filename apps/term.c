#include "mizu.h"

/* A Koi-DOS prompt, in a window.
 *
 * The desktop keeps the screen; the command's output is collected and printed
 * here. That is the whole trick, and it needed one kernel call: the shell has
 * been able to send a command's output somewhere other than the screen since
 * `>` and `|` existed, and SYS_CAPTURE is a program being allowed to ask for
 * the same thing.
 *
 * What this is not: a terminal emulator. There is no cursor addressing, no
 * colour, no scrollback for a program that redraws itself, and nothing that
 * waits for a key gets one - the command runs to its end with nobody reading
 * the keyboard for it, so `edit` here would hang and is not the way to run it.
 * `dir`, `type`, `ver`, `mem`, `set`, `dosget list`: the commands that answer
 * and stop, which is most of them and all of the ones somebody wants while
 * they are looking at something else.
 *
 * A command that draws is refused rather than started, because it would find
 * the screen already taken by the desktop and fail in a way nobody could read.
 * Those belong on Run..., which gives up the screen properly.
 */

#define TERM_CLEAR 1
#define TERM_CLOSE 2

#define LINES_MAX 200
#define LINE_MAX 96
#define INPUT_MAX 96
#define OUTPUT_MAX 8192

static const MIZU_API* mizu;
static WINDOW* window;

static char lines[LINES_MAX][LINE_MAX];
static int line_count;
static int top_line;
static char input[INPUT_MAX];
static int input_length;
static char output[OUTPUT_MAX];

static void add_line(const char* text, long length) {
    char* into;
    long at = 0;

    if (line_count >= LINES_MAX) {
        /* The oldest goes. A window that stops accepting output after two
           hundred lines is a window that lies about what happened. */
        for (int index = 1; index < LINES_MAX; index++)
            for (int byte = 0; byte < LINE_MAX; byte++)
                lines[index - 1][byte] = lines[index][byte];
        line_count = LINES_MAX - 1;
    }
    into = lines[line_count++];
    while (at < length && at + 1 < LINE_MAX) { into[at] = text[at]; at++; }
    into[at] = 0;
}

/* Break what a command printed into lines. Tabs become spaces and control
   characters are dropped: this draws with one font at one size, and a stray
   escape drawn literally is worse than a gap. */
static void take_output(const char* text, long length) {
    long start = 0;

    for (long at = 0; at <= length; at++) {
        if (at == length || text[at] == '\n') {
            long end = at;
            while (end > start && text[end - 1] == '\r') end--;
            if (at > start || at < length) add_line(text + start, end - start);
            start = at + 1;
        }
    }
}

static int is_graphical(const char* line) {
    static const char* takes_the_screen[] = {
        "MIZU", "COMMANDER", "GAMES", "DOOM", "DEMO", "SHOW", "EDIT",
        "SETUP", "SELFTEST", "SPIN", 0
    };

    for (int index = 0; takes_the_screen[index]; index++) {
        const char* name = takes_the_screen[index];
        long at = 0;

        while (name[at]) {
            char typed = line[at];
            if (typed >= 'a' && typed <= 'z') typed = (char)(typed - 32);
            if (typed != name[at]) break;
            at++;
        }
        if (!name[at] && (line[at] == 0 || line[at] == ' ' ||
                          line[at] == '\\' || line[at] == '.'))
            return 1;
    }
    return 0;
}

static void run_line(void) {
    char shown[LINE_MAX];
    long length;

    koi_snprintf(shown, sizeof(shown), "Z:\\> %s", input);
    add_line(shown, (long)strlen(shown));

    if (!input[0]) { input_length = 0; return; }

    if (is_graphical(input)) {
        add_line("That one wants the screen. Use Run... for it.", 44);
        input[0] = 0;
        input_length = 0;
        return;
    }

    length = koi_capture(input, output, sizeof(output));
    if (length > 0) take_output(output, length);
    if (length == (long)sizeof(output) - 1)
        add_line("[...output was longer than this window keeps]", 44);

    input[0] = 0;
    input_length = 0;
    top_line = line_count;      /* the newest is what somebody wants to see */
}

/* Its own colours, and not the desktop's.
 *
 * Everything else here asks the theme, because an application that draws its
 * own idea of a window looks wrong the day the theme changes. A terminal is
 * the exception and always has been: the MS-DOS Prompt in Windows 95 was
 * silver on black inside a grey window, and it was recognisable across the
 * room for exactly that reason. The colours below are that pair. Green on
 * black is the older screen - a monochrome phosphor monitor - and is one
 * constant away if that is the memory being reached for. */
static koi_uint32 paper(void) { return koi_gfx_color(0, 0, 0); }
static koi_uint32 ink(void) { return koi_gfx_color(0xC0, 0xC0, 0xC0); }

static void paint(WINDOW* self, int x, int y, int width, int height) {
    int rows = (height - WINDOW_CHAR_H - 4) / WINDOW_CHAR_H;
    int first;
    char prompt[LINE_MAX + 8];

    (void)self;
    if (rows < 1) rows = 1;
    first = line_count - rows;
    if (first < 0) first = 0;

    koi_gfx_fill(x, y, width, height, paper());
    for (int row = 0; row < rows && first + row < line_count; row++)
        mizu->label(x + 4, y + row * WINDOW_CHAR_H, lines[first + row], ink());

    /* The line being typed, at the bottom, with a caret - which is where a
       prompt belongs and where the eye already is. */
    koi_snprintf(prompt, sizeof(prompt), "Z:\\> %s_", input);
    mizu->label(x + 4, y + height - WINDOW_CHAR_H, prompt, ink());
}

static void key(WINDOW* self, int pressed) {
    (void)self;
    if (pressed == '\n' || pressed == '\r') run_line();
    else if (pressed == '\b') {
        while (input_length > 0 &&
               ((unsigned char)input[input_length - 1] & 0xC0) == 0x80)
            input_length--;
        if (input_length > 0) input_length--;
        input[input_length] = 0;
    } else if (pressed >= ' ' && pressed < 0x100 && input_length < INPUT_MAX - 1) {
        input[input_length++] = (char)pressed;
        input[input_length] = 0;
    } else {
        return;
    }
    mizu->repaint();
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case TERM_CLEAR: line_count = 0; top_line = 0; mizu->repaint(); break;
    case TERM_CLOSE:
        mizu->window_delete(window);
        window = (WINDOW*)0;
        break;
    default: break;
    }
}

static void closing(WINDOW* self) {
    if (self == window) window = (WINDOW*)0;
}

static WINDOW* open(void) {
    if (window) {
        window->minimised = 0;
        mizu->window_raise(window);
        return window;
    }
    window = mizu->window_new("Koi-DOS", 200, 150, 560, 340);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->key = key;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "File",
        { { "Clear", TERM_CLEAR }, { 0, 0 }, { "Close", TERM_CLOSE } }, 3 };
    if (!line_count) {
        add_line("Koi-DOS, in a window. Type a command.", 36);
        add_line("Anything that draws belongs on Run... instead.", 45);
    }
    return window;
}

static MIZU_APP me = { "Koi-DOS", 1, open, menu, closing };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < MIZU_API_VERSION) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
