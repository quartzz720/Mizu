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
/* The number of times a disk has appeared or gone, as of the last look. A
   stick plugged in while this window is open changes it, and that is the
   moment to read the place again - a browser whose list of drives is a
   snapshot taken when it opened is a browser that lies. */
static long disks_seen;

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

static void read_place(void);

/* Has anything been plugged in or pulled out? Asking also lets the kernel
   look again, which it does only when nothing has a file open. */
static void check_disks(WINDOW* self) {
    long now = koi_sysinfo(KOI_INFO_DISK_GENERATION, 0);

    (void)self;
    if (now == disks_seen) return;
    disks_seen = now;
    read_place();
    mizu->repaint();
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

/* The full path of a row, which several things now need rather than one. */
static void path_of(const ENTRY* entry, char* path) {
    int at = 0;

    while (place[at] && at + 1 < PATH_MAX) { path[at] = place[at]; at++; }
    if (at && path[at - 1] != '\\' && at + 1 < PATH_MAX) path[at++] = '\\';
    path[at] = 0;
    for (int index = 0; entry->name[index] && at + 1 < PATH_MAX; index++)
        path[at++] = entry->name[index];
    path[at] = 0;
}

/* Whether a row is something to act on at all: the drives and `..` are places
   to go, not files to rename or delete. */
static int is_a_file_row(int index) {
    if (index < 0 || index >= entry_count) return 0;
    if (!place[0]) return 0;
    if (entries[index].name[0] == '.' && entries[index].name[1] == '.') return 0;
    return 1;
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

    path_of(entry, path);

    if (entry->directory) { go_to(path); return; }

    {
        char line[PATH_MAX + 16];

        /* An application first: it opens in a window and the desktop stays
           where it is. Only when nothing here opens that kind of file does
           this fall back to running a program, which takes the screen. */
        if (mizu->open_file(path)) return;

        /* A program is run; anything else this desktop has no application
         * for is opened in the notepad.
         *
         * It used to fall back to `edit`, the console editor, and that became
         * a way to stop the machine the day programs started being run with
         * their output collected rather than on the screen: a console editor
         * draws where nobody can see it and then waits for a keystroke, and
         * the desktop is stopped inside the call that started it. What
         * somebody sees is a machine that has died.
         *
         * The notepad is a window, needs no screen and blocks nothing. It is
         * also simply the better answer: opening an unknown file in a windowed
         * editor is what a desktop should do. */
        if (ends_with_ignoring_case(entry->name, ".EXE") ||
            ends_with_ignoring_case(entry->name, ".BAT"))
            koi_snprintf(line, sizeof(line), "%s", path);
        else {
            /* open_file above has already tried, and it falls back to the
               notepad for anything that is not a program - so reaching here
               with something that is not an .EXE or a .BAT means the desktop
               could not open it at all. */
            mizu->message("Files", "Nothing here opens that.",
                          mizu->say(DIALOG_OK));
            return;
        }

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

/* How many rows the last paint fitted, so that a press on the scrollbar can
   page by exactly what is on screen. */
static int view_rows = 1;

/* Whether the view should chase the selected row.
 *
 * It always did, on every paint - so scrolling with the bar was undone the
 * instant the window redrew itself, and the list would not move below the
 * selected file at all. The selection is chased when it moves, which is what
 * "keep the cursor in view" means; scrolling is the user looking somewhere
 * else, and looking somewhere else is allowed. */
static int follow_selection = 1;

static void paint(WINDOW* self, int x, int y, int width, int height) {
    int rows = (height - WINDOW_CHAR_H - 6) / WINDOW_CHAR_H;
    int list_width = width - WINDOW_SCROLLBAR_W;

    (void)self;
    if (rows < 1) rows = 1;
    if (list_width < 40) list_width = 40;
    view_rows = rows;
    if (follow_selection) {
        if (selected < top_row) top_row = selected;
        if (selected >= top_row + rows) top_row = selected - rows + 1;
        follow_selection = 0;
    }
    if (top_row + rows > entry_count) top_row = entry_count - rows;
    if (top_row < 0) top_row = 0;

    for (int row = 0; row < rows && top_row + row < entry_count; row++) {
        int index = top_row + row;
        ENTRY* entry = &entries[index];
        int line_y = y + row * WINDOW_CHAR_H;
        koi_uint32 ink = mizu->color(MIZU_COLOR_TEXT);

        if (index == selected) {
            koi_gfx_fill(x, line_y, list_width, WINDOW_CHAR_H,
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
            mizu->label(x + list_width - 8 - (int)strlen(size) * WINDOW_CHAR_W,
                        line_y, size, ink);
        }
    }

    mizu->scrollbar(x + width - WINDOW_SCROLLBAR_W, y, rows * WINDOW_CHAR_H,
                    top_row, rows, entry_count);

    /* The status line, at the bottom where every browser has kept it. */
    koi_gfx_line(x, y + height - WINDOW_CHAR_H - 4, x + width - 1,
                 y + height - WINDOW_CHAR_H - 4, mizu->color(MIZU_COLOR_SHADOW));
    mizu->label(x + 4, y + height - WINDOW_CHAR_H - 1, status,
                mizu->color(MIZU_COLOR_TEXT));
}

static void click(WINDOW* self, int x, int y, int clicks) {
    int index = top_row + y / WINDOW_CHAR_H;
    int client_x, client_y, client_w, client_h;

    mizu->window_client(self, &client_x, &client_y, &client_w, &client_h);
    if (x >= client_w - WINDOW_SCROLLBAR_W) {
        top_row = mizu->scrollbar_press(0, view_rows * WINDOW_CHAR_H, top_row,
                                        view_rows, entry_count, y);
        mizu->repaint();
        return;
    }
    if (index < 0 || index >= entry_count) return;
    selected = index;
    follow_selection = 1;
    mizu->repaint();
    /* Double click opens, single selects - which is what a browser does, and
       what the icons in the control panel had to be taught. */
    if (clicks >= 2) open_entry(index);
}

/* ---- What can be done to a file -----------------------------------------
 *
 * The right button, and what it offers is the short list somebody actually
 * wants on a file: open it, rename it, copy it somewhere, delete it. No cut
 * and paste yet - a clipboard for files is a second idea and this is the
 * first, and half of it shipped would be worse than none.
 *
 * Every one of these can fail, and each says why rather than saying that it
 * failed. "Cannot rename" is a message; "there is already a file called that"
 * is an answer.
 */
#define FILES_OPEN 10
#define FILES_RENAME 11
#define FILES_COPY 12
#define FILES_DELETE 13

/* Copy a file, through a buffer big enough to be worth the syscalls.
 *
 * Thirty-two kilobytes: large enough that a megabyte is thirty-two reads
 * rather than two thousand, small enough to ask the allocator for without
 * thinking. The desktop gets a turn between them, because copying from a USB
 * stick is seconds and a desktop that stops for them is the thing we just
 * finished fixing everywhere else. */
#define COPY_CHUNK 32768

static int copy_file(const char* from, const char* to) {
    long source;
    long target;
    char* buffer;
    int ok = 1;

    source = koi_open(from, OPEN_READ);
    if (source < 0) return 0;
    target = koi_open(to, OPEN_WRITE);
    if (target < 0) { koi_close(source); return 0; }

    buffer = (char*)koi_alloc(COPY_CHUNK);
    if (!buffer) { koi_close(source); koi_close(target); return 0; }

    for (;;) {
        long got = koi_read(source, buffer, COPY_CHUNK);
        if (got < 0) { ok = 0; break; }
        if (!got) break;
        if (koi_write(target, buffer, got) != got) { ok = 0; break; }
        mizu->yield();
    }

    koi_free(buffer);
    koi_close(source);
    koi_close(target);
    /* A half-written copy is worse than no copy: it looks like a file. */
    if (!ok) koi_remove(to);
    return ok;
}

static void rename_entry(int index) {
    char from[PATH_MAX];
    char to[PATH_MAX];
    char name[NAME_MAX];
    int at = 0;

    if (!is_a_file_row(index)) return;
    while (entries[index].name[at] && at + 1 < (int)sizeof(name)) {
        name[at] = entries[index].name[at];
        at++;
    }
    name[at] = 0;

    if (!mizu->prompt("Files", mizu->say(SAY_RENAME), mizu->say(DIALOG_OK),
                      mizu->say(DIALOG_CANCEL), name, (int)sizeof(name)))
        return;
    if (!name[0]) return;

    path_of(&entries[index], from);
    {
        int cut = 0;
        for (int scan = 0; from[scan]; scan++)
            if (from[scan] == '\\') cut = scan + 1;
        for (at = 0; at < cut && at + 1 < PATH_MAX; at++) to[at] = from[at];
        to[at] = 0;
        for (int scan = 0; name[scan] && at + 1 < PATH_MAX; scan++)
            to[at++] = name[scan];
        to[at] = 0;
    }

    if (koi_exists(to)) {
        mizu->message("Files", mizu->say(SAY_NAME_TAKEN), mizu->say(DIALOG_OK));
        return;
    }
    if (koi_rename(from, to) < 0)
        mizu->message("Files", mizu->say(SAY_CANNOT_RENAME), mizu->say(DIALOG_OK));
    read_place();
    mizu->repaint();
}

static void copy_entry(int index) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    if (!is_a_file_row(index)) return;
    if (entries[index].directory) {
        /* A directory is a whole tree, and a tree is a different piece of
           work with its own failure halfway through. Saying so beats
           copying the first level and looking finished. */
        mizu->message("Files", mizu->say(SAY_ONLY_FILES), mizu->say(DIALOG_OK));
        return;
    }

    path_of(&entries[index], from);
    koi_snprintf(to, sizeof(to), "%s", from);
    if (!mizu->prompt("Files", mizu->say(SAY_COPY_TO), mizu->say(DIALOG_OK),
                      mizu->say(DIALOG_CANCEL), to, (int)sizeof(to)))
        return;
    if (!to[0]) return;

    {
        int same = 1;
        for (int at = 0; ; at++) {
            char a = from[at], b = to[at];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) { same = 0; break; }
            if (!a) break;
        }
        if (same) return;
    }

    if (koi_exists(to) &&
        mizu->confirm("Files", mizu->say(SAY_OVERWRITE), mizu->say(DIALOG_OK),
                      mizu->say(DIALOG_CANCEL), 0) != 1)
        return;

    say_status(mizu->say(SAY_COPYING));
    mizu->repaint();
    if (!copy_file(from, to))
        mizu->message("Files", mizu->say(SAY_CANNOT_COPY), mizu->say(DIALOG_OK));
    read_place();
    mizu->repaint();
}

static void delete_entry(int index) {
    char path[PATH_MAX];

    if (!is_a_file_row(index)) return;
    if (entries[index].directory) {
        /* Only an empty one, which is what the call underneath does. A
           directory with things in it is a question this window cannot ask
           yet. */
        path_of(&entries[index], path);
        if (!mizu->confirm("Files", mizu->say(SAY_DELETE_ASK),
                           mizu->say(DIALOG_OK), mizu->say(DIALOG_CANCEL), 0))
            return;
        if (koi_remove(path) < 0)
            mizu->message("Files", mizu->say(SAY_CANNOT_DELETE),
                          mizu->say(DIALOG_OK));
        read_place();
        mizu->repaint();
        return;
    }

    path_of(&entries[index], path);
    if (mizu->confirm("Files", mizu->say(SAY_DELETE_ASK), mizu->say(DIALOG_OK),
                      mizu->say(DIALOG_CANCEL), 0) != 1)
        return;
    if (koi_remove(path) < 0)
        mizu->message("Files", mizu->say(SAY_CANNOT_DELETE), mizu->say(DIALOG_OK));
    read_place();
    mizu->repaint();
}

/* The right button on a row. */
static void context(WINDOW* self, int x, int y) {
    WINDOW_ITEM items[6];
    int count = 0;
    int index;
    int client_x, client_y, client_w, client_h;
    int chosen;

    (void)self;
    index = top_row + (y - 4) / WINDOW_CHAR_H;
    if (index < 0 || index >= entry_count) return;
    /* Pointing at it first, so the menu is plainly about the row under the
       pointer and not about whatever was selected before. */
    selected = index;
    mizu->repaint();

    items[count].label = mizu->say(SAY_OPEN);
    items[count++].id = FILES_OPEN;
    if (is_a_file_row(index)) {
        items[count].label = 0;
        items[count++].id = 0;
        items[count].label = mizu->say(SAY_RENAME);
        items[count++].id = FILES_RENAME;
        items[count].label = mizu->say(SAY_COPY);
        items[count++].id = FILES_COPY;
        items[count].label = mizu->say(SAY_DELETE);
        items[count++].id = FILES_DELETE;
    }

    mizu->window_client(window, &client_x, &client_y, &client_w, &client_h);
    chosen = mizu->context_menu(items, count, client_x + x, client_y + y);
    mizu->repaint();

    switch (chosen) {
    case FILES_OPEN: open_entry(index); break;
    case FILES_RENAME: rename_entry(index); break;
    case FILES_COPY: copy_entry(index); break;
    case FILES_DELETE: delete_entry(index); break;
    default: break;
    }
}

static void drag(WINDOW* self, int x, int y) {
    int client_x, client_y, client_w, client_h;

    mizu->window_client(self, &client_x, &client_y, &client_w, &client_h);
    if (x < client_w - WINDOW_SCROLLBAR_W - 8) return;
    top_row = mizu->scrollbar_drag(0, view_rows * WINDOW_CHAR_H, view_rows,
                                   entry_count, y);
    mizu->repaint();
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
    follow_selection = 1;
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
    window->drag = drag;
    window->key = key;
    /* Once a second is often enough to notice a USB stick and rare enough to
       cost nothing. */
    window->context = context;
    window->tick = check_disks;
    window->repaint_ms = 1000;
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
    /* 7 brought the context menu, and this window is built around having
       one now. Refusing a desktop that cannot provide it is the whole point
       of the number. */
    /* 8 brought the scrollbar, which this draws beside the list. */
    if (!api || api->version < 8) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
