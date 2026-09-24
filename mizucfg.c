#include "koi.h"
#include "dialog.h"
#include "settings.h"
#include "language.h"

/* The questions Mizu asks the first time.
 *
 * On the console rather than in Mizu's own windows, and that is not laziness:
 * these are asked before Mizu has ever drawn anything, on a machine where the
 * thing being configured is the thing that would have to draw the dialogue.
 *
 * The language comes first, and everything after it is asked in that language.
 * Asking somebody in English which language they read is a small rudeness that
 * every installer commits and none of them needs to.
 */

#define AUTOEXEC "\\AUTOEXEC.BAT"

/* Where the desktop is named now.
 *
 * It used to be a line in AUTOEXEC.BAT - `RING3 \MIZU\MIZU` - which made the
 * desktop a command the shell happened to run last. The kernel has a setting
 * for it now, and that is a different statement: the machine starts the
 * desktop, the shell is underneath it, and a desktop that crashes is put back
 * on its feet rather than dropping somebody at a prompt they did not ask for.
 *
 * Its own file in the configuration directory, because a file there has one
 * owner and this one belongs to whichever desktop is installed. Two desktops
 * cannot both be the desktop; contending for one file says so honestly, and it
 * is the same contention they already had over one line of AUTOEXEC.BAT.
 *
 * The ring is the kernel's business now rather than ours: it starts whatever
 * this names at ring 3, in an address space of its own, which is where a
 * program that runs all day and runs everything else belongs.
 *
 * The old AUTOEXEC line is still recognised, and taking it out matters as much
 * as writing the new setting: a machine set up before this would otherwise
 * start the desktop twice. */
#define DESKTOP_CONFIG "\\BOOT\\CONFIG\\DESKTOP.CFG"
#define DESKTOP_SETTING "command = \\MIZU\\MIZU\r\n"
#define START_COMMAND "RING3 \\MIZU\\MIZU"
#define START_COMMAND_PLAIN "\\MIZU\\MIZU"
#define FILE_MAX 4096

static char file[FILE_MAX];

static long read_file(const char* path, char* into, long limit) {
    long handle = koi_open(path, OPEN_READ);
    long got;

    if (handle < 0) return -1;
    got = koi_read(handle, into, limit - 1);
    koi_close(handle);
    if (got < 0) got = 0;
    into[got] = 0;
    return got;
}

static int line_matches(const char* line, const char* command) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '@') line++;
    for (int index = 0; command[index]; index++)
        if (toupper((unsigned char)line[index]) !=
            toupper((unsigned char)command[index])) return 0;
    return 1;
}

/* Whether anything in the file names Mizu, wherever the file is. Deliberately
   loose: a person who edited the setting by hand and wrote a different ring or
   a drive letter still meant Mizu, and asking them again would be pedantry. */
static int names_mizu(const char* text) {
    for (long at = 0; text[at]; at++) {
        int matched = 1;
        const char* want = "MIZU";

        for (int index = 0; want[index]; index++)
            if (toupper((unsigned char)text[at + index]) != want[index]) {
                matched = 0;
                break;
            }
        if (matched) return 1;
    }
    return 0;
}

static int autostart_is_set(void) {
    long at = 0;

    if (read_file(DESKTOP_CONFIG, file, FILE_MAX) >= 0 && names_mizu(file))
        return 1;

    /* And the way it was said before there was a setting. */
    if (read_file(AUTOEXEC, file, FILE_MAX) < 0) return 0;
    while (file[at]) {
        long start = at;
        while (file[at] && file[at] != '\n') at++;
        if (line_matches(file + start, START_COMMAND)) return 1;
        if (line_matches(file + start, START_COMMAND_PLAIN)) return 1;
        if (file[at]) at++;
    }
    return 0;
}

/* Both graphical shells claim the same line of AUTOEXEC.BAT, and only one of
   them can have it: a machine that starts two full-screen programs at boot
   starts neither usefully. So turning this on takes the other one out. */
static int set_autostart(int wanted) {
    static char rebuilt[FILE_MAX];
    long out = 0;
    long at = 0;
    long handle;

    if (read_file(AUTOEXEC, file, FILE_MAX) < 0) file[0] = 0;

    while (file[at]) {
        long start = at;
        long length;

        while (file[at] && file[at] != '\n') at++;
        length = at - start;
        if (file[at]) at++;
        if (line_matches(file + start, START_COMMAND)) continue;
        if (line_matches(file + start, START_COMMAND_PLAIN)) continue;
        if (wanted && line_matches(file + start, "\\COMMANDER\\COMMANDER")) continue;
        if (!length) continue;
        for (long index = 0; index < length && out < FILE_MAX - 2; index++)
            rebuilt[out++] = file[start + index];
        rebuilt[out++] = '\n';
    }
    rebuilt[out] = 0;

    /* AUTOEXEC.BAT is rewritten either way, because either way the old line
       has to come out of it: with the setting written, a line that also starts
       the desktop would start a second one. */
    koi_remove(AUTOEXEC);
    handle = koi_open(AUTOEXEC, OPEN_WRITE);
    if (handle < 0) return 0;
    if (out && koi_write(handle, rebuilt, out) != out) {
        koi_close(handle);
        return 0;
    }
    koi_close(handle);

    if (!wanted) {
        /* Only if it is ours. Another desktop may have been installed since,
           and taking away its setting because somebody said No to ours is not
           what No meant. */
        if (read_file(DESKTOP_CONFIG, file, FILE_MAX) >= 0 && names_mizu(file))
            koi_remove(DESKTOP_CONFIG);
        return 1;
    }

    {
        const char* setting = DESKTOP_SETTING;
        long length = 0;

        while (setting[length]) length++;
        koi_remove(DESKTOP_CONFIG);
        handle = koi_open(DESKTOP_CONFIG, OPEN_WRITE);
        if (handle < 0) return 0;
        if (koi_write(handle, setting, length) != length) {
            koi_close(handle);
            return 0;
        }
        koi_close(handle);
    }
    return 1;
}

/* The questions, one row per language rather than through `say`: this program
   runs before the choice has been made, so it has to be able to speak all
   four languages at once. */
static const char* const welcome[LANGUAGE_COUNT] = {
    "Mizu is a desktop with windows. It is a package, not part of the system: "
    "it can be removed and what is left is the same Koi-DOS.\n"
    "A few questions before it is used for the first time.",
    "Mizu - это рабочий стол с окнами. Это пакет, а не часть системы: "
    "его можно удалить, и останется тот же Koi-DOS.\n"
    "Несколько вопросов перед первым запуском.",
    "Mizu - це робочий стіл з вікнами. Це пакет, а не частина системи: "
    "його можна видалити, і залишиться той самий Koi-DOS.\n"
    "Кілька питань перед першим запуском.",
    "Το Mizu είναι μια επιφάνεια εργασίας με παράθυρα. Είναι ένα πακέτο "
    "και όχι μέρος του συστήματος: μπορεί να αφαιρεθεί και το Koi-DOS "
    "που απομένει είναι το ίδιο.\n"
    "Μερικές ερωτήσεις πριν από την πρώτη χρήση."
};

static const char* const ask_autostart[LANGUAGE_COUNT] = {
    "Start Mizu automatically when the machine boots?\n"
    "Then the desktop is what this machine starts, and Koi-DOS is underneath "
    "it. Answering No later gives the prompt back.",
    "Запускать Mizu автоматически при загрузке машины?\n"
    "Тогда машина запускает рабочий стол, а Koi-DOS остаётся под ним. "
    "Ответ Нет позже вернёт приглашение командной строки.",
    "Запускати Mizu автоматично під час завантаження?\n"
    "Тоді машина запускає робочий стіл, а Koi-DOS лишається під ним. "
    "Відповідь Ні згодом поверне командний рядок.",
    "Να ξεκινά το Mizu αυτόματα κατά την εκκίνηση του υπολογιστή;\n"
    "Τότε το μηχάνημα ξεκινά την επιφάνεια εργασίας και το Koi-DOS βρίσκεται "
    "από κάτω. Αν αργότερα απαντήσετε Όχι, επιστρέφει η γραμμή εντολών."
};

static const char* const ask_sound[LANGUAGE_COUNT] = {
    "How loud should this machine be?",
    "Насколько громкой должна быть эта машина?",
    "Наскільки гучною має бути ця машина?",
    "Πόσο δυνατά πρέπει να ακούγεται αυτό το μηχάνημα;"
};

static const char* const levels[LANGUAGE_COUNT][3] = {
    { "Quiet", "Normal", "Loud" },
    { "Тихо", "Обычно", "Громко" },
    { "Тихо", "Звичайно", "Гучно" },
    { "Ήσυχα", "Κανονικά", "Δυνατά" }
};

static const char* const done[LANGUAGE_COUNT] = {
    "Mizu is ready. Type \\MIZU\\MIZU to start it, or run MIZUCFG again to "
    "change any of this.",
    "Mizu готов. Наберите \\MIZU\\MIZU, чтобы запустить, или запустите MIZUCFG "
    "снова, чтобы это изменить.",
    "Mizu готовий. Наберіть \\MIZU\\MIZU, щоб запустити, або запустіть MIZUCFG "
    "знову, щоб це змінити.",
    "Το Mizu είναι έτοιμο. Πληκτρολογήστε \\MIZU\\MIZU για να το ξεκινήσετε "
    "ή εκτελέστε ξανά το MIZUCFG για να αλλάξετε αυτές τις ρυθμίσεις."
};

static void preview_language(int selected) {
    language_preview(selected);
}

/* Cancel means cancel.
 *
 * It used to mean "skip this question": the button was drawn, and choosing it
 * moved on to the next question exactly as OK did with the default answer - a
 * Cancel with no way to cancel. Now it stops.
 *
 * What has already been answered stays answered, because those settings were
 * applied when they were given and undoing them would be undoing work somebody
 * deliberately did. What does not happen is the rest of the questions, and the
 * flag that says they were asked - so Mizu asks again next time rather than
 * quietly deciding on somebody's behalf.
 *
 * In the language chosen so far, which is the point of asking that one
 * first. */
static const char* const cancelled_text[LANGUAGE_COUNT] = {
    "Nothing else was changed. Anything already answered has been kept.\n"
    "Run MIZUCFG again to finish; until then Mizu will ask these questions "
    "the next time it starts.",
    "Больше ничего не изменено. Всё, что уже отвечено, сохранено.\n"
    "Запустите MIZUCFG снова, чтобы закончить; до тех пор Mizu будет "
    "задавать эти вопросы при каждом запуске.",
    "Більше нічого не змінено. Усе, що вже відповіли, збережено.\n"
    "Запустіть MIZUCFG знову, щоб завершити; доти Mizu ставитиме ці "
    "питання під час кожного запуску.",
    "Τίποτε άλλο δεν άλλαξε. Ό,τι έχει ήδη απαντηθεί διατηρήθηκε.\n"
    "Εκτελέστε ξανά το MIZUCFG για να ολοκληρώσετε· ως τότε το Mizu θα "
    "κάνει αυτές τις ερωτήσεις σε κάθε εκκίνηση."
};

static int cancelled(int language) {
    dialog_message("Mizu", cancelled_text[language]);
    dialog_end();
    return 0;
}

int main(void) {
    static const char* names[LANGUAGE_COUNT];
    static const int percent[] = { 25, 50, 100 };
    int language;
    int original_language;
    int autostart;
    int loudness;

    language_load();
    dialog_begin("Mizu 0.5");

    for (int index = 0; index < LANGUAGE_COUNT; index++)
        names[index] = language_name(index);

    original_language = language_current();

    language = dialog_menu("Language / Язык / Мова / Γλώσσα",
                           "Which language should this machine speak?",
                           names, 0, LANGUAGE_COUNT,
                           original_language,
                           preview_language);

    /* Cancelling the first question leaves the machine exactly as it was, and
       says so in the language it had before - not in the one being previewed
       when Cancel was chosen. */
    if (language < 0) {
        language_preview(original_language);
        return cancelled(original_language);
    }
    language_set(language);

    dialog_message("Mizu", welcome[language]);

    autostart = dialog_yesno(say(SAY_MENU_SYSTEM), ask_autostart[language],
                             autostart_is_set());
    if (autostart < 0) return cancelled(language);
    if (!set_autostart(autostart))
        dialog_message("AUTOEXEC.BAT",
                       "AUTOEXEC.BAT could not be written. Nothing else is "
                       "affected.");

    loudness = dialog_menu("Sound", ask_sound[language], levels[language], 0,
                           3, 1, 0);
    if (loudness < 0) return cancelled(language);
    {
        char text[8];
        koi_snprintf(text, sizeof(text), "%d", percent[loudness]);
        koi_sound_volume(percent[loudness] * 255 / 100);
        settings_set("SOUND", "volume", text);
    }

    settings_set("MIZU", "configured", "1");
    dialog_message("Mizu", done[language]);
    dialog_end();
    return 0;
}
