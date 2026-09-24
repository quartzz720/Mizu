#include "koi.h"
#include "window.h"
#include "mizu.h"
#include "wav.h"
#include "settings.h"
#include "language.h"
#include "wav.h"

/* Mizu 0.5 - the desktop.
 *
 * The name was the file manager's while that was the only graphical thing
 * here. The file manager is finished and is called Koi-Commander now; this is
 * what the name was being kept for.
 *
 * Windows 3.0's shape, water's colours. What it is not yet is honest to say
 * plainly: the windows here are parts of this program, because Koi-DOS holds
 * one program in memory at a time. Windows 1.0 through 3.0 in real mode were
 * exactly that too, and their bundled applications were parts of one image for
 * the same reason. When a second program can be resident, the frames and the
 * ordering below do not change - only who supplies the paint.
 */

#define MENU_ABOUT 1
#define MENU_EXIT 2
#define MENU_CONTROL 3
#define MENU_CLOCK 4
/* 5 was Koi-Commander, and it is not this desktop's to list: it is a Koi-DOS
   program that arrives from dosget, and every installed package is already in
   the Start menu. Left as a hole rather than reused - these ids travel out
   through window.c and back. */
#define MENU_IMAGE 5
#define MENU_NAMI 18
#define MENU_TILE 6
#define MENU_NOTE 7
/* 8 to 12 were NoteEdit's - Save, Bold, Italic, Underline, Plain - and are
   the application's own now. The numbers are left as a hole rather than
   reused, because these ids travel through window.c and back, and a stale one
   arriving at a number that has changed meaning is the one bug this costs
   nothing to make impossible. */
#define MENU_PLAYER 13
#define MENU_FILES 16
#define MENU_TERM 17
/* 14 and 15 were the player's Stop and Pause, and are its own now. Left as a
   hole rather than reused: these ids travel out through window.c and back. */

/* The applications this desktop ships with. A name, not a path: it is loaded
   from beside this program, wherever the package was installed. */
#define NOTEEDIT "NOTEEDIT.APP"
#define PLAYER "PLAYER.APP"
#define FILES "FILES.APP"
#define TERM "TERM.APP"
#define IMAGE "IMAGE.APP"
#define CONTROL "CONTROL.APP"
#define NAMI "NAMI.APP"

static WINDOW* control_window;
static WINDOW* clock_window;
static WINDOW* about_window;

static int current_volume_index(void) {
    long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);

    for (long index = 0; index < count; index++)
        if (koi_sysinfo(KOI_INFO_VOLUME_IS_CURRENT, index) == 1)
            return (int)index;
    return -1;
}


/* ---- The control panel ---------------------------------------------------
 *
 * Program Manager's grid, which is the right shape for a machine with no
 * overlapping-window habits yet: a page of things you can start, each one a
 * picture and a word.
 */
typedef struct {
    const char* name;
    const char* icon;          /* the drawing in ICONS\\, or null for none */
    koi_uint32 (*tint)(void);  /* what to draw instead, when it is missing */
    void (*open)(void);
} ENTRY;

static koi_uint32 tint_setup(void) { return koi_gfx_color(0x4A, 0x8F, 0xB8); }
static koi_uint32 tint_files(void) { return koi_gfx_color(0x35, 0xA6, 0xC4); }
static koi_uint32 tint_tools(void) { return koi_gfx_color(0x58, 0xB0, 0xA8); }

static void open_application(const char* file);
static void open_clock(void);
static void open_about(void);
static int run_command(const char* command);

static void open_files(void) { open_application(FILES); }
static void open_terminal(void) { open_application(TERM); }
static void open_notepad(void) { open_application(NOTEEDIT); }
static void open_picture(void) { open_application(IMAGE); }
static void open_music(void) { open_application(PLAYER); }
static void open_settings(void) { open_application(CONTROL); }

/* Eight things, and eight drawings for them.
 *
 * Koi-Commander used to be the second of these and is not here any more: it is
 * a Koi-DOS program that exists on its own, arrives from dosget, and putting
 * it in this desktop's panel claimed it as a part of the desktop. It is still
 * in the Start menu with every other installed package, which is where a
 * program that is not ours belongs.
 *
 * The names of the drawings are tied to the place rather than to the
 * application, on purpose: the same application can stand in two places with
 * different pictures, and a file called after the slot it fills is one anybody
 * can replace without reading code. */
static ENTRY entries[8];

static void name_entries(void) {
    entries[0] = (ENTRY){ say(SAY_FILES), "FILES32.BMP", tint_files, open_files };
    entries[1] = (ENTRY){ "Koi-DOS", "TERM32.BMP", tint_files, open_terminal };
    entries[2] = (ENTRY){ say(SAY_NOTEEDIT), "NOTE32.BMP", tint_tools, open_notepad };
    entries[3] = (ENTRY){ "Picture", "IMAGE32.BMP", tint_tools, open_picture };
    entries[4] = (ENTRY){ "Player", "PLAYER32.BMP", tint_setup, open_music };
    entries[5] = (ENTRY){ say(SAY_CLOCK), "CLOCK32.BMP", tint_setup, open_clock };
    entries[6] = (ENTRY){ say(SAY_SETTINGS), "SETUP32.BMP", tint_setup, open_settings };
    entries[7] = (ENTRY){ say(SAY_ABOUT), "ABOUT32.BMP", tint_tools, open_about };
}
#define ENTRY_COUNT 8

#define ICON_W 120
#define ICON_H 96

/* The icon of an entry, or a tile standing in for it.
 *
 * The drawings are files in the package and can be absent - somebody
 * installed an older Mizu, or removed them, or is running the desktop out of
 * a build directory. Standing in for a missing one costs three rectangles and
 * keeps the panel a panel; refusing to draw the row would be a system that
 * breaks because a picture is missing. */
static void draw_icon(int x, int y, koi_uint32 tint);

static void draw_entry_icon(int x, int y, const ENTRY* entry) {
    int icon = entry->icon ? window_icon(entry->icon) : 0;

    if (icon) {
        window_icon_draw(icon, x + 16 - window_icon_width(icon) / 2,
                         y + 16 - window_icon_height(icon) / 2);
        return;
    }
    draw_icon(x - 16, y - 6, entry->tint());
}

/* A tile with a drop in it: what was drawn before there were drawings, and
   what is still drawn when one is missing. */
static void draw_icon(int x, int y, koi_uint32 tint) {
    koi_gfx_fill(x + 14, y + 6, 36, 30, tint);
    koi_gfx_fill(x + 14, y + 6, 36, 6,
                 koi_gfx_color(0xFF, 0xFF, 0xFF));
    koi_gfx_rect(x + 14, y + 6, 36, 30, window_shadow);
    /* The drop: a small square with its top corners taken off, which at eight
       pixels is as much water as anybody can see. */
    koi_gfx_fill(x + 28, y + 16, 8, 10, window_client_paper);
    koi_gfx_line(x + 31, y + 13, x + 31, y + 15, window_client_paper);
    koi_gfx_rect(x + 28, y + 16, 8, 10, tint);
}

/* A label under an icon, wrapped over as many lines as it needs and every
 * line centred.
 *
 * There used to be a second line of description under the name, and in
 * English it fitted. "Control Panel" became "Панель керування" and the
 * description became the thing that ran into the neighbouring icon - so the
 * description is gone and the name gets the room instead. A caption that only
 * fits in the language it was written in is a caption that was never measured.
 *
 * Broken at spaces; a single word longer than the cell is left to overhang,
 * because breaking a word mid-letter reads worse than a wide one.
 */
#define LABEL_LINES 3

static void label_in_cell(int x, int y, const char* text) {
    int cell = ICON_W - 8;
    int fits = cell / WINDOW_CHAR_W;
    char line[64];
    int line_count = 0;

    while (*text && line_count < LABEL_LINES) {
        int bytes = 0;
        int columns = 0;
        int last_space = -1;
        int width;

        while (text[bytes] && columns < fits) {
            if (text[bytes] == ' ') last_space = bytes;
            if (((unsigned char)text[bytes] & 0xC0) != 0x80) columns++;
            bytes++;
        }
        if (text[bytes] && last_space > 0) bytes = last_space;
        if (bytes > (int)sizeof(line) - 1) bytes = (int)sizeof(line) - 1;
        memcpy(line, text, (koi_uint64)bytes);
        line[bytes] = 0;

        width = language_columns(line) * WINDOW_CHAR_W;
        window_label(x + (cell - width) / 2, y + line_count * WINDOW_CHAR_H,
                     line, window_text);
        line_count++;

        text += bytes;
        while (*text == ' ') text++;
    }
}

static void paint_control(WINDOW* window, int x, int y, int width, int height) {
    (void)window;
    (void)height;
    for (int index = 0; index < ENTRY_COUNT; index++) {
        int column = index % (width / ICON_W ? width / ICON_W : 1);
        int row = index / (width / ICON_W ? width / ICON_W : 1);
        int ix = x + 8 + column * ICON_W;
        int iy = y + 8 + row * ICON_H;

        draw_entry_icon(ix + (ICON_W - 8) / 2 - 16, iy + 4, &entries[index]);
        label_in_cell(ix, iy + 40, entries[index].name);
    }
}

/* ---- The Start menu ------------------------------------------------------
 *
 * What is in it is not written down here. Every package dosget installs leaves
 * \BOOT\DOSGET\<NAME>.PKG naming its directory and the files it put there,
 * so the menu is a reading of what this machine actually has: a package
 * installed after Mizu was built appears without Mizu knowing anything about
 * it. That record exists for the installer's sake; this is its second reader.
 *
 * The identifiers below sit above the built-in menu ids so one switch can
 * carry both.
 */
#define START_FIRST_PROGRAM 100
#define START_MAX_PROGRAMS 12
#define START_SETTINGS 90
#define START_SHUTDOWN 91
#define START_RUN 92

typedef struct {
    char label[32];      /* what to show: the package's name */
    char command[96];    /* what to run: its directory and its program */
} START_PROGRAM;

static START_PROGRAM start_programs[START_MAX_PROGRAMS];
static int start_program_count;

/* Does `text` end with `suffix`, ignoring case? Used to keep a package's
   first-run questions off the Programs menu. */
static int ends_with_ignoring_case(const char* text, const char* suffix) {
    long length = 0, tail = 0;

    while (text[length]) length++;
    while (suffix[tail]) tail++;
    if (tail > length) return 0;
    for (long index = 0; index < tail; index++) {
        char a = text[length - tail + index];
        char b = suffix[index];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* One .PKG record: the directory it names, and the first .EXE in it that is
   not a configuration program. A package whose only executable is its own
   first-run questions is not something to offer on a menu. */
static int read_package(const char* record, START_PROGRAM* into) {
    char text[1024];
    long handle = koi_open(record, OPEN_READ);
    long got;
    char directory[64];
    char program[64];

    if (handle < 0) return 0;
    got = koi_read(handle, text, sizeof(text) - 1);
    koi_close(handle);
    if (got <= 0) return 0;
    text[got] = 0;

    directory[0] = 0;
    program[0] = 0;

    for (long at = 0; at < got; ) {
        char line[128];
        long length = 0;
        const char* value;

        while (at < got && text[at] != '\n' && length + 1 < (long)sizeof(line))
            line[length++] = text[at++];
        while (at < got && text[at] != '\n') at++;
        if (at < got) at++;
        while (length && (line[length - 1] == '\r' || line[length - 1] == ' '))
            length--;
        line[length] = 0;

        value = line;
        while (*value && *value != '=') value++;
        if (!*value) continue;
        value++;
        while (*value == ' ') value++;

        if (line[0] == 'd' || line[0] == 'D') {
            long copied = 0;
            while (value[copied] && copied + 1 < (long)sizeof(directory))
                { directory[copied] = value[copied]; copied++; }
            directory[copied] = 0;
        } else if ((line[0] == 'f' || line[0] == 'F') && !program[0]) {
            long copied = 0;
            /* .EXE only, and not the package's configuration program: CMDRCFG
               and MIZUCFG are asked for from Settings, not from Programs. */
            while (value[copied] && copied + 1 < (long)sizeof(program))
                { program[copied] = value[copied]; copied++; }
            program[copied] = 0;
            if (copied < 5 || program[copied - 4] != '.') program[0] = 0;
            else if (ends_with_ignoring_case(program, "CFG.EXE")) program[0] = 0;
            else if (!ends_with_ignoring_case(program, ".EXE")) program[0] = 0;
        }
    }

    if (!directory[0] || !program[0]) return 0;

    {
        /* The label is the directory without its backslash - the package's
           name, which is what somebody would call it. */
        const char* name = directory;
        long copied = 0;
        while (*name == '\\') name++;
        while (name[copied] && copied + 1 < (long)sizeof(into->label))
            { into->label[copied] = name[copied]; copied++; }
        into->label[copied] = 0;
    }
    koi_snprintf(into->command, sizeof(into->command), "%s\\%s", directory,
                 program);
    return 1;
}

static void find_programs(void) {
    KOI_FIND_DATA found;
    long search;

    start_program_count = 0;
    search = koi_findfirst("\\BOOT\\DOSGET\\*.PKG", &found);
    while (search >= 0 && start_program_count < START_MAX_PROGRAMS) {
        char record[128];

        koi_snprintf(record, sizeof(record), "\\BOOT\\DOSGET\\%s", found.name);
        if (read_package(record, &start_programs[start_program_count]))
            start_program_count++;
        /* Zero is success here, not failure: the call answers "did this
           work" the way a system call does, not "is there another" the way an
           iterator would. Written the other way round, the loop stopped on the
           first record it found and the menu listed exactly one package. */
        if (koi_findnext(search, &found) != 0) break;
    }
    if (search >= 0) koi_findclose(search);
}

/* Is this character in that string? Two of these are needed to tell a bare
   name from a command line, and there is no such call in the SDK. */
static int koi_strchr_simple(const char* text, char wanted) {
    for (int at = 0; text[at]; at++) if (text[at] == wanted) return 1;
    return 0;
}

static void open_application(const char* file);

/* The Run box, from the Start menu or from Win+R - one function, because two
   ways in must not become two behaviours. */
static void run_dialog(void) {

        /* Whatever the shell can run: a program, or a command like `dir` that
           only exists inside it. Mizu does not look at what was typed and
           does not need to - SYS_RUN hands the line to COMMAND, which is the
           one place that knows what a command line means. What comes back is
           the exit code, and KOI_EXIT_NOT_FOUND is the shell's "there is no
           such command"; anything
           else, including a program that failed for its own reasons, is the
           program's business and not a thing to put a box in front of. */
        static char line[96];
        long code;

        if (!window_prompt(say(SAY_RUN), say(SAY_RUN_ASK), say(DIALOG_OK),
                           say(DIALOG_CANCEL), line, sizeof(line))) {
            window_repaint();
            return;
        }
        /* An application, if that is what was named.
         *
         * Written with .APP or without: `FILES` and `FILES.APP` mean the same
         * thing here, because nobody should have to know that an application
         * has a different extension from a program to open one. A bare name
         * is looked for as an application first and handed to the shell only
         * if there is no such file - so a program of the same name still
         * runs, and typing `dir` still lists a directory. */
        if (ends_with_ignoring_case(line, ".APP")) {
            open_application(line);
            window_repaint();
            return;
        }
        /* The names Windows taught everybody.
         *
         * `explorer` is not what the browser is called and `cmd` is not what
         * the terminal is called, and neither ever will be - a system that
         * renames its own parts to match somebody else's has no parts of its
         * own. But most people arriving here have Windows in their fingers,
         * and a Run box that answers a name somebody already knows costs six
         * lines and teaches the real one on the way: what opens is titled
         * Files, and next time they type that. */
        {
            static const struct { const char* known; const char* ours; }
            familiar[] = {
                { "EXPLORER", FILES }, { "CMD", TERM }, { "COMMAND", TERM },
                { "NOTEPAD", NOTEEDIT }, { "CONTROL", CONTROL },
                { "MSPAINT", IMAGE }, { "PHOTOS", IMAGE }, { 0, 0 }
            };

            for (int index = 0; familiar[index].known; index++) {
                const char* known = familiar[index].known;
                int at = 0;

                while (known[at]) {
                    char typed = line[at];
                    if (typed >= 'a' && typed <= 'z') typed = (char)(typed - 32);
                    if (typed != known[at]) break;
                    at++;
                }
                if (!known[at] && !line[at]) {
                    open_application(familiar[index].ours);
                    window_repaint();
                    return;
                }
            }
        }

        {
            char guess[128];
            char beside[160];
            int at = 0;

            while (line[at] && at + 5 < (int)sizeof(guess)) {
                guess[at] = line[at];
                at++;
            }
            guess[at] = 0;
            /* Only a bare word: anything with a dot, a backslash or an
               argument is a command line and is the shell's business. */
            if (at && !koi_strchr_simple(guess, '.') &&
                !koi_strchr_simple(guess, '\\') &&
                !koi_strchr_simple(guess, ' ')) {
                strcpy(guess + at, ".APP");
                if (koi_beside(guess, beside, sizeof(beside)) &&
                    koi_exists(beside)) {
                    open_application(guess);
                    window_repaint();
                    return;
                }
            }
        }
        code = run_command(line);
        if (code == KOI_EXIT_NOT_FOUND)
            window_message(say(SAY_RUN), say(SAY_RUN_FAILED), say(DIALOG_OK));
}

/* Open the Start menu, and do what was chosen.
 *
 * Programs are listed straight rather than in a submenu: with a handful of
 * packages a submenu is a second click for nothing, and when there are enough
 * to need one this is where it goes. */
static void start_menu(void) {
    WINDOW_ITEM items[WINDOW_ITEM_MAX];
    int count = 0;
    int chosen;
    int anchor_x, anchor_y;

    find_programs();

    for (int index = 0; index < start_program_count &&
                        count < WINDOW_ITEM_MAX - 4; index++) {
        items[count].label = start_programs[index].label;
        items[count].id = START_FIRST_PROGRAM + index;
        count++;
    }
    if (!start_program_count) {
        items[count].label = say(SAY_NO_PROGRAMS);
        items[count].id = -1;
        count++;
    }
    items[count].label = 0;          /* a line */
    items[count].id = 0;
    count++;
    items[count].label = say(SAY_RUN);
    items[count].id = START_RUN;
    count++;
    items[count].label = say(SAY_SETTINGS);
    items[count].id = START_SETTINGS;
    count++;
    items[count].label = say(SAY_SHUT_DOWN);
    items[count].id = START_SHUTDOWN;
    count++;

    window_launcher_anchor(&anchor_x, &anchor_y);
    window_launcher_pressed(1);
    chosen = window_popup(items, count, anchor_x, anchor_y);
    window_launcher_pressed(0);
    window_repaint();

    if (chosen == START_SHUTDOWN) {
        /* Asked in the middle of the screen with everything behind it dimmed,
           and starting on Cancel. A Start menu whose bottom entry turns the
           machine off without asking is one people learn to fear - and a
           question asked in the corner the menu was just in is one that gets
           the answer meant for the menu. */
        chosen = window_confirm(say(SAY_SHUT_DOWN), say(SAY_SHUT_DOWN_ASK),
                                say(DIALOG_OK), say(DIALOG_CANCEL), 0);
        window_repaint();
        if (chosen == 1) {
            /* SYS_RUN executes a command line through the shell, built-in
               commands included, so this is the same `shutdown` a person
               would type - ACPI, and the kernel's own fallback behind it. */
            koi_gfx_leave();
            koi_run("shutdown");
        }
        return;
    }
    if (chosen == START_RUN) { run_dialog(); return; }
    if (chosen == START_SETTINGS) {
        /* Settings, and not the control panel: the panel is a page of things
           to start, which is what Program Manager was. This is where the
           machine's own switches live. */
        open_application(CONTROL);
        return;
    }
    if (chosen >= START_FIRST_PROGRAM &&
        chosen < START_FIRST_PROGRAM + start_program_count) {
        /* The desktop goes away while it runs and comes back afterwards. That
           is what SYS_RUN is - the caller is stopped inside the call - and it
           is how Windows 3.0 ran a DOS program too. Until an application can
           draw into a window, this is the honest shape. */
        koi_gfx_leave();
        koi_run(start_programs[chosen - START_FIRST_PROGRAM].command);
        if (window_reopen_desktop()) window_repaint();
    }
}

/* ---- Applications --------------------------------------------------------
 *
 * NoteEdit used to be here, and is now NOTEEDIT.APP beside this program: an
 * image Mizu loads, calls, and is handed a table of functions by. The contract
 * is in mizu.h and the reasoning with it; what changes here is that the
 * desktop no longer contains the things running on it.
 *
 * The table below is that contract's other half - everything an application
 * cannot simply call for itself. Drawing, files, sound and the keyboard are
 * system calls and need nothing from us; the windows, the theme, the phrases
 * and the dialogues all live in variables inside window.c, and an application
 * that linked its own copy of that would be drawing into a second desktop
 * nobody can see.
 */
#define APPLICATION_MAX 8

typedef struct {
    char file[32];
    KOI_MODULE module;
    MIZU_APP* app;
    WINDOW* window;
} APPLICATION;

static APPLICATION applications[APPLICATION_MAX];

/* An application asking for something to be run. The screen is given up and
   taken back here, because an application cannot know what else is on it. */
static int open_file(const char* path);

/* ---- The output of a program that only printed -------------------------
 *
 * A window with what it said in it, which is where a console program's output
 * belongs when there is a desktop. Windows called this a DOS box and drew a
 * frame around a console; this is smaller and honest about being smaller - it
 * shows what was printed and it does not take typing, because the program has
 * already finished by the time this appears.
 *
 * One window, reused: two of these open at once would be two programs' output
 * on screen with nothing to say which is which, and the second would be the
 * one somebody wanted anyway.
 */
static WINDOW* output_window;
static char output_text[8192];
static int output_top;          /* the first line shown */

static int output_line_count(void) {
    int lines = 1;
    for (int at = 0; output_text[at]; at++)
        if (output_text[at] == '\n') lines++;
    return lines;
}

static void paint_output(WINDOW* self, int x, int y, int width, int height) {
    int line = 0;
    int at = 0;
    int rows = height / WINDOW_CHAR_H;

    (void)self;
    (void)width;
    while (output_text[at] && line < output_top + rows) {
        char shown[128];
        int length = 0;

        while (output_text[at] && output_text[at] != '\n' &&
               length + 1 < (int)sizeof(shown))
            shown[length++] = output_text[at++];
        shown[length] = 0;
        while (output_text[at] && output_text[at] != '\n') at++;
        if (output_text[at]) at++;

        if (line >= output_top)
            window_label(x + 4, y + (line - output_top) * WINDOW_CHAR_H, shown,
                         window_text);
        line++;
    }
}

static void key_output(WINDOW* self, int pressed) {
    int client_x, client_y, client_w, client_h;
    int rows;

    window_client(self, &client_x, &client_y, &client_w, &client_h);
    rows = client_h / WINDOW_CHAR_H;
    switch (pressed) {
    case KOI_KEY_UP: if (output_top) output_top--; break;
    case KOI_KEY_DOWN:
        if (output_top + rows < output_line_count()) output_top++;
        break;
    case KOI_KEY_PAGE_UP:
        output_top -= rows;
        if (output_top < 0) output_top = 0;
        break;
    case KOI_KEY_PAGE_DOWN:
        if (output_top + rows < output_line_count()) output_top += rows;
        break;
    default: return;
    }
    window_repaint();
}

static void show_output(const char* command, const char* text) {
    int at = 0;
    int width;
    int height;

    while (text[at] && at + 1 < (int)sizeof(output_text)) {
        output_text[at] = text[at];
        at++;
    }
    output_text[at] = 0;
    output_top = 0;

    /* Sized to what was printed, within what the screen has.
     *
     * A fixed window cut the top off DOSFETCH, whose first act is to draw its
     * name in letters eight rows high - and a window that cuts off the part a
     * program clearly meant somebody to see is a window that was measured for
     * nothing in particular. The longest line and the number of lines are
     * known here; the screen's size is a question the kernel answers. */
    {
        int longest = 0;
        int columns = 0;
        int lines = 1;
        int most_wide = (int)koi_sysinfo(KOI_INFO_SCREEN_WIDTH, 0) - 60;
        int most_tall = (int)koi_sysinfo(KOI_INFO_SCREEN_HEIGHT, 0) - 120;

        for (int scan = 0; output_text[scan]; scan++) {
            if (output_text[scan] == '\n') {
                if (columns > longest) longest = columns;
                columns = 0;
                lines++;
                continue;
            }
            /* Columns rather than bytes: the text may be in a language whose
               letters take more than one. */
            if (((unsigned char)output_text[scan] & 0xC0) != 0x80) columns++;
        }
        if (columns > longest) longest = columns;

        width = longest * WINDOW_CHAR_W + 2 * WINDOW_BORDER + 10;
        height = lines * WINDOW_CHAR_H + WINDOW_TITLE_H + 2 * WINDOW_BORDER + 8;
        if (width < 320) width = 320;
        if (height < 160) height = 160;
        if (most_wide > 200 && width > most_wide) width = most_wide;
        if (most_tall > 160 && height > most_tall) height = most_tall;
    }

    if (!output_window) {
        output_window = window_new(command, 60, 90, width, height);
        if (!output_window) return;
        output_window->paint = paint_output;
        output_window->key = key_output;
    } else {
        strncpy(output_window->title, command, WINDOW_TITLE_MAX - 1);
        output_window->width = width;
        output_window->height = height;
        output_window->minimised = 0;
        window_raise(output_window);
    }
    window_repaint();
}

/* Run a Koi-DOS program without giving the screen away first.
 *
 * This used to hand the screen back, run, and take it again - so a program
 * that only prints made the desktop disappear, showed half a second of
 * console, and came back. That flash was the desktop's doing, not the
 * program's: the program never wanted the screen at all.
 *
 * The kernel stacks the screen now. A program that asks for it is laid over
 * whoever is holding it, and the desktop's pixels sit untouched underneath
 * until it ends - so DOOM still takes the whole display, and coming back is
 * one blit rather than a repaint. A program that only prints never asks, the
 * screen never changes hands, and its output is collected instead and shown
 * in a window.
 *
 * Which of the two it is nobody has to declare. The program says so by what
 * it does, at the moment it does it - which is the same answer Windows wrote
 * into a header, arrived at from the other end. */
static int run_command(const char* command) {
    static char collected[8192];
    long length;

    if (!command || !command[0]) return -1;

    length = koi_capture(command, collected, sizeof(collected));

    /* Whatever was typed at the program that has just ended belongs to it and
       not to the desktop. Without this, quitting DOOM with F10 and Y handed
       the desktop a Y, and the Escape somebody pressed to leave a menu closed
       whichever window happened to be in front when it came back. */
    while (koi_keypressed()) (void)koi_getchar();
    window_repaint();

    /* It printed something and did not take the screen: a console program,
       and its output belongs in a window rather than nowhere. */
    if (length > 0) show_output(command, collected);
    return koi_last_exit();
}

/* Everything on the desktop that is a phrase, so that changing the language
 * changes it now rather than at the next boot.
 *
 * say() hands back a pointer into the table for the language in force when it
 * was called, and the menus keep those pointers. Setting the language and
 * repainting therefore redraws the old words perfectly. This is the second
 * copy of one truth turning up again in a new place, and the answer is the
 * same as it was for the SDK: build the labels once, from one function, and
 * call it again when the fact underneath changes. */
static WINDOW_MENU* desktop_menus_here;
static WINDOW_MENU* panel_menus_here;

static void remember_labels(WINDOW_MENU* desktop, WINDOW_MENU* panel) {
    desktop_menus_here = desktop;
    panel_menus_here = panel;
}

static void relabel(void) {
    if (!desktop_menus_here) return;

    desktop_menus_here[0] = (WINDOW_MENU){ say(SAY_MENU_SYSTEM),
        { { say(SAY_ABOUT), MENU_ABOUT }, { 0, 0 },
          { say(SAY_EXIT), MENU_EXIT } }, 3 };
    desktop_menus_here[1] = (WINDOW_MENU){ say(SAY_MENU_RUN),
        { { say(SAY_FILES), MENU_FILES }, { say(SAY_NOTEEDIT), MENU_NOTE },
          { "Player", MENU_PLAYER }, { "Koi-DOS", MENU_TERM },
          { "Picture", MENU_IMAGE }, { "Nami Explorer", MENU_NAMI } }, 6 };
    desktop_menus_here[2] = (WINDOW_MENU){ say(SAY_MENU_VIEW),
        { { say(SAY_CONTROL_PANEL), MENU_CONTROL },
          { say(SAY_CLOCK), MENU_CLOCK }, { 0, 0 },
          { say(SAY_TILE), MENU_TILE } }, 4 };
    window_desktop_menu(desktop_menus_here, 3);
    window_launcher(say(SAY_START));

    if (panel_menus_here) {
        panel_menus_here[0] = (WINDOW_MENU){ say(SAY_MENU_FILE),
            { { say(SAY_EXIT), MENU_EXIT } }, 1 };
        panel_menus_here[1] = (WINDOW_MENU){ say(SAY_MENU_OPTIONS),
            { { say(SAY_ABOUT), MENU_ABOUT } }, 1 };
        if (control_window) {
            control_window->menus[0] = panel_menus_here[0];
            control_window->menus[1] = panel_menus_here[1];
            strncpy(control_window->title, say(SAY_CONTROL_PANEL),
                    WINDOW_TITLE_MAX - 1);
        }
    }
    /* The windows the desktop opened itself carry their titles in their own
       memory, so they need renaming too - a machine half in one language is
       worse than one entirely in the wrong one. */
    if (clock_window)
        strncpy(clock_window->title, say(SAY_CLOCK), WINDOW_TITLE_MAX - 1);
    if (about_window)
        strncpy(about_window->title, say(SAY_ABOUT), WINDOW_TITLE_MAX - 1);
    name_entries();
    /* And every application that is loaded, because its menus hold the same
       kind of pointer this one did. `version` says whether it has the field
       at all: an application built before this existed has a shorter
       structure, and reading past it would be calling whatever follows. */
    for (int index = 0; index < APPLICATION_MAX; index++) {
        MIZU_APP* app = applications[index].app;

        if (app && app->version >= 3 && app->relabel) app->relabel();
    }
    window_repaint();
}

/* The language, as the desktop holds it. An application that linked its own
   copy of language.c would set its own idea of it and leave this one alone -
   which is what the settings window did until this was here. */
static int api_language(void) { return language_current(); }
static int api_language_count(void) { return LANGUAGE_COUNT; }
static const char* api_language_name(int which) { return language_name(which); }

static void api_language_set(int which) {
    language_set(which);
    relabel();
}

static koi_uint32 api_color(int which) {
    switch (which) {
    case MIZU_COLOR_FACE: return window_face;
    case MIZU_COLOR_LIGHT: return window_light;
    case MIZU_COLOR_SHADOW: return window_shadow;
    case MIZU_COLOR_PAPER: return window_client_paper;
    case MIZU_COLOR_ACCENT: return window_accent;
    default: return window_text;
    }
}

/* By name rather than by position.
 *
 * The version says what an application may expect, and this table is where
 * that promise is actually kept: a field added to MIZU_API and forgotten here
 * is a null pointer an application calls because the version told it it could.
 * Written out by name, the omission is visible; written by position it is a
 * comma nobody counts. */
static const MIZU_API mizu_api = {
    .version = MIZU_API_VERSION,
    .window_new = window_new,
    .window_delete = window_delete,
    .window_raise = window_raise,
    .window_client = window_client,
    .repaint = window_repaint,
    .label = window_label,
    .label_styled = window_label_styled,
    .raised = window_raised,
    .sunken = window_sunken,
    .color = api_color,
    .confirm = window_confirm,
    .message = window_message,
    .prompt = window_prompt,
    .context_menu = window_context,
    .scrollbar = window_scrollbar,
    .scrollbar_press = window_scrollbar_press,
    .scrollbar_drag = window_scrollbar_drag,
    .say = say,
    .yield = window_yield,
    .run = run_command,
    .open_file = open_file,
    .language = api_language,
    .language_count = api_language_count,
    .language_name = api_language_name,
    .language_set = api_language_set
};

/* Which application a window belongs to, or none. This is how a menu choice
   in an application's window reaches the application: Mizu does not know what
   the ids mean and hands them straight back. */
static APPLICATION* application_of(const WINDOW* window) {
    if (!window) return (APPLICATION*)0;
    for (int index = 0; index < APPLICATION_MAX; index++)
        if (applications[index].app && applications[index].window == window)
            return &applications[index];
    return (APPLICATION*)0;
}

/* Load it if it is not loaded, then ask it to open.
 *
 * Loaded once and kept: a module holds its own document, and loading it a
 * second time would give a second NoteEdit with a second copy of the note,
 * both writing to one file. */
static APPLICATION* load_application(const char* file) {
    APPLICATION* slot = (APPLICATION*)0;
    char path[128];
    MIZU_APP* (*entry)(const MIZU_API*);

    for (int index = 0; index < APPLICATION_MAX; index++) {
        APPLICATION* candidate = &applications[index];
        if (candidate->app && !strcmp(candidate->file, file)) return candidate;
        if (!candidate->app && !slot) slot = candidate;
    }
    if (!slot) return (APPLICATION*)0;

    /* Beside this program, because that is where its package put it - dosget
       chooses the directory, not us. */
    if (!koi_beside(file, path, sizeof(path))) return (APPLICATION*)0;
    if (koi_load(path, &slot->module) != 0) {
        window_message(say(SAY_DESKTOP_TITLE), say(SAY_NO_APPLICATION),
                       say(DIALOG_OK));
        window_repaint();
        return (APPLICATION*)0;
    }
    entry = (MIZU_APP* (*)(const MIZU_API*))(unsigned long)slot->module.entry;
    slot->app = entry(&mizu_api);
    if (!slot->app) {
        koi_unload(slot->module.base);
        return (APPLICATION*)0;
    }
    strncpy(slot->file, file, sizeof(slot->file) - 1);
    slot->file[sizeof(slot->file) - 1] = 0;
    return slot;
}

static void open_application(const char* file) {
    APPLICATION* slot = load_application(file);

    if (slot) slot->window = slot->app->open();
}

/* Which application opens a file of this kind.
 *
 * A table, and a short one on purpose. What it must not become is a registry -
 * a place where installing something rewrites how the machine behaves, which
 * is how a system stops being predictable. When a package wants to claim a
 * suffix it will say so in its own .PKG record, and this will read that the
 * way the Start menu already reads the rest. */
static int open_file(const char* path) {
    static const struct { const char* suffix; const char* application; }
    openers[] = {
        { ".BMP", IMAGE }, { ".PNG", IMAGE }, { ".GIF", IMAGE },
        { ".WAV", PLAYER }, { ".MP3", PLAYER },
        { ".HTM", NAMI }, { ".HTML", NAMI },
        { ".TXT", NOTEEDIT }, { ".MD", NOTEEDIT }, { ".BAT", NOTEEDIT },
        { ".CFG", NOTEEDIT }, { ".LOG", NOTEEDIT }, { ".INI", NOTEEDIT },
        { 0, 0 }
    };

    if (!path || !path[0]) return 0;
    for (int index = 0; openers[index].suffix; index++) {
        APPLICATION* slot;

        if (!ends_with_ignoring_case(path, openers[index].suffix)) continue;
        slot = load_application(openers[index].application);
        if (!slot || slot->app->version < 2 || !slot->app->open_with)
            return 0;
        slot->window = slot->app->open_with(path);
        return slot->window != (WINDOW*)0;
    }

    /* Anything else that is not a program: the notepad.
     *
     * A desktop that answers "nothing opens that" for every extension nobody
     * has written a table entry for is a desktop that mostly says no. A file
     * is text until proven otherwise, and a windowed editor showing bytes is
     * a fair answer to an unknown one - it is what Windows did with Notepad
     * for years and it is better than the alternative here, which was running
     * the console editor with its output collected where nobody could see it
     * and the desktop stopped waiting for it to finish.
     *
     * Programs are excluded, because a program is something to run. */
    if (ends_with_ignoring_case(path, ".EXE") ||
        ends_with_ignoring_case(path, ".COM") ||
        ends_with_ignoring_case(path, ".BAT") ||
        ends_with_ignoring_case(path, ".APP")) return 0;
    {
        APPLICATION* slot = load_application(NOTEEDIT);

        if (!slot || slot->app->version < 2 || !slot->app->open_with) return 0;
        slot->window = slot->app->open_with(path);
        return slot->window != (WINDOW*)0;
    }
}

static void open_about(void);
static void open_clock(void);

/* Close one window and forget the pointer to it.
 *
 * One function because there are two ways to close a window - its close box
 * and the Escape key - and they have to agree about what closing means. When
 * this was written out twice, the second copy did not know that the player has
 * to be stopped as well as deleted, and a closed player went on making a
 * noise. */
static void close_window(WINDOW* window) {
    APPLICATION* owner = application_of(window);

    if (!window) return;
    /* An application is told before its window goes, because only it knows
       whether there is anything to save first. */
    if (owner) {
        if (owner->app->closing) owner->app->closing(window);
        owner->window = (WINDOW*)0;
    }
    if (window == clock_window) clock_window = (WINDOW*)0;
    if (window == about_window) about_window = (WINDOW*)0;
    if (window == output_window) output_window = (WINDOW*)0;
    window_delete(window);
}

/* Which cell of the grid a point is in, or -1. */
static int entry_at(int x, int y) {
    int client_x, client_y, client_w, client_h;
    int columns;
    int index;

    if (!control_window) return -1;
    window_client(control_window, &client_x, &client_y, &client_w, &client_h);
    columns = client_w / ICON_W;
    if (columns < 1) columns = 1;
    if (x < 8 || y < 8) return -1;
    index = (y - 8) / ICON_H * columns + (x - 8) / ICON_W;
    if (index < 0 || index >= ENTRY_COUNT) return -1;
    return index;
}

/* The right button on a cell: what can be done with the thing it stands for.
 *
 * Two entries, and the second of them is the honest half - Open is what a
 * double click already does, and having it here is not for people who cannot
 * double-click but for people who are looking for what is possible. A context
 * menu is where a system says what it can do. */
static void context_control(WINDOW* window, int x, int y) {
    WINDOW_ITEM items[3];
    int index = entry_at(x, y);
    int client_x, client_y, client_w, client_h;
    int chosen;

    (void)window;
    if (index < 0) return;
    window_client(control_window, &client_x, &client_y, &client_w, &client_h);

    items[0].label = say(SAY_OPEN);
    items[0].id = 1;
    items[1].label = say(SAY_ABOUT);
    items[1].id = 2;
    /* Screen coordinates: the handler is given the window's own, and the menu
       is drawn on the screen. */
    chosen = window_context(items, 2, client_x + x, client_y + y);
    window_repaint();
    if (chosen == 1 && entries[index].open) entries[index].open();
    else if (chosen == 2) open_about();
}

static void click_control(WINDOW* window, int x, int y, int clicks) {
    int index = entry_at(x, y);

    (void)window;
    if (index < 0) return;
    /* Twice, as Program Manager had it: one click to point at a thing and two
       to set it going, so a hand resting on the button does not launch it. */
    if (clicks < 2) return;

    if (entries[index].open) entries[index].open();
}

/* ---- The clock ----------------------------------------------------------- */

/* Which weekday the first of a month falls on, 0 Sunday. Zeller's, because a
   table of month lengths and a running count is the same arithmetic written
   out longer and wrong in February. */
static int first_weekday(int year, int month) {
    int shift_month = month;
    int shift_year = year;
    int century;

    if (shift_month < 3) { shift_month += 12; shift_year--; }
    century = shift_year / 100;
    shift_year %= 100;
    return (1 + (13 * (shift_month + 1)) / 5 + shift_year + shift_year / 4 +
            century / 4 + 5 * century) % 7;
}

static int month_length(int year, int month) {
    static const int lengths[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return lengths[month - 1];
}

static void paint_clock(WINDOW* window, int x, int y, int width, int height) {
    long now = koi_sysinfo(KOI_INFO_TIME, 0);
    long today = koi_sysinfo(KOI_INFO_DATE, 0);
    int year = KOI_DATE_YEAR(today);
    int month = KOI_DATE_MONTH(today);
    int day = KOI_DATE_DAY(today);
    int start = first_weekday(year, month);
    int length = month_length(year, month);
    char line[64];
    int cell = 24;
    int left;

    (void)window;
    (void)height;

    koi_snprintf(line, sizeof(line), "%02d:%02d:%02d",
                 KOI_TIME_HOUR(now), KOI_TIME_MINUTE(now), KOI_TIME_SECOND(now));
    window_label(x + (width - language_columns(line) * WINDOW_CHAR_W) / 2, y + 6,
                 line, window_text);
    /* "9 August 2026" rather than 2026-08-09: the numbers are for sorting and
       the words are for reading, and this window is for reading. */
    koi_snprintf(line, sizeof(line), "%d %s %d", day, language_month(month),
                 year);
    window_label(x + (width - language_columns(line) * WINDOW_CHAR_W) / 2,
                 y + 24, line, window_text);

    left = x + (width - 7 * cell) / 2;
    for (int index = 0; index < 7; index++)
        window_label(left + index * cell + 4, y + 46,
                     language_weekday(index), window_shadow);

    for (int number = 1; number <= length; number++) {
        int slot = start + number - 1;
        int cx = left + (slot % 7) * cell;
        int cy = y + 66 + (slot / 7) * 20;

        koi_snprintf(line, sizeof(line), "%d", number);
        if (number == day) {
            koi_gfx_fill(cx, cy - 2, cell - 2, 18, window_accent);
            window_label(cx + (number < 10 ? 8 : 4), cy, line,
                         window_client_paper);
        } else {
            window_label(cx + (number < 10 ? 8 : 4), cy, line, window_text);
        }
    }
}

/* ---- About --------------------------------------------------------------- */

static void paint_about(WINDOW* window, int x, int y, int width, int height) {
    char line[96];
    char cpu[64];
    int current = current_volume_index();
    long free_volume = current >= 0 ? koi_sysinfo(KOI_INFO_VOLUME_FREE_BYTES,
                                                  current) : -1;

    (void)window;
    (void)width;
    (void)height;
    window_label(x + 12, y + 10, say(SAY_DESKTOP_TITLE), window_text);
    koi_snprintf(line, sizeof(line), "Koi-DOS build %ld",
                 koi_sysinfo(KOI_INFO_BUILD_NUMBER, 0));
    window_label(x + 12, y + 34, line, window_text);
    if (koi_systext(KOI_TEXT_CPU_NAME, 0, cpu, sizeof(cpu)) <= 0)
        strcpy(cpu, "unknown CPU");
    koi_snprintf(line, sizeof(line), "CPU: %s", cpu);
    window_label(x + 12, y + 58, line, window_text);
    koi_snprintf(line, sizeof(line), "RAM: %ld / %ld %s",
                 koi_sysinfo(KOI_INFO_MEMORY_FREE, 0),
                 koi_sysinfo(KOI_INFO_MEMORY_TOTAL, 0), say(SAY_FREE));
    window_label(x + 12, y + 74, line, window_text);
    if (free_volume >= 0) {
        char volume_letter = (char)koi_sysinfo(KOI_INFO_VOLUME_LETTER, current);
        koi_snprintf(line, sizeof(line), "Disk %c: %ld %s", volume_letter,
                     free_volume, say(SAY_FREE));
        window_label(x + 12, y + 98, line, window_text);
    }
}

/* ---- Opening things ------------------------------------------------------ */

static void open_clock(void) {
    if (clock_window) { clock_window->minimised = 0; window_raise(clock_window); return; }
    clock_window = window_new(say(SAY_CLOCK), 620, 300, 300, 260);
    if (!clock_window) return;
    clock_window->paint = paint_clock;
    clock_window->repaint_ms = 1000;   /* a clock that does not tick is a date */
}

static void open_about(void) {
    if (about_window) { about_window->minimised = 0; window_raise(about_window); return; }
    about_window = window_new(say(SAY_ABOUT), 360, 380, 480, 180);
    if (!about_window) return;
    about_window->paint = paint_about;
}

/* Koi-Commander is another program, so it is started the way any program is
   started here: ask for it, ask for this desktop after it, and leave. The
   screen goes away and comes back, which is honest about what the machine can
   do rather than a window pretending otherwise. */

/* The sound the desktop makes when it starts.
 *
 * Beside the program, like the wallpaper: the package is installed wherever
 * dosget put it. Absent is not an error - a desktop that refuses to start
 * because a sound file is missing would be a poor trade for a noise.
 *
 * The samples are not copied by the mixer, so the buffer has to outlive the
 * sound: it is allocated once and never freed, which for one file played once
 * at startup is the honest arrangement rather than a leak. Playing does not
 * block, so the desktop draws while it sounds - which is the whole point of a
 * startup sound and was the one thing Windows 95 got exactly right about it.
 */
static void play_startup_sound(void) {
    char path[128];
    WAV_FORMAT format;
    unsigned int data_at = 0;
    unsigned int frames;
    const char* why;
    long handle;
    long size;
    koi_uint8* file;

    if (!koi_beside("START.WAV", path, sizeof(path))) return;
    handle = koi_open(path, OPEN_READ);
    if (handle < 0) return;

    size = koi_seek(handle, 0, KOI_SEEK_END);
    (void)koi_seek(handle, 0, KOI_SEEK_SET);
    if (size <= 0) { koi_close(handle); return; }

    file = (koi_uint8*)koi_alloc(size);
    if (!file) { koi_close(handle); return; }
    {
        long done = 0;
        while (done < size) {
            long got = koi_read(handle, file + done, size - done);
            if (got <= 0) break;
            done += got;
        }
        koi_close(handle);
        if (done != size) { koi_free(file); return; }
    }

    {
        unsigned int bytes = wav_parse(file, (unsigned int)size, &format,
                                       &data_at, &why);
        if (!bytes) { koi_free(file); return; }
        frames = bytes / (format.channels * (format.bits / 8));
    }
    if (!frames) { koi_free(file); return; }

    (void)koi_sound_play_simple(file + data_at, frames, format.rate,
                                format.bits, format.channels, 255);
}

int main(void) {
    /* The desktop has a notepad in it, so it asks for the layout gesture the
       way the console editor does. The kernel takes it back when Mizu exits,
       so the prompt underneath is left typing ASCII. */
    koi_layout_gesture(1);
    /* Ctrl+C is a keystroke here, not an execution.
     *
     * The kernel's rule is right for a command and wrong for a desktop: a
     * shell is a program, so Ctrl+C closed every window and dropped whoever
     * pressed it back at the prompt. It is put back the moment this exits, so
     * the prompt underneath keeps its Ctrl+C. */
    koi_break(0);
    WINDOW_EVENT event;
    WINDOW_MENU desktop[3];
    WINDOW_MENU panel[2];

    /* The first time, ask the questions before drawing anything. Done by
       asking the shell to run the configuration and then this again, because
       one program runs at a time and this one has not taken the screen yet. */
    {
        char configured[16];

        if (!koi_arguments()[0] &&
            (!settings_get("MIZU", "configured", configured,
                           sizeof(configured)) || configured[0] != '1')) {
            char self[128];
            char config[128];
            int cut = 0;

            if (koi_systext(KOI_TEXT_PROGRAM_PATH, 0, self, sizeof(self)) <= 0)
                strcpy(self, "\\MIZU\\MIZU.EXE");
            strcpy(config, self);
            for (int index = 0; config[index]; index++)
                if (config[index] == '\\') cut = index + 1;
            strcpy(config + cut, "MIZUCFG.EXE");
            koi_chain(self);
            koi_chain(config);
            return 0;
        }
    }

    language_load();

    if (!window_open_desktop(say(SAY_DESKTOP_TITLE))) {
        koi_print("Mizu needs a framebuffer and could not get one.\n");
        return 1;
    }
    /* Built here rather than written as literals: a menu in three languages
       is three tables that drift apart, and one table filled in at startup is
       one. Built again when the language changes - see relabel(). */
    desktop[0] = (WINDOW_MENU){ say(SAY_MENU_SYSTEM),
        { { say(SAY_ABOUT), MENU_ABOUT }, { 0, 0 }, { say(SAY_EXIT), MENU_EXIT } }, 3 };
    desktop[1] = (WINDOW_MENU){ say(SAY_MENU_RUN),
        { { say(SAY_FILES), MENU_FILES }, { say(SAY_NOTEEDIT), MENU_NOTE },
          { "Player", MENU_PLAYER }, { "Koi-DOS", MENU_TERM },
          { "Picture", MENU_IMAGE }, { "Nami Explorer", MENU_NAMI } }, 6 };
    desktop[2] = (WINDOW_MENU){ say(SAY_MENU_VIEW),
        { { say(SAY_CONTROL_PANEL), MENU_CONTROL },
          { say(SAY_CLOCK), MENU_CLOCK }, { 0, 0 },
          { say(SAY_TILE), MENU_TILE } }, 4 };
    panel[0] = (WINDOW_MENU){ say(SAY_MENU_FILE),
        { { say(SAY_EXIT), MENU_EXIT } }, 1 };
    panel[1] = (WINDOW_MENU){ say(SAY_MENU_OPTIONS),
        { { say(SAY_ABOUT), MENU_ABOUT } }, 1 };

    window_desktop_menu(desktop, 3);
    window_launcher(say(SAY_START));
    play_startup_sound();
    remember_labels(desktop, panel);

    name_entries();
    control_window = window_new(say(SAY_CONTROL_PANEL), 60, 70, 512, 300);
    if (control_window) {
        control_window->paint = paint_control;
        control_window->click = click_control;
        control_window->context = context_control;
        control_window->menu_count = 2;
        control_window->menus[0] = panel[0];
        control_window->menus[1] = panel[1];
    }
    open_clock();

    while (window_next(&event)) {
        if (event.type == WINDOW_EVENT_CLOSE) {
            /* Closing the control panel closes the control panel.
             *
             * It used to end the session, which is Windows 3.0's arrangement -
             * Program Manager was the shell, and closing it was leaving. The
             * shape here is 9x: the desktop is the shell, the panel is a
             * window on it, and the way out is System - Exit to DOS, said in
             * words, on purpose.
             *
             * With nothing left open, Alt+F4 asks whether to turn the machine
             * off, which is where that key goes on every system since. */
            if (!event.window) {
                if (window_confirm(say(SAY_SHUT_DOWN), say(SAY_SHUT_DOWN_ASK),
                                   say(DIALOG_OK), say(DIALOG_CANCEL), 0) == 1) {
                    koi_gfx_leave();
                    koi_run("shutdown");
                }
                window_repaint();
                continue;
            }
            if (event.window == control_window) {
                window_delete(control_window);
                control_window = (WINDOW*)0;
                continue;
            }
            close_window(event.window);
            continue;
        }
        if (event.type == WINDOW_EVENT_MENU) {
            /* A choice made in an application's own window belongs to that
               application, whatever the number means. Mizu does not have a
               table of its ids and must not grow one - that is the difference
               between an application and a part of the shell. */
            APPLICATION* owner = application_of(event.window);

            if (owner && owner->app->menu) {
                owner->app->menu(event.window, event.id);
                continue;
            }
            switch (event.id) {
            case MENU_ABOUT: open_about(); break;
            case MENU_FILES: open_application(FILES); break;
            case MENU_TERM: open_application(TERM); break;
            case MENU_NOTE: open_application(NOTEEDIT); break;
            case MENU_PLAYER: open_application(PLAYER); break;
            case MENU_CLOCK: open_clock(); break;
            case MENU_IMAGE: open_application(IMAGE); break;
            case MENU_NAMI: open_application(NAMI); break;
            case MENU_CONTROL:
                if (control_window) {
                    control_window->minimised = 0;
                    window_raise(control_window);
                }
                break;
            case MENU_TILE: window_tile(); break;
            case MENU_EXIT:
                koi_log("MIZU: Exit to DOS was chosen\n");
                /* "Close" in a window's own File menu closes that window;
                   "Exit to DOS" in the desktop's menu ends everything. */
                window_quit();
                break;
            default: break;
            }
            continue;
        }
        /* Escape closes what is in front, and never the desktop.
         *
         * It used to end Mizu outright, from anywhere, with no question asked
         * - one key away at all times, next to nothing, on a machine where
         * leaving means the screen you were working on is gone. There is a way
         * out and it is deliberate: "Exit to DOS" in the desktop's menu.
         *
         * On the control panel it does nothing, because the control panel is
         * the desktop: closing it is leaving, and leaving is the thing Escape
         * must not do by accident. */
        if (event.type == WINDOW_EVENT_LAUNCHER) { start_menu(); continue; }

        /* The desktop itself was clicked.
         *
         * The right button opens what there is to do here, at the pointer,
         * the way it has since Windows 95: the things that are about the
         * desktop rather than about any window. The left button does nothing
         * yet - there is nothing on the wallpaper to select - and saying so
         * here is cheaper than wondering later why the case is missing. */
        if (event.type == WINDOW_EVENT_DESKTOP) {
            WINDOW_ITEM items[6];
            int count = 0;
            int chosen;

            if (event.button != WINDOW_BUTTON_RIGHT) continue;

            items[count].label = say(SAY_CONTROL_PANEL);
            items[count++].id = MENU_CONTROL;
            items[count].label = say(SAY_RUN);
            items[count++].id = START_RUN;
            items[count].label = say(SAY_TILE);
            items[count++].id = MENU_TILE;
            items[count].label = 0;
            items[count++].id = 0;
            items[count].label = say(SAY_ABOUT);
            items[count++].id = MENU_ABOUT;

            chosen = window_context(items, count, event.x, event.y);
            window_repaint();
            if (chosen == MENU_CONTROL) {
                if (control_window) {
                    control_window->minimised = 0;
                    window_raise(control_window);
                }
            }
            else if (chosen == START_RUN) run_dialog();
            else if (chosen == MENU_TILE) window_tile();
            else if (chosen == MENU_ABOUT) open_about();
            continue;
        }
        /* Win+R goes straight to the Run box, without the menu it lives in. */
        if (event.type == WINDOW_EVENT_KEY && event.id == KOI_KEY_RUN) {
            run_dialog();
            continue;
        }
        if (event.type == WINDOW_EVENT_KEY && event.id == 27) {
            WINDOW* front = window_active();

            if (front && front != control_window) close_window(front);
        }
    }

    /* And which way out was taken, in the one place every way out passes
       through. A desktop that vanishes without a word is a bug report nobody
       can answer. */
    koi_log("MIZU: the event loop ended\n");
    window_close_desktop();
    return 0;
}
