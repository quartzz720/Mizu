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
#define NOTE_OPEN 9
#define NOTE_CUT 10

static const MIZU_API* mizu;
static WINDOW* window;
static EDITOR note;
static int ready;
static int style;
static long top_line;

/* What the last paint put on screen, for the scrollbar's sake: a click on the
   bar has to know how many rows were showing and how many lines there are, and
   the paint is where both are worked out. */
static int view_rows = 1;
static long view_total = 1;

static void paint(WINDOW* self, int x, int y, int width, int height) {
    long total = edit_lines(&note);
    long caret_line = edit_line_of(&note, note.cursor);
    int rows = height / WINDOW_CHAR_H;
    int columns = (width - WINDOW_SCROLLBAR_W) / WINDOW_CHAR_W;

    (void)self;
    if (rows < 1) rows = 1;
    if (columns < 8) columns = 8;
    view_rows = rows;
    view_total = total;

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

        /* The selected part of this line, behind the text.
         *
         * Drawn as a block rather than by colouring the letters, because that
         * is what a selection has looked like since text had a background,
         * and because the library draws a string in one colour. */
        if (edit_has_selection(&note)) {
            long from, to;

            edit_selection(&note, &from, &to);
            if (to > start && from < start + length + 1) {
                long left = from > start ? from - start : 0;
                long right = to < start + length ? to - start : length;

                if (right > columns) right = columns;
                if (left < right)
                    koi_gfx_fill(x + 2 + (int)left * WINDOW_CHAR_W,
                                 y + row * WINDOW_CHAR_H,
                                 (int)(right - left) * WINDOW_CHAR_W,
                                 WINDOW_CHAR_H,
                                 mizu->color(MIZU_COLOR_ACCENT));
            }
        }

        mizu->label_styled(x + 2, y + row * WINDOW_CHAR_H, line,
                           mizu->color(MIZU_COLOR_TEXT), style);
    }

    {
        int row = (int)(caret_line - top_line);
        long column = edit_column_of(&note, note.cursor);
        if (row >= 0 && row < rows && column < columns)
            koi_gfx_fill(x + 2 + (int)column * WINDOW_CHAR_W,
                         y + row * WINDOW_CHAR_H, 2, WINDOW_CHAR_H,
                         mizu->color(MIZU_COLOR_ACCENT));
    }

    mizu->scrollbar(x + width - WINDOW_SCROLLBAR_W, y, rows * WINDOW_CHAR_H,
                    (int)top_line, rows, (int)total);
}

/* Whether a point is on the bar rather than in the text. The bar is the last
   sixteen pixels of the window, and a click there is never a caret. */
static int on_scrollbar(WINDOW* self, int x) {
    int client_x, client_y, client_w, client_h;

    mizu->window_client(self, &client_x, &client_y, &client_w, &client_h);
    return x >= client_w - WINDOW_SCROLLBAR_W;
}

static void copy_all(void);
static void paste(void);
static void cut_all(void);
static WINDOW* open_with(const char* path);

/* Where in the text a point in the window is.
 *
 * Row and column from the pixel, then the offset of that column in that line -
 * clamped to the line's length, because pointing past the end of a short line
 * means the end of it and not the middle of the next one. */
static long offset_at(int x, int y) {
    long line = top_line + y / WINDOW_CHAR_H;
    long column = x / WINDOW_CHAR_W;
    long total = edit_lines(&note);
    long start;
    long length;

    if (line < 0) line = 0;
    if (line >= total) line = total ? total - 1 : 0;
    start = edit_line_start(&note, line);
    length = edit_line_length(&note, line);
    if (column < 0) column = 0;
    if (column > length) column = length;
    return start + column;
}

/* Click puts the caret there and drops whatever was selected. */
static void click(WINDOW* self, int x, int y, int clicks) {
    (void)clicks;
    if (on_scrollbar(self, x)) {
        top_line = mizu->scrollbar_press(0, view_rows * WINDOW_CHAR_H,
                                         (int)top_line, view_rows,
                                         (int)view_total, y);
        mizu->repaint();
        return;
    }
    edit_move_to(&note, offset_at(x, y), 0);
    mizu->repaint();
}

/* And dragging from it selects: the same move, extending rather than
   dropping, which is what `extend` in the editor has meant all along and what
   nothing was calling. */
static void drag(WINDOW* self, int x, int y) {
    if (on_scrollbar(self, x)) {
        top_line = mizu->scrollbar_drag(0, view_rows * WINDOW_CHAR_H, view_rows,
                                        (int)view_total, y);
        mizu->repaint();
        return;
    }
    edit_move_to(&note, offset_at(x, y), 1);
    mizu->repaint();
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
    /* The three everybody's fingers already know. Ctrl+C arrives as an
       ordinary character here because the desktop asked the kernel to stop
       treating it as "stop this program" - see koi_break in mizu.c. */
    case 3: copy_all(); break;
    case 22: paste(); break;
    case 24: cut_all(); break;
    default:
        if (pressed >= ' ' && pressed < 0x100)
            edit_insert_char(&note, (char)pressed);
        break;
    }
    mizu->repaint();
}

static void save(void) {
    /* The title says which file, and it used to say NOTE.TXT whatever had
       been saved: the notepad was written when there was only one document
       and the name was a constant. */
    if (edit_save(&note, note.path))
        koi_snprintf(window->title, WINDOW_TITLE_MAX, "NoteEdit - %s",
                     note.path[0] ? note.path : NOTE_PATH);
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
/* What is selected, or the whole thing when nothing is.
 *
 * "Copy all" was honest while there was no way to select anything. There is
 * now, and a Copy that ignored a selection somebody had just made with the
 * pointer would be the wrong kind of honest. */
static void copy_all(void) {
    if (edit_has_selection(&note)) {
        long from, to;

        edit_selection(&note, &from, &to);
        if (to > from) koi_clip_put(note.text + from, to - from);
        return;
    }
    koi_clip_put(note.text, note.length);
}

static void paste(void) {
    static char held[4096];
    long got = koi_clip_get(held, sizeof(held) - 1);

    if (got <= 0) return;
    held[got] = 0;
    edit_insert(&note, held, got);
}

/* Open something else. A notepad that can only ever hold one file is a
   notepad that was written before anything could hand it one - and asking for
   a name is the smallest thing that fixes it until there is a file dialogue
   worth the name. The browser hands files over the other way, through
   open_with, and both end in the same place. */
static void open_another(void) {
    char path[EDIT_PATH_MAX];
    int at = 0;

    while (note.path[at] && at + 1 < (int)sizeof(path)) { path[at] = note.path[at]; at++; }
    path[at] = 0;
    if (!mizu->prompt("NoteEdit", mizu->say(SAY_MENU_FILE), mizu->say(DIALOG_OK),
                      mizu->say(DIALOG_CANCEL), path, (int)sizeof(path)))
        return;
    if (note.modified) edit_save(&note, note.path);
    open_with(path);
}

static void cut_all(void) {
    /* A selection is copied and then removed, which is what cut means. Only
       when there is none does this fall back to emptying the whole thing -
       the behaviour from before there was selecting, kept because the menu
       entry says "Cut all". */
    if (edit_has_selection(&note)) {
        copy_all();
        edit_backspace(&note);   /* removes the selection, says editcore */
        mizu->repaint();
        return;
    }
    copy_all();
    edit_close(&note);
    if (!edit_new(&note, NOTE_CAPACITY)) ready = 0;
    else strcpy(note.path, NOTE_PATH);
    mizu->repaint();
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case NOTE_OPEN: open_another(); break;
    case NOTE_CUT: cut_all(); break;
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
    if (!edit_load(&note, path, NOTE_CAPACITY)) {
        /* The document that was already open is still the document. Loading
           failed - the file is bigger than this notepad holds - and replacing
           what somebody was working on with nothing would be the worse of the
           two outcomes by a long way. */
        WINDOW* existing = open();

        mizu->message("NoteEdit",
                      "That file is larger than the notepad can hold.",
                      mizu->say(DIALOG_OK));
        mizu->repaint();
        return existing;
    }
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
    window->click = click;
    window->drag = drag;
    window->menu_count = 3;
    window->menus[0] = (WINDOW_MENU){ mizu->say(SAY_MENU_FILE),
        { { "Open...", NOTE_OPEN }, { mizu->say(SAY_SAVE), NOTE_SAVE },
          { 0, 0 }, { mizu->say(SAY_CLOSE), NOTE_CLOSE } }, 4 };
    window->menus[2] = (WINDOW_MENU){ "Edit",
        { { "Cut all", NOTE_CUT }, { "Copy all", NOTE_COPY },
          { "Paste", NOTE_PASTE } }, 3 };
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
        { { "Open...", NOTE_OPEN }, { mizu->say(SAY_SAVE), NOTE_SAVE },
          { 0, 0 }, { mizu->say(SAY_CLOSE), NOTE_CLOSE } }, 4 };
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
