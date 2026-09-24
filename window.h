#ifndef WINDOW_H
#define WINDOW_H

#include "koi.h"

/* Windows, for programs that draw.
 *
 * A desktop with a menu bar, windows that overlap and can be dragged, and a
 * bar along the bottom listing them. The shape is Windows 3.0's, with the one
 * addition 3.0 did not have and needed: overlapping windows get lost behind
 * each other, and something has to name the ones you cannot see.
 *
 * ---- What this is not, yet -----------------------------------------------
 *
 * The windows belong to one program. Koi-DOS holds one program in memory at a
 * time, at a fixed address, which is why running something from the file
 * manager goes through SYS_CHAIN and the screen visibly goes away. So a window
 * here is a part of the program that opened it, not another program.
 *
 * That is not a stopgap invented to hide a missing feature - it is what
 * Windows 1.0 through 3.0 in real mode actually were, and their bundled
 * applications were parts of one image for the same reason. Everything below
 * stays true when a second program can be resident; only who supplies the
 * paint callback changes.
 *
 * ---- How a program uses it ------------------------------------------------
 *
 *     window_open_desktop("Mizu-DOS");
 *     WINDOW* clock = window_new("Clock", 40, 40, 300, 200);
 *     clock->paint = paint_clock;
 *     while (window_next(&event)) { ... }
 *     window_close_desktop();
 *
 * A window's `paint` is handed the client area in screen coordinates and draws
 * whatever it likes into it. The library owns the frame, the title, the
 * ordering and the pointer; a window owns its contents and nothing else. That
 * split is the whole reason for a library rather than a pile of helpers.
 */

#define WINDOW_MAX 12
#define WINDOW_TITLE_MAX 48
#define WINDOW_MENU_MAX 8
#define WINDOW_ITEM_MAX 12

/* One entry of a drop-down. `id` comes back in the event; a zero label is a
   separator, which is a line and cannot be chosen. */
typedef struct {
    const char* label;
    int id;
} WINDOW_ITEM;

typedef struct {
    const char* label;
    WINDOW_ITEM items[WINDOW_ITEM_MAX];
    int count;
} WINDOW_MENU;

typedef struct WINDOW WINDOW;

struct WINDOW {
    char title[WINDOW_TITLE_MAX];
    int x, y, width, height;      /* the frame, not the client area */
    int minimised;
    int used;
    /* Smaller than this and the frame has eaten the contents. A window that
       can be dragged to nothing is a window somebody loses. */
    int minimum_width;
    int minimum_height;

    /* The menu strip inside this window, under its title. Zero menus means no
       strip and the client area starts higher. */
    WINDOW_MENU menus[WINDOW_MENU_MAX];
    int menu_count;

    /* Contents. `paint` is given the client rectangle in screen coordinates;
       `click` is given a point inside it, relative to its top left. */
    void (*paint)(WINDOW* window, int x, int y, int width, int height);
    void (*click)(WINDOW* window, int x, int y, int clicks);
    void (*key)(WINDOW* window, int key);
    /* Milliseconds between repaints, or 0 for a window that only changes when
     * something happens to it.
     *
     * Most windows are the second kind and redrawing them on a timer would be
     * a screenful of work for nothing. A clock and a progress bar are the
     * first kind: nothing happens to them at all, and they were both correct
     * and both frozen until this existed. */
    int repaint_ms;
    /* Called when that interval has passed, before the repaint, whether or
     * not this window is in front. `repaint_ms` alone only ever redrew what
     * was already there; a window with something to *advance* on a timer - a
     * track playing, a transfer, an animation - had nowhere to do it and got
     * its turn only when somebody happened to touch the machine. */
    void (*tick)(WINDOW* window);
    void* data;

    /* The library's own. Do not set these: `ticked` is when this window last
       had its turn, and `busy` says one of its handlers is running - which is
       how a handler that yields does not get handed a second keystroke while
       it is still inside the first. */
    koi_uint64 ticked;
    int busy;
    int needs_paint;
    /* The right button, in the window's own coordinates.
     *
     * Separate from `click` rather than a button number added to it, because
     * every application already implements `click` and none of them would
     * have been checking a parameter that did not exist. A window that wants
     * a context menu says so by setting this; one that does not is one where
     * the right button does nothing, which is the correct behaviour and comes
     * for free. */
    void (*context)(WINDOW* window, int x, int y);

    /* The pointer moving with the left button held, after a press that landed
     * in this window's contents.
     *
     * Separate from `click` for the same reason `context` is: a window that
     * wants it says so, and one that does not is one where dragging does
     * nothing - which is right for a list of files and wrong only for the
     * things that select. The coordinates are the window's own, and they are
     * not clamped to it: a hand that selects text usually leaves the window
     * on the way, and stopping at the edge would stop the selection with it. */
    void (*drag)(WINDOW* window, int x, int y);
};

#define WINDOW_EVENT_NONE 0
#define WINDOW_EVENT_MENU 1        /* a menu item was chosen */
#define WINDOW_EVENT_CLOSE 2       /* a window's close box */
#define WINDOW_EVENT_QUIT 3        /* the desktop was asked to end */
#define WINDOW_EVENT_KEY 4         /* a key nothing else wanted */
#define WINDOW_EVENT_LAUNCHER 5    /* the taskbar button was pressed */
/* The desktop itself was clicked - the wallpaper, with no window under the
 * pointer. `x` and `y` say where, `id` how many clicks, and `button` which
 * one. What is on the desktop is the desktop's business rather than this
 * library's: it draws no icons and knows of none, and this is how whoever
 * does hears about a click on them. */
#define WINDOW_EVENT_DESKTOP 6

typedef struct {
    int type;
    WINDOW* window;                /* which one, when it is about a window */
    int id;                        /* the menu item, the key, or the clicks */
    int x, y;                      /* where, for a click on the desktop */
    int button;                    /* WINDOW_BUTTON_LEFT or _RIGHT */
} WINDOW_EVENT;

#define WINDOW_BUTTON_LEFT 0
#define WINDOW_BUTTON_RIGHT 1

/* Take the screen. Returns 0 when it could not be had. */
int window_open_desktop(const char* title);
void window_close_desktop(void);

/* Take the screen again after something else has had it, keeping every window
   where it was. Returns 0 if it could not be had back. */
int window_reopen_desktop(void);

/* The desktop's own menu bar, across the top. */
void window_desktop_menu(const WINDOW_MENU* menus, int count);

/* One button at the left of the taskbar, or NULL for none.
 *
 * Deliberately not called a Start button. This library draws a bar and knows
 * the order things are stacked in; it has no business knowing what a shell
 * puts on it. Pressing it produces WINDOW_EVENT_LAUNCHER and nothing else
 * happens - what the button means is the caller's, and Mizu is the caller that
 * makes it a Start menu.
 *
 * `pressed` draws it held down, for as long as the menu it opened is up. */
void window_launcher(const char* label);
void window_launcher_pressed(int pressed);

/* A menu that appears where it is told and above everything.
 *
 * Here rather than in the caller because only this file knows what is drawn
 * over what, and because the pointer has to be taken off the screen and put
 * back around it. Blocks until something is chosen or the pointer is clicked
 * away; returns the chosen item's `id`, or -1.
 *
 * `x` and `y` are the bottom-left corner, because the thing that opens one is
 * on the taskbar and a menu that grew downwards from there would grow off the
 * screen. */
int window_popup(const WINDOW_ITEM* items, int count, int x, int y);

/* The same menu, opening downwards from the point given rather than upwards -
 * which is what a menu the right button opened does, at the pointer.
 *
 * It flips back upwards when there is no room below, because a menu running
 * off the bottom of the screen is a menu with entries nobody can reach. Either
 * button works it, so the press that opened it can be held, slid and released
 * onto an entry the way the Start menu already allows. */
int window_context(const WINDOW_ITEM* items, int count, int x, int y);

/* A question in the middle of the screen, with the rest of it dimmed.
 *
 * Blocks until it is answered; returns 1 for `accept`, 0 for `cancel`, and 0
 * for Escape - a question waved away should give the answer that does
 * nothing. `accept_by_default` says which button the keyboard starts on, and
 * for anything irreversible that is the other one.
 *
 * The dimming is what makes it a question rather than one more window: see
 * the note in window.c. */
int window_confirm(const char* title, const char* message, const char* accept,
                   const char* cancel, int accept_by_default);

/* The same box with one button: something to be told, not agreed to. A notice
   wearing a Cancel button asks a question it has no answer for. */
int window_message(const char* title, const char* message, const char* accept);

/* The same box with a line to type in. `buffer` is both what it starts with
 * and where the answer lands, and it must hold `size` bytes. Returns 1 when
 * the accepting button was chosen and something was typed, 0 otherwise - so a
 * caller can act on the answer without also checking whether it is empty.
 *
 * The labels are the caller's because this file has no language table and
 * should not grow one: it knows how to draw a button, not what to call it. */
int window_prompt(const char* title, const char* message, const char* accept,
                  const char* cancel, char* buffer, int size);

/* Where a menu opened from the taskbar button belongs. The caller does not
   know how tall the bar is or where the button sits, and should not have to
   ask twice for something this file already decided. */
void window_launcher_anchor(int* x, int* y);

WINDOW* window_new(const char* title, int x, int y, int width, int height);
void window_delete(WINDOW* window);
void window_raise(WINDOW* window);

/* Lay every open window out side by side, filling the desktop. What "tile"
   meant in the system this borrows its shape from - not "put three windows
   back where they started", which is what it did first and is a different
   verb. */
void window_tile(void);
WINDOW* window_active(void);

/* Where a window's contents live, in screen coordinates. */
void window_client(const WINDOW* window, int* x, int* y, int* width,
                   int* height);

/* Wait for something to happen and describe it. Returns 0 when the desktop is
   finished - which is the loop's condition, so the caller never has to test
   for quit twice. */
int window_next(WINDOW_EVENT* event);

/* Run one pass of the desktop's loop and come back.
 *
 * For an application in the middle of something long: the clock goes on
 * ticking, windows go on being draggable, and anything meant for the desktop
 * is put by for the next window_next rather than lost.
 *
 * Cooperative, and the word is the whole bargain: nothing takes the processor
 * away from anybody. An application that never yields freezes the desktop, and
 * that is the trade until there is memory protection to make preemption safe.
 *
 * Meant to be called from inside a handler - a menu choice that starts a long
 * piece of work is the case it exists for. A window already inside one of its
 * own handlers is not handed anything new while it is there, so an editor
 * cannot find itself in two of its own keystrokes at once. */
void window_yield(void);

/* End the loop. The next window_next returns 0. */
void window_quit(void);

/* Draw something of your own on the desktop, every time it is repainted.
 *
 * Called after the wallpaper and before any window, so what it draws is
 * behind everything and in front of nothing. Icons are the reason it exists;
 * this library has no idea what an icon means and should not acquire one.
 * Pass null to stop. */
void window_desktop_paint(void (*paint)(void));

/* Ask for everything to be drawn again. Called for you when a window moves or
   the order changes; call it yourself when a window's contents change. */
void window_repaint(void);

/* The palette, so a window's contents match its frame. Water, because that is
   what Mizu means. */
extern koi_uint32 window_face;
extern koi_uint32 window_light;
extern koi_uint32 window_shadow;
extern koi_uint32 window_text;
extern koi_uint32 window_client_paper;
extern koi_uint32 window_title_active;
extern koi_uint32 window_title_idle;
extern koi_uint32 window_accent;

/* Drawing helpers a window's contents will want anyway, in the same style as
   the frame: a raised or sunken rectangle, and text. */
void window_raised(int x, int y, int width, int height);
void window_sunken(int x, int y, int width, int height);
void window_label(int x, int y, const char* text, koi_uint32 color);
/* The same with KOI_TEXT_BOLD / ITALIC / UNDERLINE. */
void window_label_styled(int x, int y, const char* text, koi_uint32 color,
                         int style);

/* ---- Icons ---------------------------------------------------------------
 *
 * A picture with one colour treated as "not there": magenta, 255,0,255, the
 * way every 16-colour system did it and for the same reason - a BMP has no
 * alpha channel, and one colour nobody would draw with costs nothing to
 * reserve. A pixel of exactly that colour is skipped; every other pixel is
 * drawn as it is.
 *
 * Loaded once and kept. An icon is at most 32x32, which is four kilobytes,
 * and the alternative is reading a file every time a window is repainted.
 *
 * Named by the file rather than by what it means, and the name is looked for
 * beside the program: `window_icon("FILES32.BMP")`. Names are what the
 * drawings are called, so the code and the ICONS.md that the drawings were
 * made from say the same word.
 *
 * Returns a handle to draw with, or 0 when there is no such file - which is
 * not an error worth stopping for. A desktop whose icons have not been
 * installed should be a desktop with no pictures on it, not a desktop that
 * refuses to start. */
int window_icon(const char* name);

/* Draw one, top-left at x,y. A handle of 0 draws nothing, so a caller may
   pass the result of window_icon() straight in without testing it. */
void window_icon_draw(int icon, int x, int y);

/* How wide and tall that icon is, or 0. For laying a row of them out without
   assuming the size the file turned out to be. */
int window_icon_width(int icon);
int window_icon_height(int icon);

#define WINDOW_ICON_MAX_SIZE 32

/* ---- The scrollbar -------------------------------------------------------
 *
 * For a window whose contents are longer than it is. The caller counts in
 * whatever its items are - lines, files, wrapped rows - and says how many
 * there are, how many fit and which is first; this draws the bar and answers
 * where a click or a drag should move to.
 *
 * `window_scrollbar_press` is for a press: the arrows step by one, the trough
 * above or below the thumb pages, and a press on the thumb itself changes
 * nothing and waits for the drag. `window_scrollbar_drag` is for the pointer
 * moving with the button down, and puts the thumb under it.
 *
 * All three take the bar's own y and height, so an application that puts it
 * beside a toolbar does not have to tell three functions the same arithmetic
 * twice. */
#define WINDOW_SCROLLBAR_W 16

void window_scrollbar(int x, int y, int height, int top, int visible,
                      int total);
int window_scrollbar_press(int y, int height, int top, int visible, int total,
                           int point_y);
int window_scrollbar_drag(int y, int height, int visible, int total,
                          int point_y);

#define WINDOW_CHAR_W 8
#define WINDOW_CHAR_H 16
#define WINDOW_TITLE_H 22
#define WINDOW_MENU_H 20
#define WINDOW_BORDER 3
#define WINDOW_TASKBAR_H 28
#define WINDOW_TOPBAR_H 22

#endif
