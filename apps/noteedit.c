#include "mizu.h"
#include "editcore.h"
#include "language.h"

/* NoteEdit, as an application.
 *
 * The first thing to leave mizu.c, and the reason it went first: it uses
 * nearly everything an application can use - a window with a menu strip, the
 * keyboard, drawing in the desktop's own colours, a file on disk and a
 * question asked in the desktop's own manner. If the interface in mizu.h were
 * short of anything, this is where it would show, which is the point of
 * porting something real before writing five applications against a contract
 * nobody has tried.
 *
 * The editing itself is the SDK's editcore - the same buffer the console
 * editor uses, linked into this file. That is not the same as linking window.c
 * would be: editcore holds only this application's own text, and a second copy
 * of it is a second document, which is exactly what it should be. window.c
 * holds the desktop, and a second copy of that would be a second desktop.
 *
 * The style is the whole document's, which is what Notepad did and for the
 * same reason: a style that varies inside the text needs a parallel buffer
 * saying where each run begins, and a plain text file has nowhere to keep it.
 */

#define NOTE_CAPACITY (64L * 1024L)
#define NOTE_PATH "\\NOTE.TXT"

/* The menu ids are this application's own. Mizu passes back whatever the
   window carried and does not interpret it - so these need agree with nobody,
   which is the difference between an application and a part of the shell. */
#define NOTE_SAVE 1
#define NOTE_CLOSE 2
#define NOTE_BOLD 3
#define NOTE_ITALIC 4
#define NOTE_UNDERLINE 5
#define NOTE_PLAIN 6
#define NOTE_COPY 7
#define NOTE_PASTE 8

static const MIZU_API* mizu;
static WINDOW* window;
static EDITOR note;
static int ready;
static int style;
static long top_line;

static void paint(WINDOW* self, int x, int y, int width, int height) {
    long total = edit_lines(&note);
    long caret_line = edit_line_of(&note, note.cursor);
    int rows = height / WINDOW_CHAR_H;
    int columns = width / WINDOW_CHAR_W;

    (void)self;
    if (rows < 1) rows = 1;

    /* Keep the caret in view before drawing anything, so the first frame after
       a keystroke already shows where it went. */
    if (caret_line < top_line) top_line = caret_line;
    if (caret_line >= top_line + rows) top_line = caret_line - rows + 1;
    if (top_line < 0) top_line = 0;

    for (int row = 0; row < rows && top_line + row < total; row++) {
        long number = top_line + row;
        long start = edit_line_start(&note, number);
        long length = edit_line_length(&note, number);
        char line[256];
        long copied = 0;

        while (copied < length && copied < columns && copied < 255) {
            char character = note.text[start + copied];
            line[copied] = (character == '\t') ? ' ' : character;
            copied++;
        }
        line[copied] = 0;
        mizu->label_styled(x + 2, y + row * WINDOW_CHAR_H, line,
                           mizu->color(MIZU_COLOR_TEXT), style);
    }

    {
        int row = (int)(caret_line - top_line);
        long column = edit_column_of(&note, note.cursor);
        if (row >= 0 && row < rows)
            koi_gfx_fill(x + 2 + (int)column * WINDOW_CHAR_W,
                         y + row * WINDOW_CHAR_H, 2, WINDOW_CHAR_H,
                         mizu->color(MIZU_COLOR_ACCENT));
    }
}

static void key(WINDOW* self, int pressed) {
    (void)self;
    switch (pressed) {
    case KOI_KEY_LEFT:  edit_move_by(&note, -1, 0); break;
    case KOI_KEY_RIGHT: edit_move_by(&note, 1, 0); break;
    case KOI_KEY_UP:    edit_move_lines(&note, -1, 0); break;
    case KOI_KEY_DOWN:  edit_move_lines(&note, 1, 0); break;
    case KOI_KEY_HOME:  edit_move_home(&note, 0); break;
    case KOI_KEY_END:   edit_move_end(&note, 0); break;
    case KOI_KEY_DELETE: edit_delete(&note); break;
    case '\b': edit_backspace(&note); break;
    case '\n': case '\r': edit_insert_char(&note, '\n'); break;
    case '\t': edit_insert(&note, "    ", 4); break;
    default:
        if (pressed >= ' ' && pressed < 0x100)
            edit_insert_char(&note, (char)pressed);
        break;
    }
    mizu->repaint();
}

static void save(void) {
    if (edit_save(&note, note.path))
        strcpy(window->title, "NoteEdit - NOTE.TXT");
    else
        mizu->message("NoteEdit", mizu->say(SAY_COULD_NOT_SAVE),
                      mizu->say(DIALOG_OK));
    mizu->repaint();
}

/* The clipboard is the kernel's, not the desktop's, which is why an
   application reaches it with an ordinary system call and why what is copied
   here can be pasted into Koi-Commander's editor - a program that knows
   nothing about Mizu. One clipboard for the machine is the whole point of
   putting it there. */
static void copy_all(void) {
    koi_clip_put(note.text, note.length);
}

static void paste(void) {
    static char held[4096];
    long got = koi_clip_get(held, sizeof(held) - 1);

    if (got <= 0) return;
    held[got] = 0;
    edit_insert(&note, held, got);
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case NOTE_COPY: copy_all(); break;
    case NOTE_PASTE: paste(); mizu->repaint(); break;
    case NOTE_SAVE: save(); break;
    case NOTE_CLOSE:
        mizu->window_delete(window);
        window = (WINDOW*)0;
        break;
    case NOTE_BOLD: style ^= KOI_TEXT_BOLD; mizu->repaint(); break;
    case NOTE_ITALIC: style ^= KOI_TEXT_ITALIC; mizu->repaint(); break;
    case NOTE_UNDERLINE: style ^= KOI_TEXT_UNDERLINE; mizu->repaint(); break;
    case NOTE_PLAIN: style = 0; mizu->repaint(); break;
    default: break;
    }
}

/* The window is going. Save first and without asking: a note is a scrap of
   paper, and a scrap of paper that asks a question when you put it down is
   not the thing it is imitating. */
static void closing(WINDOW* self) {
    if (self == window) {
        if (note.modified) edit_save(&note, note.path);
        window = (WINDOW*)0;
    }
}

static WINDOW* open(void);

/* Open on a particular file, which is what the browser hands over. The
   document is whatever was asked for rather than always NOTE.TXT - the
   notepad was written before anything could hand it a file. */
static WINDOW* open_with(const char* path) {
    if (!path || !path[0]) return open();

    if (window) {
        /* One document at a time, and the one on screen is saved before it is
           replaced: this is a notepad, and a notepad that loses what was on it
           because somebody double-clicked something else is not one. */
        if (note.modified) edit_save(&note, note.path);
    }
    if (!edit_load(&note, path, NOTE_CAPACITY)) return open();
    if (!note.path[0]) strcpy(note.path, path);
    ready = 1;
    top_line = 0;

    if (!window) {
        WINDOW* opened = open();
        if (!opened) return (WINDOW*)0;
    } else {
        window->minimised = 0;
        mizu->window_raise(window);
    }
    koi_snprintf(window->title, WINDOW_TITLE_MAX, "NoteEdit - %s", path);
    mizu->repaint();
    return window;
}

static WINDOW* open(void) {
    if (window) {
        window->minimised = 0;
        mizu->window_raise(window);
        return window;
    }
    if (!ready) {
        if (!edit_load(&note, NOTE_PATH, NOTE_CAPACITY) &&
            !edit_new(&note, NOTE_CAPACITY)) return (WINDOW*)0;
        if (!note.path[0]) strcpy(note.path, NOTE_PATH);
        ready = 1;
    }

    window = mizu->window_new("NoteEdit - NOTE.TXT", 300, 120, 520, 340);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->key = key;
    window->menu_count = 3;
    window->menus[0] = (WINDOW_MENU){ mizu->say(SAY_MENU_FILE),
        { { mizu->say(SAY_SAVE), NOTE_SAVE }, { 0, 0 },
          { mizu->say(SAY_CLOSE), NOTE_CLOSE } }, 3 };
    window->menus[2] = (WINDOW_MENU){ "Edit",
        { { "Copy all", NOTE_COPY }, { "Paste", NOTE_PASTE } }, 2 };
    window->menus[1] = (WINDOW_MENU){ mizu->say(SAY_MENU_FORMAT),
        { { mizu->say(SAY_BOLD), NOTE_BOLD },
          { mizu->say(SAY_ITALIC), NOTE_ITALIC },
          { mizu->say(SAY_UNDERLINE), NOTE_UNDERLINE }, { 0, 0 },
          { mizu->say(SAY_PLAIN), NOTE_PLAIN } }, 5 };
    return window;
}

/* Its menus are built from say(), so they hold the words of the language that
   was in force when the window opened. */
static void relabel(void) {
    if (!window) return;
    window->menus[0] = (WINDOW_MENU){ mizu->say(SAY_MENU_FILE),
        { { mizu->say(SAY_SAVE), NOTE_SAVE }, { 0, 0 },
          { mizu->say(SAY_CLOSE), NOTE_CLOSE } }, 3 };
    window->menus[1] = (WINDOW_MENU){ mizu->say(SAY_MENU_FORMAT),
        { { mizu->say(SAY_BOLD), NOTE_BOLD },
          { mizu->say(SAY_ITALIC), NOTE_ITALIC },
          { mizu->say(SAY_UNDERLINE), NOTE_UNDERLINE }, { 0, 0 },
          { mizu->say(SAY_PLAIN), NOTE_PLAIN } }, 5 };
}

static MIZU_APP me = { "NoteEdit", MIZU_APP_VERSION, open, menu, closing,
                       open_with, relabel };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < MIZU_API_VERSION) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
