#include "mizu.h"
#include "wav.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

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

/* ---- Songs, which do not fit in memory ----------------------------------
 *
 * A WAV is loaded whole and played as one voice; that is what audio_play is
 * for and it is right for a sound of a few seconds. An MP3 is a song: four
 * minutes of it is forty-six megabytes of samples, so it is decoded a frame
 * at a time - 1152 of them, twenty-six milliseconds - and handed to a stream
 * the kernel refills from.
 *
 * The decoding happens in the window's tick, a little on each pass, and never
 * in a loop of its own. A player that decoded a whole song before returning
 * would freeze the desktop for as long as the song lasts, which is the same
 * mistake the picture viewer made and the reason the desktop has a tick at
 * all.
 *
 * What is missing, and said out loud rather than half-done: pausing and
 * seeking. Both mean throwing away what has been queued and knowing where in
 * the file a given second is, and neither is five minutes' work. Stop and
 * start again is what this offers for a song. */
static mp3dec_t mp3;
static mp3dec_frame_info_t mp3_info;
static short mp3_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
static unsigned char mp3_input[16384];
static long mp3_have;                  /* bytes in the window */
static long mp3_at;                    /* how far into it we have decoded */
static long mp3_file = -1;
static int mp3_playing;
static unsigned long long mp3_frames;  /* queued so far, for the clock */
static long mp3_size;                  /* the file, for estimating length */
static unsigned int mp3_total_frames;  /* see mp3_measure */
/* The seek table out of the Xing header, when the file has one: entry i is
   where i per cent of the way through is, as a fraction of the file in 256ths.
   Empty when there is none, and then seeking is done by proportion. */
static unsigned char mp3_toc[100];
static int mp3_has_toc;
static void mp3_measure(const unsigned char* frame, long length, int rate);

static void mp3_close(void) {
    if (mp3_file >= 0) koi_close(mp3_file);
    mp3_file = -1;
    mp3_playing = 0;
    mp3_have = 0;
    mp3_at = 0;
    mp3_frames = 0;
}

static int ends_with_mp3(const char* path) {
    int length = 0;

    while (path[length]) length++;
    if (length < 4) return 0;
    return (path[length - 4] == '.') &&
           (path[length - 3] == 'M' || path[length - 3] == 'm') &&
           (path[length - 2] == 'P' || path[length - 2] == 'p') &&
           (path[length - 1] == '3');
}

static void player_stop(void) {
    mp3_close();
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
    /* A song is paused where it is: the kernel stops taking from the stream
       and the queue stays exactly as it is, so resuming continues on the same
       sample. A WAV is stopped and restarted from a frame, which is what it
       did before streams existed and is still right for a sound held whole in
       memory. */
    if (mp3_playing) {
        if (voice < 0) return;
        koi_sound_pause(voice, 1);
        player_paused = 1;
        player_message[0] = 0;
        player_sync_pause_label();
        return;
    }
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
    if (mp3_playing) {
        if (voice < 0) return;
        koi_sound_pause(voice, 0);
        player_paused = 0;
        player_message[0] = 0;
        player_sync_pause_label();
        return;
    }
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
static void player_scan_pattern(const char* directory, const char* suffix) {
    KOI_FIND_DATA found;
    char pattern[PLAYER_PATH];
    long search;

    koi_snprintf(pattern, sizeof(pattern), "%s*%s", directory, suffix);
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

static void player_scan_in(const char* directory) {
    player_scan_pattern(directory, ".WAV");
    player_scan_pattern(directory, ".MP3");
}

static void player_scan(void) {
    track_count = 0;
    player_scan_in("\\");
    player_scan_in("\\MUSIC\\");
    player_scan_in("\\WAV\\");
    player_scan_in("\\MP3\\");
}

/* The last component of a path, which is what a list wants to show. */
static const char* basename_of(const char* path) {
    const char* last = path;
    for (int at = 0; path[at]; at++)
        if (path[at] == '\\') last = path + at + 1;
    return last;
}

/* Decode a little and hand it over. Called from the window's tick, so the
   desktop keeps its turn between frames. Returns 0 when the song has ended. */
static int mp3_pump(void) {
    int rounds = 0;

    if (!mp3_playing || mp3_file < 0) return 0;

    /* While there is room for a frame and we have not done too many this
       pass. Eight frames is 208 ms of music decoded in a few milliseconds -
       enough to stay well ahead, little enough not to be felt. */
    /* Until the ring is full, or twenty-four frames - which is more than
       fills it from empty. The cap is not for pacing, it is so that a
       corrupt file cannot spin here for ever. */
    while (rounds < 24 && voice >= 0 && koi_sound_space(voice) > 1152) {
        int samples;

        if (mp3_have - mp3_at < 2048) {
            long moved = mp3_have - mp3_at;
            long got;

            for (long at = 0; at < moved; at++) mp3_input[at] = mp3_input[mp3_at + at];
            mp3_have = moved;
            mp3_at = 0;
            got = koi_read(mp3_file, mp3_input + mp3_have,
                           (long)sizeof(mp3_input) - mp3_have);
            if (got > 0) mp3_have += got;
            else if (mp3_have == 0) { player_stop(); return 0; }
        }

        samples = mp3dec_decode_frame(&mp3, mp3_input + mp3_at,
                                      (int)(mp3_have - mp3_at), mp3_pcm,
                                      &mp3_info);
        mp3_at += mp3_info.frame_bytes;
        if (!mp3_info.frame_bytes) { player_stop(); return 0; }
        if (!samples) continue;

        {
            int done = 0;

            while (done < samples) {
                int taken = koi_sound_queue(voice,
                                            mp3_pcm + done * mp3_info.channels,
                                            samples - done);
                if (taken <= 0) break;      /* full; the next tick continues */
                done += taken;
            }
            mp3_frames += (unsigned long long)done;
        }
        rounds++;
    }
    return 1;
}

static void player_play_mp3(int index) {
    int samples = 0;

    mp3_file = koi_open(tracks[index], OPEN_READ);
    if (mp3_file < 0) {
        koi_snprintf(player_message, sizeof(player_message),
                     "Could not open %s", basename_of(tracks[index]));
        return;
    }
    mp3dec_init(&mp3);
    mp3_have = 0;
    mp3_at = 0;
    mp3_frames = 0;
    mp3_size = koi_filesize(mp3_file);
    mp3_total_frames = 0;

    /* One frame decoded before anything is opened, because the rate and the
       number of channels are in the file rather than declared, and a stream
       has to be opened for the shape it is going to be fed. */
    {
        long got = koi_read(mp3_file, mp3_input, (long)sizeof(mp3_input));

        if (got <= 0) { mp3_close(); return; }
        mp3_have = got;
        while (mp3_at < mp3_have) {
            samples = mp3dec_decode_frame(&mp3, mp3_input + mp3_at,
                                          (int)(mp3_have - mp3_at), mp3_pcm,
                                          &mp3_info);
            mp3_at += mp3_info.frame_bytes;
            if (!mp3_info.frame_bytes) break;
            if (samples) break;
        }
    }
    if (!samples) {
        mp3_close();
        koi_snprintf(player_message, sizeof(player_message),
                     "%s: nothing this can decode", basename_of(tracks[index]));
        return;
    }

    voice = koi_sound_open((unsigned int)mp3_info.hz, KOI_SOUND_S16,
                           mp3_info.channels, 255);
    if (voice < 0) {
        mp3_close();
        koi_snprintf(player_message, sizeof(player_message),
                     "Every voice is busy");
        return;
    }
    voice_format.rate = (unsigned int)mp3_info.hz;
    voice_format.channels = (unsigned short)mp3_info.channels;
    voice_format.bits = 16;
    /* The length, estimated from the size and the rate it opened at. Used by
       the bar and the clock, and by seeking to turn a fraction back into a
       place in the file. */
    if (mp3_info.bitrate_kbps > 0 && mp3_size > 0)
        mp3_total_frames = (unsigned int)((koi_uint64)mp3_size * 8 *
                                          (koi_uint64)mp3_info.hz /
                                          ((koi_uint64)mp3_info.bitrate_kbps *
                                           1000));
    /* And what the file says about itself, which beats the estimate whenever
       it is there. */
    mp3_measure(mp3_input, mp3_have, mp3_info.hz);
    voice_frames = mp3_total_frames;
    mp3_playing = 1;
    track_playing = index;
    koi_snprintf(player_message, sizeof(player_message),
                 "%d kbit/s, %d Hz - stop and start again to restart",
                 mp3_info.bitrate_kbps, mp3_info.hz);

    {
        int done = 0;
        while (done < samples) {
            int taken = koi_sound_queue(voice, mp3_pcm + done * mp3_info.channels,
                                        samples - done);
            if (taken <= 0) break;
            done += taken;
        }
        mp3_frames += (unsigned long long)done;
    }
}

/* What a variable-bitrate file says about itself.
 *
 * The length was being worked out from the file size and the bitrate of the
 * first frame, which is exact for a file recorded at a constant rate and
 * badly wrong for one that is not: the first frame of a variable-rate file is
 * not music at all, it is a header - and it is written at a low rate, so
 * dividing by it made a four-minute song claim to be twelve.
 *
 * That header is the answer as well as the problem. "Xing" or "Info" sits
 * just past the side information of the first frame and carries the exact
 * number of frames in the file, and usually a table of a hundred entries
 * saying where each per cent of the way through begins. With it, both the
 * clock and the bar are right; without it, the old estimate is what there is.
 *
 * Searched for rather than computed from the side-information length, because
 * that length depends on the version and the channel mode, and a search of a
 * few hundred bytes for four characters cannot get it wrong. */
static void mp3_measure(const unsigned char* frame, long length, int rate) {
    long at;

    mp3_has_toc = 0;
    /* The whole of what was read, not the first two hundred bytes: a file
       with a picture in its ID3 tag pushes the first frame - and the header
       inside it - kilobytes in. The sanity check below is what keeps four
       letters inside a song title from being mistaken for it. */
    for (at = 0; at + 12 <= length; at++) {
        unsigned int flags;
        long field;

        if (!((frame[at] == 'X' && frame[at + 1] == 'i' &&
               frame[at + 2] == 'n' && frame[at + 3] == 'g') ||
              (frame[at] == 'I' && frame[at + 1] == 'n' &&
               frame[at + 2] == 'f' && frame[at + 3] == 'o'))) continue;

        field = at + 4;
        if (field + 4 > length) return;
        flags = ((unsigned int)frame[field] << 24) |
                ((unsigned int)frame[field + 1] << 16) |
                ((unsigned int)frame[field + 2] << 8) | frame[field + 3];
        /* Only four flags are defined, so anything else means these four
           letters were part of something that is not this header. */
        if (flags > 15) continue;
        field += 4;

        if (flags & 1) {                       /* the number of frames */
            unsigned int frames;

            if (field + 4 > length) return;
            frames = ((unsigned int)frame[field] << 24) |
                     ((unsigned int)frame[field + 1] << 16) |
                     ((unsigned int)frame[field + 2] << 8) | frame[field + 3];
            field += 4;
            /* 1152 samples per frame, which is what Layer III is. */
            if (frames) mp3_total_frames = frames * 1152;
        }
        if (flags & 2) field += 4;             /* the number of bytes */
        if (flags & 4) {                       /* the seek table */
            if (field + 100 > length) return;
            for (int index = 0; index < 100; index++)
                mp3_toc[index] = frame[field + index];
            mp3_has_toc = 1;
        }
        (void)rate;
        return;
    }
}

/* Where in the file a fraction of the way through is.
 *
 * A song's length is not written down anywhere this reads: an MP3 is frames
 * one after another, and the only way to know how many there are is to walk
 * them all. So it is estimated from the size and the bitrate, which is exact
 * for a file at a constant rate and close for one that varies - close enough
 * for a bar and a clock, and honest about being an estimate rather than
 * pretending to an accuracy it has not got.
 *
 * Seeking is the same arithmetic backwards: a fraction of the song is that
 * fraction of the file. The decoder is told to start again there and finds
 * the next frame header itself, which is what makes this work without an
 * index - MP3 was designed to be joined in the middle, because it was
 * designed for radio. */
static void mp3_seek(unsigned int frame) {
    long place;

    if (!mp3_playing || mp3_file < 0 || !mp3_total_frames) return;
    if (frame >= mp3_total_frames) frame = mp3_total_frames - 1;

    if (mp3_has_toc) {
        /* The file's own table: a hundred entries, one per cent each, saying
           where that per cent begins as a fraction of the file in 256ths.
           Interpolated between two of them, which is what everything else
           does and is accurate enough that a click lands on the bar where it
           was aimed. */
        koi_uint64 percent = (koi_uint64)frame * 100 / mp3_total_frames;
        unsigned int low;
        unsigned int high;
        koi_uint64 within;

        if (percent > 99) percent = 99;
        low = mp3_toc[percent];
        high = percent < 99 ? mp3_toc[percent + 1] : 256;
        within = (koi_uint64)frame * 100 % mp3_total_frames;
        place = (long)(((koi_uint64)mp3_size *
                        (low * mp3_total_frames + (high - low) * within)) /
                       (256ULL * mp3_total_frames));
    } else {
        place = (long)((koi_uint64)mp3_size * frame / mp3_total_frames);
    }
    if (place < 0) place = 0;
    if (koi_seek(mp3_file, place, KOI_SEEK_SET) < 0) return;

    /* Nothing of the old position survives: the decoder's overlap belongs to
       the frame before the one it was about to decode, and the queue belongs
       to the part of the song being left. */
    mp3dec_init(&mp3);
    mp3_have = 0;
    mp3_at = 0;
    mp3_frames = frame;
    koi_sound_flush(voice, frame);
    (void)mp3_pump();
}

/* Every quarter second: top the stream up if a song is playing. */
static void tick_player(WINDOW* self) {
    (void)self;
    if (mp3_playing) (void)mp3_pump();
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

    if (ends_with_mp3(tracks[index])) { player_play_mp3(index); return; }

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
/* The buttons, under the bar rather than in a menu.
 *
 * Pause lived in the File menu, which is where a thing goes when nobody has
 * decided where it belongs: pausing is not a file operation, it is the second
 * most-used control in a player and it was two clicks and a menu away. Under
 * the bar it is beside the thing it acts on. */
#define BUTTON_TOP (BAR_TOP + BAR_HEIGHT + 6)
#define BUTTON_HEIGHT 20
#define BUTTON_WIDTH 74
#define LIST_TOP (BUTTON_TOP + BUTTON_HEIGHT + 8)

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

    /* Pause and Stop, drawn as buttons and pressed like them. */
    {
        const char* label = player_paused ? "Resume" : "Pause";
        int text_left;

        mizu->raised(x + 8, y + BUTTON_TOP, BUTTON_WIDTH, BUTTON_HEIGHT);
        text_left = x + 8 + (BUTTON_WIDTH -
                             (int)strlen(label) * WINDOW_CHAR_W) / 2;
        mizu->label(text_left, y + BUTTON_TOP + 2, label,
                    mizu->color(MIZU_COLOR_TEXT));

        mizu->raised(x + 8 + BUTTON_WIDTH + 8, y + BUTTON_TOP, BUTTON_WIDTH,
                     BUTTON_HEIGHT);
        mizu->label(x + 8 + BUTTON_WIDTH + 8 +
                    (BUTTON_WIDTH - 4 * WINDOW_CHAR_W) / 2,
                    y + BUTTON_TOP + 2, "Stop", mizu->color(MIZU_COLOR_TEXT));
    }

    clock_text(left, sizeof(left), at, voice_format.rate);
    clock_text(right, sizeof(right), voice_frames, voice_format.rate);
    /* A message where the name goes, when there is one. A player that does
       nothing and says nothing is a player somebody thinks is broken. */
    if (player_message[0]) {
        mizu->label(x + 8 + 2 * (BUTTON_WIDTH + 8), y + BUTTON_TOP + 2,
                    player_message, mizu->color(MIZU_COLOR_SHADOW));
    } else {
        koi_snprintf(line, sizeof(line), "%s / %s   %s", left, right,
                     track_playing >= 0 ? basename_of(tracks[track_playing])
                                        : "");
        mizu->label(x + 8 + 2 * (BUTTON_WIDTH + 8), y + BUTTON_TOP + 2, line,
                    mizu->color(MIZU_COLOR_TEXT));
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
                     "No .WAV or .MP3 files in \\, \\MUSIC, \\WAV or \\MP3.", mizu->color(MIZU_COLOR_SHADOW));
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
            /* A song is sought by moving in the file and throwing the queue
               away; a sound held whole in memory is sought by moving the
               position the mixer reads. Different mechanisms, one bar. */
            if (mp3_playing) mp3_seek((unsigned int)frame);
            else if (voice >= 0) koi_sound_seek(voice, (unsigned int)frame);
            else player_paused_frame = (unsigned int)frame;
            mizu->repaint();
        }
        return;
    }

    if (y >= BUTTON_TOP && y < BUTTON_TOP + BUTTON_HEIGHT) {
        if (x >= 8 && x < 8 + BUTTON_WIDTH) {
            player_toggle_pause();
            mizu->repaint();
        } else if (x >= 8 + BUTTON_WIDTH + 8 &&
                   x < 8 + 2 * BUTTON_WIDTH + 8) {
            player_stop();
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
    /* And the tick is where a song is decoded: a little on each pass, never
       in a loop of its own. See mp3_pump. */
    window->tick = tick_player;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "File",
        { { "Pause", PLAYER_PAUSE }, { "Stop", PLAYER_STOP }, { 0, 0 },
          { "Close", PLAYER_CLOSE } }, 4 };
    player_sync_pause_label();
    return window;
}

/* Opened on a file, which is what the browser hands over: find it in the list
   it just scanned and start it. A player that opens with the file somebody
   double-clicked already playing is the only behaviour anybody expects; before
   this, a WAV from the browser fell through to the shell's `play`, took the
   whole screen, and came back - which is what "it opens in Koi-DOS instead of
   the player" was. */
static WINDOW* open_with(const char* path) {
    WINDOW* opened = open();

    if (!opened || !path || !path[0]) return opened;
    /* The browser says "Z:\\BEEP.WAV" and the scan says "\\BEEP.WAV": the same
       file, named from different places. The drive letter comes off both
       before they are compared, because a player that opens the right window
       and plays nothing is worse than one that does not open. */
    if (path[0] && path[1] == ':') path += 2;

    for (int index = 0; index < track_count; index++) {
        const char* have = tracks[index];
        int same = 1;

        if (have[0] && have[1] == ':') have += 2;

        for (int at = 0; ; at++) {
            char a = have[at];
            char b = path[at];

            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) { same = 0; break; }
            if (!a) break;
        }
        if (same) { player_play(index); break; }
    }
    mizu->repaint();
    return opened;
}

static MIZU_APP me = { "Player", 2, open, menu, closing, open_with };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    if (!api || api->version < MIZU_API_VERSION) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
