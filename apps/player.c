#include "mizu.h"
#include "wav.h"

/* The Player, as an application.
 *
 * The second one, and that is its whole reason for going second: an interface
 * shaped around one application is not an interface, it is that application
 * with extra steps. Nothing had to be added to MIZU_API to move this - a list,
 * a bar that can be clicked, a menu whose labels change while it is open, and
 * sound - which is the answer this port was asked for.
 *
 * Sound is a system call and needs nothing from the desktop, so all of that
 * moved unchanged. What went through the table is the drawing that has to
 * match the desktop it happens on, and the window it happens in.
 */

#define PLAYER_PAUSE 1
#define PLAYER_STOP 2
#define PLAYER_CLOSE 3

static const MIZU_API* mizu;
static WINDOW* window;

/* ---- The player ----------------------------------------------------------
 *
 * A list of the WAV files it can find, a bar, and the bar can be clicked.
 *
 * The bar is the whole point and it was impossible yesterday. The mixer walks
 * a sound in 32.32 fixed point so that a recording made at one rate can play
 * at another, and the whole part of that number is how far in it has got - it
 * always knew, and nothing had ever asked. Three calls later there is a
 * position, a length and a seek, and a progress bar is arithmetic.
 *
 * The samples stay in memory for as long as the sound plays, because the mixer
 * reads them where they are rather than copying them. Freeing the buffer while
 * a voice still points into it is the one way to make this crash, so the
 * buffer is freed when the voice is stopped and never before.
 */
#define PLAYER_FILES 64
#define PLAYER_PATH 96

/* Whole files, in memory, for as long as they play.
 *
 * The mixer reads the samples where they are rather than copying them, so a
 * track is resident from the moment it starts until it is stopped. There is no
 * streaming: nothing in the audio interface can ask a program for more samples
 * partway through, and inventing that is a bigger change than a player.
 *
 * So the limit is memory, and it used to be a made-up four megabytes - which
 * at CD rates is twenty-three seconds, and is the sort of number that gets
 * written once and then quietly decides what the software is for. SYS_ALLOC
 * goes straight to the page allocator, so what is actually available is most
 * of the machine. A file is now measured before it is read and given exactly
 * what it needs, up to half of what is free - half, so that starting a long
 * track cannot leave the rest of the system with nothing. */
static char tracks[PLAYER_FILES][PLAYER_PATH];
static char player_message[80];
static int track_count;
static int track_playing = -1;
static void* track_data;
static unsigned int track_data_at;
static int voice = -1;
static WAV_FORMAT voice_format;
static unsigned int voice_frames;
static unsigned int player_paused_frame;
static int player_paused;

static void player_sync_pause_label(void) {
    if (window)
        window->menus[0].items[0].label = player_paused ? "Resume" : "Pause";
}

static void player_stop(void) {
    if (voice >= 0) koi_sound_stop(voice);
    voice = -1;
    if (track_data) { koi_free(track_data); track_data = 0; }
    track_playing = -1;
    player_paused = 0;
    player_paused_frame = 0;
    player_sync_pause_label();
}

static int player_start_from(unsigned int start_frame) {
    if (start_frame >= voice_frames) return -1;
    voice = koi_sound_play_simple((const char*)track_data + track_data_at,
                                  voice_frames, voice_format.rate,
                                  voice_format.bits == 16 ? KOI_SOUND_S16
                                                          : KOI_SOUND_U8,
                                  voice_format.channels, 255);
    if (voice < 0) return -1;
    if (start_frame && koi_sound_seek(voice, start_frame) < 0) {
        koi_sound_stop(voice);
        voice = -1;
        return -1;
    }
    return 0;
}

static void player_pause(void) {
    if (voice < 0 || !koi_sound_active(voice) || !track_data) return;
    player_paused_frame = koi_sound_where(voice);
    if (player_paused_frame >= voice_frames && voice_frames)
        player_paused_frame = voice_frames - 1;
    koi_sound_stop(voice);
    voice = -1;
    player_paused = 1;
    player_message[0] = 0;
    player_sync_pause_label();
}

static void player_resume(void) {
    if (!player_paused || !track_data) return;
    if (player_start_from(player_paused_frame) < 0) {
        koi_snprintf(player_message, sizeof(player_message),
                     "Every voice is busy");
        return;
    }
    player_paused = 0;
    player_paused_frame = 0;
    player_message[0] = 0;
    player_sync_pause_label();
}

static void player_toggle_pause(void) {
    if (player_paused) player_resume();
    else player_pause();
}

/* The root and the two directories somebody would actually keep music in.
   Not a file browser: a player that can only see the root is a player nobody
   can put a song in front of, and one that browses the disk is a different
   program. */
static void player_scan_in(const char* directory) {
    KOI_FIND_DATA found;
    char pattern[PLAYER_PATH];
    long search;

    koi_snprintf(pattern, sizeof(pattern), "%s*.WAV", directory);
    search = koi_findfirst(pattern, &found);
    if (search < 0) return;
    do {
        if (track_count >= PLAYER_FILES) break;
        koi_snprintf(tracks[track_count], PLAYER_PATH, "%s%s", directory,
                     found.name);
        track_count++;
    } while (koi_findnext(search, &found) == 0);
    koi_findclose(search);
}

static void player_scan(void) {
    track_count = 0;
    player_scan_in("\\");
    player_scan_in("\\MUSIC\\");
    player_scan_in("\\WAV\\");
}

/* The last component of a path, which is what a list wants to show. */
static const char* basename_of(const char* path) {
    const char* last = path;
    for (int at = 0; path[at]; at++)
        if (path[at] == '\\') last = path + at + 1;
    return last;
}

static void player_play(int index) {
    long handle;
    long size;
    long got;
    long affordable;
    unsigned int data_at = 0;
    unsigned int data_size;
    const char* why;

    if (index < 0 || index >= track_count) return;
    player_stop();
    player_message[0] = 0;

    handle = koi_open(tracks[index], OPEN_READ);
    if (handle < 0) {
        koi_snprintf(player_message, sizeof(player_message),
                     "Could not open %s", basename_of(tracks[index]));
        return;
    }

    size = koi_filesize(handle);
    /* KOI_INFO_MEMORY_FREE is in KiB. Half of it, so that playing something
       long does not leave the machine with nothing for anything else. */
    affordable = koi_sysinfo(KOI_INFO_MEMORY_FREE, 0) / 2 * 1024;
    if (size <= 0) {
        koi_close(handle);
        koi_snprintf(player_message, sizeof(player_message), "%s is empty",
                     basename_of(tracks[index]));
        return;
    }
    if (size > affordable) {
        koi_close(handle);
        /* Said with both numbers. "Out of memory" leaves somebody guessing
           whether a slightly smaller file would have worked. */
        koi_snprintf(player_message, sizeof(player_message),
                     "%s is %ld KiB and only %ld KiB can be spared",
                     basename_of(tracks[index]), size / 1024,
                     affordable / 1024);
        return;
    }

    track_data = koi_alloc(size);
    if (!track_data) {
        koi_close(handle);
        koi_snprintf(player_message, sizeof(player_message),
                     "No room for %ld KiB", size / 1024);
        return;
    }
    got = koi_read(handle, track_data, size);
    koi_close(handle);
    if (got <= 0) { player_stop(); return; }

    data_size = wav_parse((const unsigned char*)track_data, (unsigned int)got,
                          &voice_format, &data_at, &why);
    if (!data_size) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message), "%s: %s",
                     basename_of(tracks[index]), why);
        return;
    }

    track_data_at = data_at;
    voice_frames = data_size /
        (unsigned int)(voice_format.channels * (voice_format.bits / 8));
    if (!voice_frames) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message),
                     "%s has no samples in it", basename_of(tracks[index]));
        return;
    }

    if (player_start_from(0) < 0) {
        player_stop();
        koi_snprintf(player_message, sizeof(player_message),
                     "Every voice is busy");
        return;
    }
    track_playing = index;
}

#define BAR_TOP 8
#define BAR_HEIGHT 18
#define LIST_TOP 56

static void clock_text(char* out, koi_uint64 size, unsigned int frames,
                       unsigned int rate) {
    unsigned int seconds = rate ? frames / rate : 0;
    koi_snprintf(out, size, "%u:%02u", seconds / 60, seconds % 60);
}

static void paint_player(WINDOW* window, int x, int y, int width, int height) {
    char line[64];
    char left[16];
    char right[16];
    unsigned int at = voice >= 0 ? koi_sound_where(voice)
                                 : (player_paused ? player_paused_frame : 0);
    int rows = (height - LIST_TOP) / WINDOW_CHAR_H;

    (void)window;

    /* The bar. Drawn even when nothing is playing, because a control that
       appears only once it is useful is a control nobody finds. */
    mizu->sunken(x + 8, y + BAR_TOP, width - 16, BAR_HEIGHT);
    if ((voice >= 0 || player_paused) && voice_frames) {
        int span = (int)((koi_uint64)(width - 18) * at / voice_frames);
        koi_gfx_fill(x + 9, y + BAR_TOP + 1, span, BAR_HEIGHT - 2, mizu->color(MIZU_COLOR_ACCENT));
    }

    clock_text(left, sizeof(left), at, voice_format.rate);
    clock_text(right, sizeof(right), voice_frames, voice_format.rate);
    /* A message where the name goes, when there is one. A player that does
       nothing and says nothing is a player somebody thinks is broken. */
    if (player_message[0]) {
        mizu->label(x + 8, y + BAR_TOP + BAR_HEIGHT + 4, player_message,
                     mizu->color(MIZU_COLOR_SHADOW));
    } else {
        koi_snprintf(line, sizeof(line), "%s / %s   %s", left, right,
                     track_playing >= 0 ? basename_of(tracks[track_playing])
                                        : "");
        mizu->label(x + 8, y + BAR_TOP + BAR_HEIGHT + 4, line, mizu->color(MIZU_COLOR_TEXT));
    }

    for (int index = 0; index < track_count && index < rows; index++) {
        int row = y + LIST_TOP + index * WINDOW_CHAR_H;
        if (index == track_playing) {
            koi_gfx_fill(x + 4, row, width - 8, WINDOW_CHAR_H, mizu->color(MIZU_COLOR_ACCENT));
            mizu->label(x + 8, row, basename_of(tracks[index]),
                         mizu->color(MIZU_COLOR_PAPER));
        } else {
            mizu->label(x + 8, row, basename_of(tracks[index]), mizu->color(MIZU_COLOR_TEXT));
        }
    }
    if (!track_count)
        mizu->label(x + 8, y + LIST_TOP,
                     "No .WAV files in \\, \\MUSIC or \\WAV.", mizu->color(MIZU_COLOR_SHADOW));
}

static void click_player(WINDOW* window, int x, int y, int clicks) {
    int client_x, client_y, client_w, client_h;

    (void)window;
    (void)clicks;
    mizu->window_client(window, &client_x, &client_y, &client_w, &client_h);
    /* On the bar: seek. One click, not two - a bar is a place, and asking for
       a place twice is not a different request. */
    if (y >= BAR_TOP && y < BAR_TOP + BAR_HEIGHT) {
        if ((voice >= 0 || player_paused) && voice_frames && client_w > 18) {
            koi_uint64 frame = (koi_uint64)(x - 9) * voice_frames /
                               (koi_uint64)(client_w - 18);
            if (x < 9) frame = 0;
            if (frame >= voice_frames) frame = voice_frames - 1;
            if (voice >= 0) koi_sound_seek(voice, (unsigned int)frame);
            else player_paused_frame = (unsigned int)frame;
            mizu->repaint();
        }
        return;
    }

    if (y >= LIST_TOP) {
        int index = (y - LIST_TOP) / WINDOW_CHAR_H;
        if (index >= 0 && index < track_count) {
            player_play(index);
            mizu->repaint();
        }
    }
}


static void closing(WINDOW* self) {
    /* The samples are freed with the voice and never before: the mixer reads
       them where they are, so a buffer freed while a voice still points into
       it is the one way to make this crash. player_stop does both, in that
       order, and closing the window has to go through it. */
    if (self == window) {
        player_stop();
        window = (WINDOW*)0;
    }
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case PLAYER_PAUSE: player_toggle_pause(); mizu->repaint(); break;
    case PLAYER_STOP: player_stop(); mizu->repaint(); break;
    case PLAYER_CLOSE:
        player_stop();
        mizu->window_delete(window);
        window = (WINDOW*)0;
        break;
    default: break;
    }
}

static WINDOW* open(void) {
    if (window) {
        window->minimised = 0;
        mizu->window_raise(window);
        return window;
    }
    player_scan();
    window = mizu->window_new("Player", 340, 200, 400, 300);
    if (!window) return (WINDOW*)0;
    window->paint = paint_player;
    window->click = click_player;
    /* Four times a second: fast enough that the bar moves smoothly and slow
       enough that a desktop with a track playing is not repainting itself
       thirty times a second to move two pixels. */
    window->repaint_ms = 250;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "File",
        { { "Pause", PLAYER_PAUSE }, { "Stop", PLAYER_STOP }, { 0, 0 },
          { "Close", PLAYER_CLOSE } }, 4 };
    player_sync_pause_label();
    return window;
}

static MIZU_APP me = { "Player", 1, open, menu, closing };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < MIZU_API_VERSION) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
