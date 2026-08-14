#ifndef MIZU_H
#define MIZU_H

#include "koi.h"
#include "window.h"

/* The contract between Mizu and an application.
 *
 * ---- What an application is -----------------------------------------------
 *
 * A file that Mizu loads into memory and calls. Not a process: there is no
 * scheduler, no address space of its own and no protection between the two -
 * an application is code Mizu runs, the way a DLL was code Windows ran and a
 * VxD was code the virtual machine manager ran. Both were built on exactly
 * this: a table of function pointers handed over at load time, and a small
 * structure handed back.
 *
 * That is a smaller claim than "multitasking" and it is deliberately the whole
 * of it. What it buys is the thing that was actually missing: two applications
 * open at once, with the desktop alive underneath them, and applications that
 * ship separately from the desktop they run on.
 *
 * ---- How one is written ---------------------------------------------------
 *
 *     #include "mizu.h"
 *
 *     static const MIZU_API* mizu;
 *
 *     static WINDOW* open(void) {
 *         WINDOW* window = mizu->window_new("Clock", 40, 40, 300, 200);
 *         if (window) window->paint = paint;
 *         return window;
 *     }
 *
 *     static MIZU_APP me = { "Clock", 1, open, 0, 0 };
 *
 *     MIZU_APPLICATION(start)
 *     static MIZU_APP* start(const MIZU_API* api) { mizu = api; return &me; }
 *
 * and `koicc -m clock.c` builds CLOCK.APP.
 *
 * ---- Why the table, and not simply calling window_new ----------------------
 *
 * Because there would be two of it. window.c holds the windows, the stacking
 * order, the pointer and the theme in its own variables; an application that
 * linked its own copy would have a second desktop, invisible and empty, and
 * would draw into it. The table is how the application reaches the one that
 * exists. Everything that is *not* held anywhere - the drawing calls, files,
 * sound, the keyboard - stays an ordinary system call, and an application
 * makes those directly.
 */

/* 2 adds `yield`, 3 adds `run`, 4 adds `open_with` to MIZU_APP and
   `open_file` to the table, 5 adds the language. An application that needs one
   of them says so by refusing anything older, which is what the version is
   for. */
#define MIZU_API_VERSION 5

/* Which colour, since the theme lives in window.c's own variables and an
   application cannot see them. Asked for by name so that a theme that changes
   while the machine is running changes what an application draws too. */
enum {
    MIZU_COLOR_FACE, MIZU_COLOR_LIGHT, MIZU_COLOR_SHADOW, MIZU_COLOR_TEXT,
    MIZU_COLOR_PAPER, MIZU_COLOR_ACCENT
};

typedef struct {
    /* MIZU_API_VERSION as Mizu was built with. An application that needs more
       than it finds here should say so and refuse, rather than call a pointer
       that is not there. */
    unsigned int version;

    /* Windows. The application fills in the callbacks on what it gets back -
       paint, key, click - and Mizu calls them from its own loop. */
    WINDOW* (*window_new)(const char* title, int x, int y, int width,
                          int height);
    void (*window_delete)(WINDOW* window);
    void (*window_raise)(WINDOW* window);
    void (*window_client)(const WINDOW* window, int* x, int* y, int* width,
                          int* height);
    void (*repaint)(void);

    /* Drawing that has to match the desktop it happens on: the frame style and
       the theme are the library's, and an application that drew its own would
       be an application that looks wrong the day the theme changes. */
    void (*label)(int x, int y, const char* text, koi_uint32 color);
    void (*label_styled)(int x, int y, const char* text, koi_uint32 color,
                         int style);
    void (*raised)(int x, int y, int width, int height);
    void (*sunken)(int x, int y, int width, int height);
    koi_uint32 (*color)(int which);

    /* Asking something, in the desktop's own manner and language. */
    int (*confirm)(const char* title, const char* message, const char* accept,
                   const char* cancel, int accept_by_default);
    int (*message)(const char* title, const char* message, const char* accept);
    int (*prompt)(const char* title, const char* message, const char* accept,
                  const char* cancel, char* buffer, int size);

    /* The phrase table, so an application speaks the language the machine is
       set to without carrying its own copy of the settings. */
    const char* (*say)(int phrase);

    /* Let the desktop have a turn in the middle of something long.
     *
     * Cooperative, and that is the whole bargain: nothing takes the processor
     * away from an application, so one that never calls this freezes
     * everything - and one that calls it in a loop keeps the clock ticking
     * and the windows draggable while it works. Windows 3.0 lived on exactly
     * this arrangement for five years.
     *
     * Do not call it from inside a handler the desktop just called: it does
     * nothing there on purpose. A pass inside a pass is where cooperative
     * systems earn their reputation. */
    void (*yield)(void);

    /* Run a Koi-DOS command line and come back when it has finished.
     *
     * The desktop goes away while it runs and is drawn again afterwards,
     * because Koi-DOS holds one program in memory at a time - the caller is
     * stopped inside this call. It is what Windows 3.0 did with a DOS
     * program, and it is honest about it rather than pretending.
     *
     * Here rather than koi_run directly because the screen has to be given up
     * and taken back around it, and only the desktop knows how. Returns the
     * command's exit code, or KOI_EXIT_NOT_FOUND when there was no such
     * command. */
    int (*run)(const char* command);

    /* Hand a file to whatever opens that kind of file, and let the desktop
     * decide which application that is.
     *
     * The browser needs this and is the reason it exists: without it, opening
     * a picture meant running a console program that takes the whole screen,
     * which is what a system does when its applications cannot talk to each
     * other. Returns 1 when something took it. */
    int (*open_file)(const char* path);

    /* The language the machine is set to, and how to change it.
     *
     * Here rather than in the application because language.c keeps the
     * current language in a variable of its own: an application that linked
     * its own copy would change its idea of it and leave the desktop still
     * drawing the old words. Setting it through this relabels everything that
     * is already on screen. */
    int (*language)(void);
    int (*language_count)(void);
    const char* (*language_name)(int which);
    void (*language_set)(int which);
} MIZU_API;

/* What an application hands back.
 *
 * `open` is called when somebody asks for it and should return the window it
 * opened, or raise the one it already has and return that. `menu` and
 * `closing` arrive for windows this application opened, and only for those -
 * Mizu knows which window belongs to whom because `open` told it. */
/* `version` is this structure's, not the application's: it says how many of
 * the fields below are really there. An application built against an older
 * mizu.h has a shorter structure, and a desktop that read past the end of it
 * would be calling whatever happened to follow in memory - which is the one
 * way this arrangement can hurt somebody who did nothing wrong.
 *
 *   1 - name, open, menu, closing
 *   2 - open_with
 *   3 - relabel
 */
#define MIZU_APP_VERSION 3

typedef struct {
    const char* name;
    unsigned int version;
    WINDOW* (*open)(void);
    void (*menu)(WINDOW* window, int id);
    void (*closing)(WINDOW* window);
    /* Open, on a particular file. An application that has no use for one
       leaves this null and is simply never asked. */
    WINDOW* (*open_with)(const char* path);
    /* The language changed: build your labels again. Anything an application
       took from say() is a pointer into the table for the language that was
       in force at the time, so its menus go on saying the old words however
       carefully the desktop repaints. */
    void (*relabel)(void);
} MIZU_APP;

/* The entry point, spelled once so that an application does not have to know
   how the loader calls it. */
#define MIZU_APPLICATION(function)                                       \
    static MIZU_APP* function(const MIZU_API* api);                      \
    void* module_main(void* argument);                                   \
    void* module_main(void* argument) {                                  \
        return (void*)function((const MIZU_API*)argument);               \
    }

#endif
