#include "mizu.h"
#include "language.h"      /* the SAY_* names only: the table is the desktop's */
#include "settings.h"

/* Settings that change things.
 *
 * The last of Stage 4, and the shortest, because nothing here is new: the
 * kernel has had every one of these switches for months and the only place to
 * reach them was a command typed at a prompt or a question asked once when
 * Mizu was first run. A setting somebody can only change by remembering a
 * command is a setting most people never change.
 *
 * Each row is a name and a value that can be stepped with the left and right
 * halves of its button, or with the arrow keys. No dialogue, no OK: every
 * change happens when it is made and is written to \BOOT\CONFIG at the same
 * moment. That is the arrangement the machine already has - the kernel reads
 * settings once at boot and programs write them - and a settings window with
 * an Apply button would be a second copy of the state, which is the thing that
 * goes stale.
 *
 * What is not here: the wallpaper and the desktop colours. Those are Mizu's
 * own drawing rather than the kernel's, and they need somewhere to be kept
 * before they can be changed - which is a change to the desktop, not to this
 * window, and it is honest to leave the row out rather than draw one that does
 * nothing.
 */

#define SETTINGS_CLOSE 1

#define ROW_LANGUAGE 0
#define ROW_VOLUME 1
#define ROW_THEME 2
#define ROW_COUNT 3

static const MIZU_API* mizu;
static WINDOW* window;
static int chosen_row;

/* The console themes, as pairs the shell already understands. Named after
   what they are rather than after a colour scheme somebody has to picture:
   the amber one is the screen of a 1981 terminal and is the reason this list
   exists at all. */
static const struct {
    const char* name;
    int foreground;
    int background;
} themes[] = {
    { "Grey on black", KOI_LIGHT_GRAY, KOI_BLACK },
    { "Amber",         KOI_BROWN,      KOI_BLACK },
    { "Green",         KOI_LIGHT_GREEN, KOI_BLACK },
    { "White on blue", KOI_WHITE,      KOI_BLUE }
};
#define THEME_COUNT 4

static int theme_index;
static int volume_percent = 50;

static void load_state(void) {
    char text[16];

    if (settings_get("SOUND", "volume", text, sizeof(text))) {
        int value = 0;
        for (int at = 0; text[at] >= '0' && text[at] <= '9'; at++)
            value = value * 10 + (text[at] - '0');
        if (value >= 0 && value <= 100) volume_percent = value;
    }
    if (settings_get("CONSOLE", "foreground", text, sizeof(text))) {
        for (int index = 0; index < THEME_COUNT; index++) {
            char name[8];
            koi_snprintf(name, sizeof(name), "%d", themes[index].foreground);
            if (!strcmp(name, text)) { theme_index = index; break; }
        }
    }
}

/* Through the desktop, and not through language_set here.
 *
 * language.c keeps the current language in a variable, and this application
 * would have its own copy of that variable - so setting it here would change
 * what this window says and leave the desktop underneath drawing the old
 * words. It did exactly that, once, and the screenshot is why this call
 * exists. The desktop relabels itself and everything on it. */
static void set_language(int step) {
    int language = mizu->language() + step;

    if (language < 0) language = mizu->language_count() - 1;
    if (language >= mizu->language_count()) language = 0;
    mizu->language_set(language);
    koi_snprintf(window->title, WINDOW_TITLE_MAX, "%s", mizu->say(SAY_SETTINGS));
}

static void set_volume(int step) {
    char text[8];

    volume_percent += step * 10;
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    koi_sound_volume(volume_percent * 255 / 100);
    koi_snprintf(text, sizeof(text), "%d", volume_percent);
    settings_set("SOUND", "volume", text);
}

/* The shell's colours, changed for this session and written down for the
   next. Both, because one without the other is either a change that does not
   last or a setting that does nothing until a reboot. */
static void set_theme(int step) {
    char text[8];

    theme_index += step;
    if (theme_index < 0) theme_index = THEME_COUNT - 1;
    if (theme_index >= THEME_COUNT) theme_index = 0;

    koi_theme(themes[theme_index].foreground, themes[theme_index].background,
              -1, -1);
    koi_snprintf(text, sizeof(text), "%d", themes[theme_index].foreground);
    settings_set("CONSOLE", "foreground", text);
    koi_snprintf(text, sizeof(text), "%d", themes[theme_index].background);
    settings_set("CONSOLE", "background", text);
}

static void step_row(int row, int step) {
    switch (row) {
    case ROW_LANGUAGE: set_language(step); break;
    case ROW_VOLUME: set_volume(step); break;
    case ROW_THEME: set_theme(step); break;
    default: break;
    }
    mizu->repaint();
}

static const char* row_name(int row) {
    switch (row) {
    case ROW_LANGUAGE: return mizu->say(SAY_LANGUAGE);
    case ROW_VOLUME: return mizu->say(SAY_SOUND);
    case ROW_THEME: return mizu->say(SAY_CONSOLE_COLOURS);
    default: return "";
    }
}

static void row_value(int row, char* into, long size) {
    switch (row) {
    case ROW_LANGUAGE:
        koi_snprintf(into, size, "%s", mizu->language_name(mizu->language()));
        break;
    case ROW_VOLUME:
        koi_snprintf(into, size, "%d%%", volume_percent);
        break;
    case ROW_THEME:
        koi_snprintf(into, size, "%s", themes[theme_index].name);
        break;
    default:
        into[0] = 0;
        break;
    }
}

#define ROW_HEIGHT 34
#define ARROW_W 24

static void paint(WINDOW* self, int x, int y, int width, int height) {
    (void)self;
    (void)height;

    for (int row = 0; row < ROW_COUNT; row++) {
        int line = y + 10 + row * ROW_HEIGHT;
        char value[48];
        int value_x = x + width - 8 - ARROW_W;

        if (row == chosen_row)
            koi_gfx_fill(x + 2, line - 4, width - 4, ROW_HEIGHT - 4,
                         mizu->color(MIZU_COLOR_FACE));
        mizu->label(x + 10, line, row_name(row), mizu->color(MIZU_COLOR_TEXT));

        /* Two buttons and the value between them: the whole control is "less"
           and "more", which needs no explanation in any language. */
        mizu->raised(x + width - 8 - ARROW_W, line - 3, ARROW_W, 22);
        mizu->label(value_x + 8, line, ">", mizu->color(MIZU_COLOR_TEXT));
        mizu->raised(x + width - 8 - ARROW_W - 180, line - 3, ARROW_W, 22);
        mizu->label(x + width - 8 - ARROW_W - 180 + 8, line, "<",
                    mizu->color(MIZU_COLOR_TEXT));

        row_value(row, value, sizeof(value));
        mizu->label(x + width - 8 - ARROW_W - 150, line, value,
                    mizu->color(MIZU_COLOR_TEXT));
    }

    mizu->label(x + 10, y + 10 + ROW_COUNT * ROW_HEIGHT + 6,
                mizu->say(SAY_KEPT_AT_ONCE),
                mizu->color(MIZU_COLOR_SHADOW));
}

static void click(WINDOW* self, int x, int y, int clicks) {
    int row = (y - 6) / ROW_HEIGHT;
    int width;
    int ignored;

    (void)self;
    (void)clicks;
    if (row < 0 || row >= ROW_COUNT) return;
    chosen_row = row;
    mizu->window_client(window, &ignored, &ignored, &width, &ignored);

    if (x >= width - 8 - ARROW_W) step_row(row, 1);
    else if (x >= width - 8 - ARROW_W - 180 && x < width - 8 - ARROW_W - 156)
        step_row(row, -1);
    else mizu->repaint();
}

static void key(WINDOW* self, int pressed) {
    (void)self;
    switch (pressed) {
    case KOI_KEY_UP: if (chosen_row > 0) chosen_row--; break;
    case KOI_KEY_DOWN: if (chosen_row + 1 < ROW_COUNT) chosen_row++; break;
    case KOI_KEY_LEFT: step_row(chosen_row, -1); return;
    case KOI_KEY_RIGHT: step_row(chosen_row, 1); return;
    default: return;
    }
    mizu->repaint();
}

static void menu(WINDOW* self, int id) {
    (void)self;
    if (id == SETTINGS_CLOSE) {
        mizu->window_delete(window);
        window = (WINDOW*)0;
    }
}

static void closing(WINDOW* self) {
    if (self == window) window = (WINDOW*)0;
}

/* The language changed under us. The rows draw their names through say() every
   time, so they were already right; the window's title and its menu are the
   two places holding pointers taken once. */
static void relabel(void) {
    if (!window) return;
    strncpy(window->title, mizu->say(SAY_SETTINGS), WINDOW_TITLE_MAX - 1);
    window->menus[0] = (WINDOW_MENU){ mizu->say(SAY_MENU_FILE),
        { { mizu->say(SAY_CLOSE), SETTINGS_CLOSE } }, 1 };
}

static WINDOW* open(void) {
    if (window) {
        window->minimised = 0;
        mizu->window_raise(window);
        return window;
    }
    load_state();
    window = mizu->window_new(mizu->say(SAY_SETTINGS), 260, 180, 460, 200);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->click = click;
    window->key = key;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ mizu->say(SAY_MENU_FILE),
        { { mizu->say(SAY_CLOSE), SETTINGS_CLOSE } }, 1 };
    return window;
}

static MIZU_APP me = { "Settings", MIZU_APP_VERSION, open, menu, closing,
                       0, relabel };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < 5) return (MIZU_APP*)0;   /* 5 brought the language */
    mizu = api;
    return &me;
}
