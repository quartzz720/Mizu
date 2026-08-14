#include "mizu.h"

/* A test application: work that takes a while, done politely.
 *
 * This exists to be run, not to be useful. Cooperative multitasking is the
 * kind of thing that looks finished the moment it compiles and is not - so
 * there is an application here whose whole job is to sit in a loop for several
 * seconds and yield inside it. If the clock goes on ticking, the taskbar goes
 * on working and the windows can still be dragged while this is running, the
 * arrangement does what it says. If it does not, this says so in five seconds
 * rather than the day somebody writes a real application that does real work.
 *
 * It is not part of the MIZU package for the same reason: a test belongs where
 * it can be run, not where it can be installed by somebody who wanted a
 * desktop. build.sh builds it; publish.sh does not ship it.
 *
 * The impolite version is deliberately here too. Start without yielding and
 * everything stops until it finishes - which is the honest cost of this
 * arrangement, and worth being able to see rather than only read about.
 */

#define SPIN_POLITE 1
#define SPIN_RUDE 2
#define SPIN_CLOSE 3

#define SPIN_STEPS 200

static const MIZU_API* mizu;
static WINDOW* window;
static int progress;          /* 0 to SPIN_STEPS, or -1 for idle */
static int polite = 1;
static unsigned long long ticks;

static void paint(WINDOW* self, int x, int y, int width, int height) {
    char line[64];

    (void)self;
    (void)height;
    mizu->label(x + 8, y + 8,
                progress < 0 ? "Idle. File - Work politely, or rudely."
                             : (polite ? "Working, and yielding."
                                       : "Working, and not yielding."),
                mizu->color(MIZU_COLOR_TEXT));

    mizu->sunken(x + 8, y + 34, width - 16, 18);
    if (progress > 0) {
        int span = (width - 18) * progress / SPIN_STEPS;
        koi_gfx_fill(x + 9, y + 35, span, 16, mizu->color(MIZU_COLOR_ACCENT));
    }

    /* How many turns the desktop got while this was working. Zero is the
       whole story when the answer is meant to be two hundred. */
    koi_snprintf(line, sizeof(line), "turns given to the desktop: %u",
                 (unsigned int)ticks);
    mizu->label(x + 8, y + 60, line, mizu->color(MIZU_COLOR_TEXT));
}

/* Something slow, without asking the kernel to wait for us: koi_sleep would
   park the processor and prove nothing about who gets to run. */
static void a_moment_of_work(void) {
    volatile unsigned long long sum = 0;
    for (unsigned long long index = 0; index < 3000000ULL; index++) sum += index;
}

static void work(void) {
    ticks = 0;
    for (progress = 1; progress <= SPIN_STEPS; progress++) {
        a_moment_of_work();
        if (polite) {
            mizu->repaint();
            mizu->yield();
            ticks++;
        }
    }
    progress = -1;
    mizu->repaint();
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case SPIN_POLITE: polite = 1; work(); break;
    case SPIN_RUDE: polite = 0; work(); break;
    case SPIN_CLOSE:
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
    progress = -1;
    window = mizu->window_new("Spin", 360, 380, 380, 160);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "File",
        { { "Work politely", SPIN_POLITE }, { "Work rudely", SPIN_RUDE },
          { 0, 0 }, { "Close", SPIN_CLOSE } }, 4 };
    return window;
}

static MIZU_APP me = { "Spin", 1, open, menu, closing };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    /* 2 is where yield arrived, and this application is nothing without it. */
    if (!api || api->version < 2) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
