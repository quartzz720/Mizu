#include "mizu.h"
#include "language.h"

/* Files - the shell's browser, and deliberately not Koi-Commander.
 *
 * Nobody shipped Norton Commander in place of explorer.exe, and the reason is
 * that they are different tools. Two panels are for moving things between two
 * places you have already chosen. A browser is for *finding* something: one
 * place at a time, where you are written at the top, and a way back up. The
 * commander stays what it is and stays better at what it does.
 *
 * What this shows, top to bottom: the drives, when you are above every drive;
 * then `..` to go up; then the directories; then the files. Directories before
 * files because that is the order somebody scanning a list wants them in, and
 * every browser since has agreed.
 *
 * Opening a file runs something: a program runs itself, a .BAT goes through
 * the shell, a picture opens in `show` and text in `edit`. Those are Koi-DOS
 * programs and they take the screen while they run - the desktop comes back
 * afterwards. That is the shape of the machine today and this does not pretend
 * otherwise; when there is an application to hand a file to, this is the one
 * line that changes.
 */

#define FILES_UP 1
#define FILES_REFRESH 2
#define FILES_CLOSE 3

#define ENTRY_MAX 256
#define PATH_MAX 128
#define NAME_MAX 64

typedef struct {
    char name[NAME_MAX];
    unsigned int size;
    int directory;
} ENTRY;

static const MIZU_API* mizu;
static WINDOW* window;

/* Where we are. An empty path is above the drives - the place Windows called
   My Computer, which exists because a machine with four volumes has no one
   root and pretending otherwise means hiding three of them. */
static char place[PATH_MAX];
static ENTRY entries[ENTRY_MAX];
static int entry_count;
static int selected;
static int top_row;
static char status[80];

static void say_status(const char* text) {
    int at = 0;
    while (text[at] && at + 1 < (int)sizeof(status)) { status[at] = text[at]; at++; }
    status[at] = 0;
}

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

static void add_entry(const char* name, unsigned int size, int directory) {
    ENTRY* into;
    int at = 0;

    if (entry_count >= ENTRY_MAX) return;
    into = &entries[entry_count++];
    while (name[at] && at + 1 < NAME_MAX) { into->name[at] = name[at]; at++; }
    into->name[at] = 0;
    into->size = size;
    into->directory = directory;
}

/* The volumes this machine has, as `Z:` and the rest. */
static void read_drives(void) {
    long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);

    for (long index = 0; index < count; index++) {
        long letter = koi_sysinfo(KOI_INFO_VOLUME_LETTER, index);
        char name[4];

        if (letter <= 0) continue;
        name[0] = (char)letter;
        name[1] = ':';
        name[2] = 0;
        add_entry(name, 0, 1);
    }
    say_status("Drives");
}

static void read_place(void) {
    KOI_FIND_DATA found;
    char pattern[PATH_MAX + 8];
    long search;
    long at = 0;

    entry_count = 0;
    if (!place[0]) { read_drives(); return; }

    /* `..` first, because leaving is the commonest thing anybody does here. */
    add_entry("..", 0, 1);

    while (place[at]) { pattern[at] = place[at]; at++; }
    if (at && pattern[at - 1] != '\\') pattern[at++] = '\\';
    pattern[at++] = '*';
    pattern[at] = 0;

    /* Two passes, directories then files, so the list reads the way a list of
       places should rather than the order FAT happens to hold them in. */
    for (int wanted = 1; wanted >= 0; wanted--) {
        search = koi_findfirst(pattern, &found);
        while (search >= 0) {
            int directory = (found.attributes & KOI_ATTRIBUTE_DIRECTORY) != 0;

            if (directory == wanted && found.name[0] != '.')
                add_entry(found.name, found.size, directory);
            if (koi_findnext(search, &found) != 0) break;
        }
        if (search >= 0) koi_findclose(search);
    }
    koi_snprintf(status, sizeof(status), "%d item(s)", entry_count - 1);
}

static void go_to(const char* path) {
    int at = 0;

    while (path[at] && at + 1 < PATH_MAX) { place[at] = path[at]; at++; }
    place[at] = 0;
    selected = 0;
    top_row = 0;
    read_place();
    if (window) {
        /* The title is where you are. A browser whose title says only what
           the program is called makes you look at the list to find out. */
        koi_snprintf(window->title, WINDOW_TITLE_MAX, "Files - %s",
                     place[0] ? place : "Drives");
    }
    mizu->repaint();
}

static void go_up(void) {
    int cut = -1;

    if (!place[0]) return;
    for (int at = 0; place[at]; at++)
        if (place[at] == '\\') cut = at;
    /* "Z:\" is the root of a drive; above it are the drives themselves. */
    if (cut <= 2) {
        if (place[0] && place[1] == ':' && place[2] == '\\' && place[3])
            place[3] = 0, go_to(place);
        else go_to("");
        return;
    }
    place[cut] = 0;
    go_to(place);
}

static void open_entry(int index) {
    char path[PATH_MAX];
    ENTRY* entry;

    if (index < 0 || index >= entry_count) return;
    entry = &entries[index];

    if (!place[0]) {                     /* a drive */
        koi_snprintf(path, sizeof(path), "%s\\", entry->name);
        go_to(path);
        return;
    }
    if (entry->name[0] == '.' && entry->name[1] == '.') { go_up(); return; }

    {
        int at = 0;
        while (place[at] && at + 1 < PATH_MAX) { path[at] = place[at]; at++; }
        if (at && path[at - 1] != '\\' && at + 1 < PATH_MAX) path[at++] = '\\';
        path[at] = 0;
        for (int index2 = 0; entry->name[index2] && at + 1 < PATH_MAX; index2++)
            path[at++] = entry->name[index2];
        path[at] = 0;
    }

    if (entry->directory) { go_to(path); return; }

    {
        char line[PATH_MAX + 16];

        /* An application first: it opens in a window and the desktop stays
           where it is. Only when nothing here opens that kind of file does
           this fall back to running a program, which takes the screen. */
        if (mizu->open_file(path)) return;

        if (ends_with_ignoring_case(entry->name, ".EXE") ||
            ends_with_ignoring_case(entry->name, ".BAT"))
            koi_snprintf(line, sizeof(line), "%s", path);
        else if (ends_with_ignoring_case(entry->name, ".WAV"))
            koi_snprintf(line, sizeof(line), "play %s", path);
        else
            koi_snprintf(line, sizeof(line), "edit %s", path);

        if (mizu->run(line) == KOI_EXIT_NOT_FOUND)
            mizu->message("Files", "There is nothing here that opens that.",
                          mizu->say(DIALOG_OK));
        read_place();
        mizu->repaint();
    }
}

/* A folder, drawn: a tab and a body, which is what a folder has been since
   1984 and is four rectangles at this size. */
static void folder_icon(int x, int y, koi_uint32 face, koi_uint32 edge) {
    koi_gfx_fill(x, y + 2, 6, 3, face);
    koi_gfx_fill(x, y + 4, 14, 9, face);
    koi_gfx_rect(x, y + 4, 14, 9, edge);
}

static void file_icon(int x, int y, koi_uint32 face, koi_uint32 edge) {
    koi_gfx_fill(x + 2, y + 1, 10, 12, face);
    koi_gfx_rect(x + 2, y + 1, 10, 12, edge);
    koi_gfx_line(x + 4, y + 4, x + 9, y + 4, edge);
    koi_gfx_line(x + 4, y + 7, x + 9, y + 7, edge);
}

static void paint(WINDOW* self, int x, int y, int width, int height) {
    int rows = (height - WINDOW_CHAR_H - 6) / WINDOW_CHAR_H;

    (void)self;
    if (rows < 1) rows = 1;
    if (selected < top_row) top_row = selected;
    if (selected >= top_row + rows) top_row = selected - rows + 1;

    for (int row = 0; row < rows && top_row + row < entry_count; row++) {
        int index = top_row + row;
        ENTRY* entry = &entries[index];
        int line_y = y + row * WINDOW_CHAR_H;
        koi_uint32 ink = mizu->color(MIZU_COLOR_TEXT);

        if (index == selected) {
            koi_gfx_fill(x, line_y, width, WINDOW_CHAR_H,
                         mizu->color(MIZU_COLOR_ACCENT));
            ink = mizu->color(MIZU_COLOR_PAPER);
        }
        if (entry->directory)
            folder_icon(x + 4, line_y + 1, mizu->color(MIZU_COLOR_FACE), ink);
        else
            file_icon(x + 4, line_y + 1, mizu->color(MIZU_COLOR_PAPER), ink);
        mizu->label(x + 24, line_y, entry->name, ink);

        if (!entry->directory) {
            char size[24];
            koi_snprintf(size, sizeof(size), "%u", entry->size);
            mizu->label(x + width - 8 - (int)strlen(size) * WINDOW_CHAR_W,
                        line_y, size, ink);
        }
    }

    /* The status line, at the bottom where every browser has kept it. */
    koi_gfx_line(x, y + height - WINDOW_CHAR_H - 4, x + width - 1,
                 y + height - WINDOW_CHAR_H - 4, mizu->color(MIZU_COLOR_SHADOW));
    mizu->label(x + 4, y + height - WINDOW_CHAR_H - 1, status,
                mizu->color(MIZU_COLOR_TEXT));
}

static void click(WINDOW* self, int x, int y, int clicks) {
    int index = top_row + y / WINDOW_CHAR_H;

    (void)self;
    (void)x;
    if (index < 0 || index >= entry_count) return;
    selected = index;
    mizu->repaint();
    /* Double click opens, single selects - which is what a browser does, and
       what the icons in the control panel had to be taught. */
    if (clicks >= 2) open_entry(index);
}

static void key(WINDOW* self, int pressed) {
    (void)self;
    switch (pressed) {
    case KOI_KEY_UP: if (selected > 0) selected--; break;
    case KOI_KEY_DOWN: if (selected + 1 < entry_count) selected++; break;
    case KOI_KEY_HOME: selected = 0; break;
    case KOI_KEY_END: selected = entry_count - 1; break;
    case KOI_KEY_PAGE_UP: selected -= 10; if (selected < 0) selected = 0; break;
    case KOI_KEY_PAGE_DOWN:
        selected += 10;
        if (selected >= entry_count) selected = entry_count - 1;
        break;
    case '\n': case '\r': open_entry(selected); return;
    case '\b': go_up(); return;
    default: return;
    }
    mizu->repaint();
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case FILES_UP: go_up(); break;
    case FILES_REFRESH: read_place(); mizu->repaint(); break;
    case FILES_CLOSE:
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
    window = mizu->window_new("Files", 120, 90, 520, 380);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->click = click;
    window->key = key;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "File",
        { { "Up one level", FILES_UP }, { "Refresh", FILES_REFRESH },
          { 0, 0 }, { "Close", FILES_CLOSE } }, 4 };

    /* Opens on the drive the machine booted from, at its root.
     *
     * Not on the shell's current directory, because there is no call that
     * asks for one - and inventing a kernel call for the sake of an opening
     * position would be the wrong reason to add one. The root of the system
     * volume is where everything is anyway. */
    {
        long count = koi_sysinfo(KOI_INFO_VOLUME_COUNT, 0);
        char root[8];

        root[0] = 0;
        for (long index = 0; index < count; index++) {
            if (koi_sysinfo(KOI_INFO_VOLUME_IS_CURRENT, index) != 1) continue;
            root[0] = (char)koi_sysinfo(KOI_INFO_VOLUME_LETTER, index);
            root[1] = ':';
            root[2] = '\\';
            root[3] = 0;
            break;
        }
        go_to(root);
    }
    return window;
}

static MIZU_APP me = { "Files", 1, open, menu, closing };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    /* 4 brought open_file, which is how a picture opens in a window rather
       than in a program that takes the screen. */
    if (!api || api->version < 4) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
