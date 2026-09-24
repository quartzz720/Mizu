#include "mizu.h"
#include "language.h"
#include "png.h"
#include "gif.h"

/* Nami Explorer.
 *
 * 波 - a wave. Mizu is water; Nami is what moves across it. Koi named it, and
 * it named itself: this is the thing that goes out over the water and comes
 * back with something.
 *
 * ---- What it is ----------------------------------------------------------
 *
 * A browser of about 1997: it fetches a page over HTTP, reads its stylesheets,
 * lays the text out under them, and follows a link when it is clicked. There
 * is an address bar to type in, a history to go back and forward through, a
 * title in the window's title, and text in whatever alphabet the page was
 * written in.
 *
 * There are no scripts, and that is a decision rather than a gap. A script
 * engine is a second language, a second memory model and a second set of
 * security problems, and it buys nothing a page has to have: everything below
 * is a document being read, which is what the web was for before it was an
 * application platform. If a page needs Javascript to show its text, this
 * shows the text it has and says so.
 *
 * ---- What it cannot do, said plainly -------------------------------------
 *
 * https:// - there is no TLS in this system yet. In 2026 that is most of the
 * web, and pretending otherwise by drawing a padlock would be worse than
 * saying so. The address bar refuses it with a sentence rather than a silence.
 *
 * Pictures. <img> leaves the alternative text in brackets, because the system
 * can decode BMP and the web is PNG and JPEG. That is the next thing rather
 * than the impossible thing.
 *
 * ---- How a page becomes a screen -----------------------------------------
 *
 *   bytes  ->  UTF-8       the page says which alphabet it is in, and
 *                          everything above this line is one alphabet
 *   pass 1 ->  stylesheets what <style> holds and what <link> points at,
 *                          plus <title>
 *   rules  ->  cascade     including this browser's own stylesheet, which is
 *                          written in CSS rather than in C
 *   pass 2 ->  pieces      words, each carrying the style it computed to
 *   width  ->  lines       packed when the width is known, so a resize
 *                          re-wraps without re-reading anything
 *
 * The document is read twice because a stylesheet may be declared after the
 * text it styles, and nothing here reflows: a word's style is decided once,
 * when the word is made. Reading it twice is cheaper than being able to
 * change our mind later.
 *
 * Nothing is kept as a tree. The tree exists only while pass 2 is running -
 * as a stack of open elements, which is all a cascade needs and all a
 * document that no script will ever modify can be asked about.
 */

#define NAMI_CLOSE 1
#define NAMI_BACK 2
#define NAMI_FORWARD 3
#define NAMI_RELOAD 4
#define NAMI_GO 5
#define NAMI_HOME 6
#define NAMI_STYLES 7

/* What a page may be.
 *
 * It was 64 KiB, which sounds generous for a document and is not one for a
 * page: google.com is 84 KiB of which three quarters is script, and every one
 * of its seven links lives past the sixty-fourth kilobyte. The buffer filled
 * with Javascript nobody here will ever run, and the browser showed two lines
 * and stopped - looking, from the outside, exactly like a browser that cannot
 * read the page. */
#define PAGE_MAX (256 * 1024)
#define TEXT_MAX 32768          /* what is left after the tags come out */
#define CSS_MAX 16384
#define PIECES_MAX 6144
#define LINES_MAX 4096
#define LINKS_MAX 512
#define LINK_POOL 24576
#define RULES_MAX 192
#define STYLES_MAX 128
#define STACK_MAX 24
#define HOTSPOTS_MAX 512
#define URL_MAX 256
#define HISTORY_MAX 32
#define SHEETS_MAX 4
#define NO_LINK 0xFFFF

static const MIZU_API* mizu;
static WINDOW* window;

/* ---- Small things everything else needs ---------------------------------- */

static int same_ignoring_case(const char* a, const char* b, int length) {
    for (int at = 0; at < length; at++) {
        char left = a[at];
        char right = b[at];

        if (left >= 'A' && left <= 'Z') left = (char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (char)(right + 32);
        if (left != right) return 0;
    }
    return 1;
}

static int same_word(const char* a, const char* b) {
    int length = (int)strlen(b);

    if ((int)strlen(a) != length) return 0;
    return same_ignoring_case(a, b, length);
}

static int ends_with_ignoring_case(const char* text, const char* ending) {
    int length = (int)strlen(text);
    int wanted = (int)strlen(ending);

    if (length < wanted) return 0;
    return same_ignoring_case(text + length - wanted, ending, wanted);
}

static void copy_text(char* into, int size, const char* from) {
    int at = 0;

    while (from[at] && at + 1 < size) { into[at] = from[at]; at++; }
    into[at] = 0;
}

static void lower_text(char* text) {
    for (int at = 0; text[at]; at++)
        if (text[at] >= 'A' && text[at] <= 'Z') text[at] = (char)(text[at] + 32);
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* How wide a run of UTF-8 is on screen, in character cells.
 *
 * Not its length in bytes, which is what this counted before there was any
 * Russian on screen: `привет` is six letters and twelve bytes, and a line
 * packed by bytes wrapped after three words and left half the window empty.
 * One code point is one cell here, because the font is one width. */
static int cells_of(const char* text, int length) {
    int cells = 0;

    for (int at = 0; at < length; ) {
        unsigned char lead = (unsigned char)text[at];

        if (lead < 0x80) at += 1;
        else if ((lead & 0xE0) == 0xC0) at += 2;
        else if ((lead & 0xF0) == 0xE0) at += 3;
        else if ((lead & 0xF8) == 0xF0) at += 4;
        else at += 1;
        cells++;
    }
    return cells;
}

/* How many bytes the first `cells` cells of a run occupy - the other
   direction, for putting a caret somewhere in the middle of a line. */
static int bytes_for_cells(const char* text, int cells) {
    int at = 0;

    while (cells > 0 && text[at]) {
        unsigned char lead = (unsigned char)text[at];

        if (lead < 0x80) at += 1;
        else if ((lead & 0xE0) == 0xC0) at += 2;
        else if ((lead & 0xF0) == 0xE0) at += 3;
        else if ((lead & 0xF8) == 0xF0) at += 4;
        else at += 1;
        cells--;
    }
    return at;
}

static int write_utf8(char* out, unsigned int codepoint) {
    if (codepoint < 0x80) { out[0] = (char)codepoint; return 1; }
    if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

/* ---- Style ---------------------------------------------------------------
 *
 * What a word looks like, which is everything the cascade computes and the
 * only thing the layout below is told. Deliberately flat: there are no boxes
 * here, no margins that collapse and no floats, because there is one column
 * of text and a font of one size. What is here is what changes what a reader
 * sees on a screen of characters.
 */

#define ALIGN_LEFT 0
#define ALIGN_CENTRE 1
#define ALIGN_RIGHT 2

typedef struct {
    koi_uint32 color;
    koi_uint32 background;
    unsigned char has_background;
    unsigned char bold;
    unsigned char italic;
    unsigned char underline;
    unsigned char align;
    unsigned char indent;        /* left margin, in characters */
    unsigned char pre;           /* white-space: pre */

    /* The box half of CSS, which this had none of: a block can have a line
       round it, a colour behind it, room inside that line, and a width of its
       own. Without these a page is a column of words - which is what every
       page looked like here, however carefully its author had drawn it. */
    koi_uint32 border;
    unsigned char has_border;
    unsigned char padding;       /* inside the border, in characters */
    unsigned char width;         /* in characters, 0 for "as wide as it can" */
} STYLE;

static STYLE styles[STYLES_MAX];
static int style_count;

/* Styles are shared rather than stored per word: a page of ten thousand words
   has perhaps a dozen appearances, and a table of them turns a style into two
   bytes on a piece. */
static unsigned short intern_style(const STYLE* style) {
    for (int at = 0; at < style_count; at++)
        if (!memcmp(&styles[at], style, sizeof(STYLE)))
            return (unsigned short)at;
    if (style_count >= STYLES_MAX) return 0;
    styles[style_count] = *style;
    return (unsigned short)style_count++;
}

/* ---- Colours -------------------------------------------------------------
 *
 * The named ones a page of this age actually uses, plus the three of the
 * desktop's own theme. `color: text` is not CSS and no page will write it -
 * it is how this browser's own stylesheet says "whatever the theme calls
 * text", so that a page which sets no colours looks like the rest of Mizu
 * and follows it when the theme changes.
 */

typedef struct { const char* name; koi_uint32 value; } COLOUR_NAME;

static const COLOUR_NAME colour_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 },
    { "green", 0x008000 }, { "lime", 0x00FF00 }, { "blue", 0x0000FF },
    { "navy", 0x000080 }, { "yellow", 0xFFFF00 }, { "olive", 0x808000 },
    { "cyan", 0x00FFFF }, { "aqua", 0x00FFFF }, { "teal", 0x008080 },
    { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "purple", 0x800080 },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB },
    { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC },
    { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C }, { "salmon", 0xFA8072 },
    { "tan", 0xD2B48C }, { "violet", 0xEE82EE }, { "indigo", 0x4B0082 },
    { "darkblue", 0x00008B }, { "darkred", 0x8B0000 },
    { "darkgreen", 0x006400 }, { "darkgray", 0xA9A9A9 },
    { "darkgrey", 0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "lightblue", 0xADD8E6 },
    { "whitesmoke", 0xF5F5F5 }, { "transparent", 0 },
    { 0, 0 }
};

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_colour(const char* value, koi_uint32* out) {
    if (value[0] == '#') {
        int digits[6];
        int count = 0;

        while (count < 6 && hex_digit(value[count + 1]) >= 0) {
            digits[count] = hex_digit(value[count + 1]);
            count++;
        }
        if (count >= 6) {
            *out = (koi_uint32)((digits[0] << 20) | (digits[1] << 16) |
                                (digits[2] << 12) | (digits[3] << 8) |
                                (digits[4] << 4) | digits[5]);
            return 1;
        }
        if (count >= 3) {
            /* #abc is #aabbcc, which is a shorthand and not a shorter
               colour - a page that writes #f00 means red, not #0f0000. */
            *out = (koi_uint32)((digits[0] << 20) | (digits[0] << 16) |
                                (digits[1] << 12) | (digits[1] << 8) |
                                (digits[2] << 4) | digits[2]);
            return 1;
        }
        return 0;
    }
    if (same_ignoring_case(value, "rgb", 3)) {
        int channel[3] = { 0, 0, 0 };
        int at = 3;
        int which = 0;

        while (value[at] && value[at] != '(') at++;
        if (value[at]) at++;
        while (value[at] && which < 3) {
            if (value[at] >= '0' && value[at] <= '9') {
                int number = 0;

                while (value[at] >= '0' && value[at] <= '9')
                    number = number * 10 + (value[at++] - '0');
                if (value[at] == '%') { number = number * 255 / 100; at++; }
                channel[which++] = number > 255 ? 255 : number;
                continue;
            }
            at++;
        }
        *out = (koi_uint32)((channel[0] << 16) | (channel[1] << 8) | channel[2]);
        return 1;
    }
    if (same_word(value, "text")) { *out = mizu->color(MIZU_COLOR_TEXT); return 1; }
    if (same_word(value, "paper")) { *out = mizu->color(MIZU_COLOR_PAPER); return 1; }
    if (same_word(value, "accent")) { *out = mizu->color(MIZU_COLOR_ACCENT); return 1; }
    if (same_word(value, "shadow")) { *out = mizu->color(MIZU_COLOR_SHADOW); return 1; }

    for (int at = 0; colour_names[at].name; at++)
        if (same_word(value, colour_names[at].name)) {
            *out = colour_names[at].value;
            return 1;
        }
    return 0;
}

/* ---- Declarations --------------------------------------------------------
 *
 * A block of properties as written, with a bit per property saying it was
 * written at all. That bit is the whole of why a cascade works: a rule that
 * says only `color` must not quietly set everything else back to its default
 * when it is applied over one that set the weight.
 */

#define DECL_COLOR      0x001
#define DECL_BACKGROUND 0x002
#define DECL_WEIGHT     0x004
#define DECL_ITALIC     0x008
#define DECL_DECORATION 0x010
#define DECL_ALIGN      0x020
#define DECL_DISPLAY    0x040
#define DECL_MARGIN     0x080
#define DECL_PRE        0x100
#define DECL_SPACE      0x200
#define DECL_BORDER     0x400
#define DECL_PADDING    0x800
#define DECL_WIDTH      0x1000

#define DISPLAY_INLINE 0
#define DISPLAY_BLOCK 1
#define DISPLAY_NONE 2

typedef struct {
    unsigned int set;
    koi_uint32 color;
    koi_uint32 background;
    unsigned char bold;
    unsigned char italic;
    unsigned char underline;
    unsigned char align;
    unsigned char display;
    unsigned char margin;        /* characters, added to what is inherited */
    unsigned char pre;
    unsigned char space;         /* blank lines above the block */
    koi_uint32 border;
    unsigned char has_border;
    unsigned char padding;
    unsigned char width;
} DECL;

/* A length in whatever unit it was written in, as a count of characters.
 *
 * The font is eight pixels wide and sixteen tall, so a pixel measurement is
 * divided and an em is one character. Nothing here can be finer than a
 * character, which is the honest limit of a screen made of them: 12px and
 * 15px are both one character of indent, and a page that depends on the
 * difference was never going to work on this. */
static int length_in_characters(const char* value) {
    int number = 0;
    int at = 0;

    while (value[at] == ' ') at++;
    if (value[at] == '-') return 0;                     /* nothing goes left */
    while (value[at] >= '0' && value[at] <= '9')
        number = number * 10 + (value[at++] - '0');
    if (value[at] == '.') { at++; while (value[at] >= '0' && value[at] <= '9') at++; }

    if (same_ignoring_case(value + at, "px", 2)) number = (number + 4) / 8;
    else if (same_ignoring_case(value + at, "%", 1)) number = number / 6;
    else if (same_ignoring_case(value + at, "pt", 2)) number = (number + 3) / 6;
    /* em, ex, rem, ch and a bare number all count as characters. */

    if (number > 40) number = 40;
    return number;
}

/* Font size, which this cannot honour and can acknowledge: there is one
   size, so anything bigger than normal is drawn bold and anything smaller is
   drawn as it is. A heading that loses its size but keeps its weight still
   reads as a heading; one drawn identically to the paragraph under it does
   not. */
static int size_is_large(const char* value) {
    int number = 0;
    int at = 0;

    if (same_ignoring_case(value, "large", 5) ||
        same_ignoring_case(value, "x-large", 7) ||
        same_ignoring_case(value, "xx-large", 8) ||
        same_ignoring_case(value, "larger", 6)) return 1;

    while (value[at] >= '0' && value[at] <= '9')
        number = number * 10 + (value[at++] - '0');
    if (same_ignoring_case(value + at, "px", 2)) return number >= 20;
    if (same_ignoring_case(value + at, "pt", 2)) return number >= 15;
    if (same_ignoring_case(value + at, "%", 1)) return number >= 130;
    if (value[at] == '.' || same_ignoring_case(value + at, "em", 2))
        return number >= 2;
    return 0;
}

static void set_property(DECL* into, const char* name, const char* value) {
    if (same_word(name, "color")) {
        if (parse_colour(value, &into->color)) into->set |= DECL_COLOR;
        return;
    }
    if (same_word(name, "background") ||
        same_word(name, "background-color")) {
        /* `background` may carry a picture and a position as well; the colour
           is whichever word of it is one, and the rest cannot be drawn. */
        koi_uint32 colour;

        if (parse_colour(value, &colour)) {
            into->background = colour;
            into->set |= DECL_BACKGROUND;
        }
        return;
    }
    if (same_word(name, "font-weight")) {
        int number = 0;

        for (int at = 0; value[at] >= '0' && value[at] <= '9'; at++)
            number = number * 10 + (value[at] - '0');
        into->bold = (unsigned char)(same_word(value, "bold") ||
                                     same_word(value, "bolder") ||
                                     number >= 600);
        into->set |= DECL_WEIGHT;
        return;
    }
    if (same_word(name, "font-style")) {
        into->italic = (unsigned char)(same_word(value, "italic") ||
                                       same_word(value, "oblique"));
        into->set |= DECL_ITALIC;
        return;
    }
    if (same_word(name, "font-size")) {
        if (size_is_large(value)) { into->bold = 1; into->set |= DECL_WEIGHT; }
        return;
    }
    if (same_word(name, "font")) {
        /* The shorthand, of which only the size is legible here. */
        if (size_is_large(value)) { into->bold = 1; into->set |= DECL_WEIGHT; }
        return;
    }
    if (same_word(name, "text-decoration") ||
        same_word(name, "text-decoration-line")) {
        into->underline = (unsigned char)(same_ignoring_case(value, "underline", 9));
        into->set |= DECL_DECORATION;
        return;
    }
    if (same_word(name, "text-align")) {
        if (same_word(value, "center") || same_word(value, "centre"))
            into->align = ALIGN_CENTRE;
        else if (same_word(value, "right")) into->align = ALIGN_RIGHT;
        else into->align = ALIGN_LEFT;
        into->set |= DECL_ALIGN;
        return;
    }
    if (same_word(name, "display")) {
        if (same_word(value, "none")) into->display = DISPLAY_NONE;
        else if (same_word(value, "inline")) into->display = DISPLAY_INLINE;
        else into->display = DISPLAY_BLOCK;
        into->set |= DECL_DISPLAY;
        return;
    }
    if (same_word(name, "visibility")) {
        if (same_word(value, "hidden")) {
            into->display = DISPLAY_NONE;
            into->set |= DECL_DISPLAY;
        }
        return;
    }
    if (same_word(name, "border") || same_word(name, "border-color") ||
        same_word(name, "border-style") || same_word(name, "border-top") ||
        same_word(name, "border-bottom") || same_word(name, "border-left") ||
        same_word(name, "border-right")) {
        /* `border: 5px ridge #5BCEFA` is a width, a manner and a colour in
         * one, and of the three only the colour can be drawn here: a line is
         * one pixel and ridged is not a thing a single pixel can be. So the
         * colour is looked for anywhere in the value, and the rest is read as
         * "there is a border".
         *
         * `border: none` is the exception that matters, because a page that
         * turns a border off means it. */
        koi_uint32 colour;
        int at = 0;

        if (same_ignoring_case(value, "none", 4) ||
            same_ignoring_case(value, "0", 1)) {
            into->has_border = 0;
            into->set |= DECL_BORDER;
            return;
        }
        into->has_border = 1;
        into->border = 0x808080;
        while (value[at]) {
            char word[32];
            int out = 0;

            while (value[at] == ' ') at++;
            while (value[at] && value[at] != ' ' && out + 1 < (int)sizeof(word))
                word[out++] = value[at++];
            word[out] = 0;
            if (!out) break;
            if (parse_colour(word, &colour)) into->border = colour;
        }
        into->set |= DECL_BORDER;
        return;
    }
    if (same_word(name, "padding")) {
        into->padding = (unsigned char)length_in_characters(value);
        into->set |= DECL_PADDING;
        return;
    }
    if (same_word(name, "width")) {
        /* A width in characters, which is the only unit this screen has.
           A percentage is of the window, near enough. */
        int at = 0;
        int number = 0;

        while (value[at] >= '0' && value[at] <= '9')
            number = number * 10 + (value[at++] - '0');
        if (value[at] == '%') {
            into->width = (unsigned char)(number * 80 / 100);
        } else {
            int cells = length_in_characters(value);

            into->width = (unsigned char)(cells > 200 ? 200 : cells);
        }
        into->set |= DECL_WIDTH;
        return;
    }
    if (same_word(name, "margin-left") || same_word(name, "padding-left")) {
        into->margin = (unsigned char)length_in_characters(value);
        into->set |= DECL_MARGIN;
        return;
    }
    if (same_word(name, "margin-top") || same_word(name, "margin-bottom") ||
        same_word(name, "padding-top")) {
        int lines = length_in_characters(value) / 2;

        if (lines > 2) lines = 2;
        if (lines > 0) { into->space = (unsigned char)lines; into->set |= DECL_SPACE; }
        return;
    }
    if (same_word(name, "white-space")) {
        into->pre = (unsigned char)(same_ignoring_case(value, "pre", 3));
        into->set |= DECL_PRE;
        return;
    }
    /* Everything else - widths, floats, borders, shadows, transitions - is
       read and dropped. A browser that refused a page for a property it does
       not draw would refuse every page written this century. */
}

static void apply_declaration(STYLE* into, const DECL* decl) {
    if (decl->set & DECL_COLOR) into->color = decl->color;
    if (decl->set & DECL_BACKGROUND) {
        into->background = decl->background;
        into->has_background = 1;
    }
    if (decl->set & DECL_WEIGHT) into->bold = decl->bold;
    if (decl->set & DECL_ITALIC) into->italic = decl->italic;
    if (decl->set & DECL_DECORATION) into->underline = decl->underline;
    if (decl->set & DECL_ALIGN) into->align = decl->align;
    if (decl->set & DECL_PRE) into->pre = decl->pre;
    if (decl->set & DECL_BORDER) {
        into->has_border = decl->has_border;
        into->border = decl->border;
    }
    if (decl->set & DECL_PADDING) into->padding = decl->padding;
    if (decl->set & DECL_WIDTH) into->width = decl->width;
    if (decl->set & DECL_MARGIN) {
        /* Added rather than assigned: a list inside a list is indented twice,
           which is what a reader expects and what assignment would lose. */
        int indent = into->indent + decl->margin;

        into->indent = (unsigned char)(indent > 60 ? 60 : indent);
    }
}

/* ---- Rules ---------------------------------------------------------------
 *
 * A selector here is one element, optionally with one ancestor: `p`, `.note`,
 * `#main`, `td.price`, `#main a`. Deeper selectors are stored by their last
 * two parts, which matches more than it should and never less - a page whose
 * layout depends on `nav ul li > a:hover` was not going to survive the
 * missing font sizes either, and the parts that decide whether text is
 * readable are almost always this shallow.
 */

typedef struct {
    char tag[12];
    char klass[24];
    char id[24];
    char ancestor_tag[12];
    char ancestor_class[24];
    unsigned short specificity;
    unsigned short order;
    unsigned char origin;        /* 0 this browser's, 1 the page's */
    DECL decl;
} RULE;

static RULE rules[RULES_MAX];
static int rule_count;

static void parse_declarations(const char* text, long length, DECL* into) {
    long at = 0;

    while (at < length) {
        char name[32];
        char value[64];
        int out = 0;

        while (at < length && (is_space(text[at]) || text[at] == ';')) at++;
        while (at < length && text[at] != ':' && text[at] != ';' &&
               text[at] != '}') {
            if (out + 1 < (int)sizeof(name) && !is_space(text[at]))
                name[out++] = text[at];
            at++;
        }
        name[out] = 0;
        if (at >= length || text[at] != ':') { at++; continue; }
        at++;

        out = 0;
        while (at < length && text[at] != ';' && text[at] != '}') {
            /* !important is a promise this cannot keep in the order it should
               and can keep in effect: the last rule that matches wins here
               anyway, which is where an important declaration usually ends. */
            if (out + 1 < (int)sizeof(value)) value[out++] = text[at];
            at++;
        }
        while (out && is_space(value[out - 1])) out--;
        value[out] = 0;
        {
            int from = 0;

            while (value[from] == ' ') from++;
            if (from) copy_text(value, sizeof(value), value + from);
        }
        if (name[0] && value[0]) {
            lower_text(name);
            set_property(into, name, value);
        }
    }
}

/* One simple selector - `div`, `.note`, `#main`, `a.button` - taken apart. */
static void parse_simple(const char* text, char* tag, char* klass, char* id,
                         int* specificity) {
    int at = 0;
    int part = 0;                /* 0 tag, 1 class, 2 id */
    int out = 0;

    tag[0] = klass[0] = id[0] = 0;
    while (text[at]) {
        char c = text[at];

        if (c == '.' || c == '#') {
            part = c == '.' ? 1 : 2;
            out = 0;
            at++;
            continue;
        }
        /* A pseudo-class or an attribute test is where this stops reading:
           `a:hover` styles `a`, because there is no pointer state here and
           styling the link itself is nearer the truth than ignoring it. */
        if (c == ':' || c == '[' || c == '(') break;
        if (part == 0 && out + 1 < 12) tag[out++] = c;
        if (part == 1 && out + 1 < 24) klass[out++] = c;
        if (part == 2 && out + 1 < 24) id[out++] = c;
        if (part == 0) tag[out] = 0;
        if (part == 1) klass[out] = 0;
        if (part == 2) id[out] = 0;
        at++;
    }
    if (tag[0] == '*') tag[0] = 0;
    lower_text(tag);
    lower_text(klass);
    lower_text(id);
    *specificity = (id[0] ? 100 : 0) + (klass[0] ? 10 : 0) + (tag[0] ? 1 : 0);
}

static void add_rule(const char* selector, const DECL* decl,
                     unsigned char origin) {
    char parts[4][48];
    int part_count = 0;
    int out = 0;
    RULE* rule;

    if (rule_count >= RULES_MAX || !decl->set) return;

    /* Split the selector on whitespace and combinators, and keep the last two
       parts: the element being styled, and one ancestor. */
    for (int at = 0; ; at++) {
        char c = selector[at];

        if (c == '>' || c == '+' || c == '~') c = ' ';
        if (c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (out) {
                parts[part_count % 4][out] = 0;
                part_count++;
                out = 0;
            }
            if (c == 0) break;
            continue;
        }
        if (out + 1 < 48) parts[part_count % 4][out++] = c;
    }
    if (!part_count) return;

    rule = &rules[rule_count];
    memset(rule, 0, sizeof(RULE));
    {
        int specificity = 0;
        int subject = (part_count - 1) % 4;

        parse_simple(parts[subject], rule->tag, rule->klass, rule->id,
                     &specificity);
        rule->specificity = (unsigned short)specificity;
        if (part_count > 1) {
            char tag[12], klass[24], id[24];
            int ancestor_specificity = 0;
            int index = (part_count - 2) % 4;

            parse_simple(parts[index], tag, klass, id, &ancestor_specificity);
            copy_text(rule->ancestor_tag, sizeof(rule->ancestor_tag), tag);
            copy_text(rule->ancestor_class, sizeof(rule->ancestor_class), klass);
            rule->specificity = (unsigned short)(specificity +
                                                 ancestor_specificity);
        }
    }
    rule->decl = *decl;
    rule->origin = origin;
    rule->order = (unsigned short)rule_count;
    rule_count++;
}

static void parse_stylesheet(const char* text, long length,
                             unsigned char origin) {
    long at = 0;

    while (at < length) {
        char selector[192];
        int out = 0;
        long body;
        DECL decl;

        /* Comments, and the at-rules this cannot use. @media is entered
           rather than skipped: its contents are ordinary rules, and a screen
           is a screen - the alternative is a page whose entire stylesheet
           lives inside `@media screen` arriving unstyled. */
        while (at < length) {
            if (text[at] == '/' && at + 1 < length && text[at + 1] == '*') {
                at += 2;
                while (at + 1 < length &&
                       !(text[at] == '*' && text[at + 1] == '/')) at++;
                at += 2;
                continue;
            }
            if (is_space(text[at])) { at++; continue; }
            break;
        }
        if (at >= length) break;

        if (text[at] == '@') {
            int depth = 0;
            int entered = same_ignoring_case(text + at, "@media", 6) ||
                          same_ignoring_case(text + at, "@supports", 9);

            while (at < length && text[at] != '{' && text[at] != ';') at++;
            if (at < length && text[at] == ';') { at++; continue; }
            if (at < length && entered) { at++; continue; }
            /* @font-face and friends: skipped whole, braces and all. */
            while (at < length) {
                if (text[at] == '{') depth++;
                if (text[at] == '}') { depth--; at++; if (!depth) break; continue; }
                at++;
            }
            continue;
        }
        if (text[at] == '}') { at++; continue; }

        while (at < length && text[at] != '{') {
            if (out + 1 < (int)sizeof(selector)) selector[out++] = text[at];
            at++;
        }
        selector[out] = 0;
        if (at >= length) break;
        at++;

        body = at;
        while (at < length && text[at] != '}') at++;

        memset(&decl, 0, sizeof(decl));
        parse_declarations(text + body, at - body, &decl);
        if (at < length) at++;

        /* A comma-separated selector list is several rules that happen to
           have been written once. */
        {
            char one[192];
            int copied = 0;

            for (int scan = 0; ; scan++) {
                char c = selector[scan];

                if (c == ',' || c == 0) {
                    one[copied] = 0;
                    if (copied) add_rule(one, &decl, origin);
                    copied = 0;
                    if (!c) break;
                    continue;
                }
                if (copied + 1 < (int)sizeof(one)) one[copied++] = c;
            }
        }
    }
}

/* ---- This browser's own stylesheet ---------------------------------------
 *
 * What a bare <p> looks like, written as CSS rather than as C.
 *
 * Every browser has one of these; most have it buried in the layout code as a
 * table of tag defaults. Written as a stylesheet it costs nothing extra - the
 * cascade already exists to read the page's - and it means a page can
 * override any of it exactly the way it overrides another page's, through
 * specificity, without a line of code that knows a heading from a paragraph.
 *
 * The colours are the theme's names, so a document that sets none of its own
 * belongs to the desktop it is being read on.
 */
static const char* browser_stylesheet =
    "html,body{color:text;background:paper;display:block}"
    /* noscript is deliberately absent from this list. It holds what a page
       wants shown when its scripts do not run, and nothing here runs one -
       so it is not a fallback, it is the page. */
    "head,title,meta,link,script,style,svg,iframe,object,select,"
    "textarea,button,input{display:none}"
    "p,div,section,article,header,footer,nav,main,aside,form,figure,"
    "figcaption,address,fieldset,dl,dd,dt,table,tbody,thead,tr,caption,"
    "video,audio,details,summary{display:block}"
    "p{margin-top:2}"
    "h1,h2,h3,h4,h5,h6{display:block;font-weight:bold;margin-top:2}"
    "b,strong{font-weight:bold}"
    "i,em,cite,var,dfn{font-style:italic}"
    "u,ins{text-decoration:underline}"
    "a{color:accent;text-decoration:underline}"
    "ul,ol,dl{display:block;margin-top:2;margin-left:4}"
    "li{display:block}"
    "dd{margin-left:4}"
    "blockquote{display:block;margin-left:4;margin-top:2;color:shadow}"
    "pre,code,samp,kbd,tt{white-space:pre}"
    "pre{display:block;margin-top:2;margin-left:2}"
    "center{display:block;text-align:center}"
    "hr{display:block}"
    "th{font-weight:bold}"
    "small,sub,sup,span,label{display:inline}"
    "mark{background:yellow;color:black}"
    "del,s,strike{color:shadow}";

/* ---- The document --------------------------------------------------------
 *
 * Words, in the order they are read, each with the style it computed to and
 * the link it is inside. A line break is a piece too, because a break carries
 * how much space goes above it and the layout cannot work that out from the
 * words alone.
 */

#define PIECE_WORD 0
#define PIECE_BREAK 1
#define PIECE_RULE 2            /* <hr>, drawn as a line */
/* A picture. `at` holds which one, into the table above, rather than an
   offset into the text - the only piece whose fields mean something else, and
   worth saying so here because it is the sort of thing that is discovered the
   hard way. */
#define PIECE_IMAGE 3
/* A block with something to draw round it opens and closes. `at` holds which
   box, into the table below - the same trick the picture piece uses, for the
   same reason: a piece is six bytes and a box is not. */
#define PIECE_BOX_START 4
#define PIECE_BOX_END 5
/* A table, its rows and its cells. The layout needs them as marks in the
   stream rather than as a tree: it walks the pieces once to measure the
   columns and once to place them, and a tree would be a second shape of the
   same thing to keep in step. */
#define PIECE_TABLE_START 6
#define PIECE_TABLE_END 7
#define PIECE_ROW 8
#define PIECE_CELL 9

typedef struct {
    unsigned char kind;
    unsigned char spaces;       /* spaces before a word; blank lines on a break */
    /* No space before this word, however it was split from the one before.
     *
     * A tag ends a word whether or not a space does: `<code>&lt;p&gt;</code>,`
     * is one word, a tag, and a comma, and treating the tag as a space put the
     * comma a space away from what it belongs to. Every page that marks up
     * part of a word - and that is most of them - came out speckled with gaps
     * until this existed. */
    unsigned char glue;
    unsigned short style;
    unsigned short link;        /* NO_LINK when this is not inside one */
    unsigned short at;          /* where in `text` */
    unsigned short length;      /* in bytes */
    unsigned short cells;       /* on screen */
} PIECE;

static char page[PAGE_MAX];
static long page_length;
static char css[CSS_MAX];
static long css_length;
static char text[TEXT_MAX];
static long text_length;
static PIECE pieces[PIECES_MAX];
static int piece_count;

static char link_pool[LINK_POOL];
static int link_pool_used;
static unsigned short link_at[LINKS_MAX];
static int link_count;

static koi_uint32 page_background;
static int page_has_background;

static unsigned char glue_next;

/* ---- Pictures ------------------------------------------------------------
 *
 * A page names its pictures; each is a separate errand to a server, and each
 * arrives as bytes that have to be turned into pixels. They are collected in
 * the first pass over the document, fetched by the worker after the page
 * itself, and by the time the layout runs every one of them either has a size
 * or is known to have failed - which is what lets a picture take up room on a
 * line rather than appearing later and pushing the text about.
 *
 * The limits are deliberate and small: a page of a hundred photographs is a
 * page this machine should decline politely rather than spend a minute on.
 */
#define IMAGE_MAX 24
#define IMAGE_BYTES_MAX (6L * 1024 * 1024)   /* decoded, over the whole page */
#define IMAGE_FILE_MAX (2L * 1024 * 1024)

typedef struct {
    char url[URL_MAX];
    koi_uint32* pixels;
    int width, height;          /* as decoded */
    int shown_width, shown_height;   /* after fitting the column */
    int failed;
    /* Animation, for a GIF with more than one frame. The frames are decoded
       one at a time onto the same canvas, which is what the format means. */
    koi_uint8* file;
    long file_length;
    int frame_count;
    int frame;
    int delay_ms;
    koi_uint64 due;             /* when the next frame is owed, in ticks */
} IMAGE;

static IMAGE images[IMAGE_MAX];
static int image_count;
static long image_bytes;

/* ---- Boxes ---------------------------------------------------------------
 *
 * A block that has a colour behind it, a line round it, or a width of its own
 * becomes one of these. The pieces only say where it opens and closes; the
 * lines it covers are worked out when the page is laid out, because that is
 * the first moment anybody knows how wide the window is.
 */
#define BOX_MAX 64

typedef struct {
    koi_uint32 background;
    koi_uint32 border;
    unsigned char has_background;
    unsigned char has_border;
    unsigned char indent;        /* where its left edge sits, in characters */
    unsigned char width;         /* in characters */
    int first_line;
    int last_line;
} BOX;

static BOX boxes[BOX_MAX];
static int box_count;

static void resolve_link(const char* href, char* out);

static void forget_images(void) {
    for (int at = 0; at < IMAGE_MAX; at++) {
        if (images[at].pixels) koi_free(images[at].pixels);
        if (images[at].file) koi_free(images[at].file);
        images[at].pixels = (koi_uint32*)0;
        images[at].file = (koi_uint8*)0;
        images[at].width = images[at].height = 0;
        images[at].frame_count = 0;
        images[at].failed = 0;
        images[at].url[0] = 0;
    }
    image_count = 0;
    image_bytes = 0;
}

static void add_piece(unsigned char kind, unsigned short at,
                      unsigned short length, unsigned short style,
                      unsigned short link, unsigned char spaces) {
    if (piece_count >= PIECES_MAX) return;
    pieces[piece_count].glue = (unsigned char)(kind == PIECE_WORD ? glue_next : 0);
    pieces[piece_count].kind = kind;
    pieces[piece_count].at = at;
    pieces[piece_count].length = length;
    pieces[piece_count].cells = (unsigned short)cells_of(text + at, length);
    pieces[piece_count].style = style;
    pieces[piece_count].link = link;
    pieces[piece_count].spaces = spaces;
    piece_count++;
}

static int add_link(const char* href) {
    int length = (int)strlen(href);

    if (link_count >= LINKS_MAX) return -1;
    if (link_pool_used + length + 1 > LINK_POOL) return -1;
    link_at[link_count] = (unsigned short)link_pool_used;
    for (int at = 0; at <= length; at++)
        link_pool[link_pool_used++] = href[at];
    return link_count++;
}

static const char* link_text(int which) {
    if (which < 0 || which >= link_count) return "";
    return link_pool + link_at[which];
}

/* ---- Entities ------------------------------------------------------------ */

typedef struct { const char* name; unsigned int codepoint; } ENTITY;

static const ENTITY entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 0x00A0 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "copy", 0x00A9 },
    { "reg", 0x00AE }, { "trade", 0x2122 }, { "laquo", 0x00AB },
    { "raquo", 0x00BB }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "bull", 0x2022 },
    { "middot", 0x00B7 }, { "deg", 0x00B0 }, { "plusmn", 0x00B1 },
    { "times", 0x00D7 }, { "divide", 0x00F7 }, { "euro", 0x20AC },
    { "pound", 0x00A3 }, { "yen", 0x00A5 }, { "cent", 0x00A2 },
    { "sect", 0x00A7 }, { "para", 0x00B6 }, { "dagger", 0x2020 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "harr", 0x2194 },
    { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 },
    { 0, 0 }
};

/* An entity at `at`, written into `out` as UTF-8. Returns how many bytes of
   the page it consumed, including the ampersand, or 0 when it was not one. */
static int read_entity(const char* at, long left, char* out, int* out_length) {
    if (left < 2 || at[0] != '&') return 0;

    if (at[1] == '#') {
        unsigned int value = 0;
        int scan = 2;
        int digits = 0;

        if (scan < left && (at[scan] == 'x' || at[scan] == 'X')) {
            scan++;
            while (scan < left && hex_digit(at[scan]) >= 0) {
                value = value * 16 + (unsigned int)hex_digit(at[scan++]);
                digits++;
            }
        } else {
            while (scan < left && at[scan] >= '0' && at[scan] <= '9') {
                value = value * 10 + (unsigned int)(at[scan++] - '0');
                digits++;
            }
        }
        if (!digits || !value || value > 0x10FFFF) return 0;
        if (scan < left && at[scan] == ';') scan++;
        *out_length = write_utf8(out, value);
        return scan;
    }

    for (int index = 0; entities[index].name; index++) {
        int length = (int)strlen(entities[index].name);

        if (left < length + 1) continue;
        if (!same_ignoring_case(at + 1, entities[index].name, length)) continue;
        /* `&ampere` is not `&amp;ere`: the name has to end where a name ends. */
        {
            char after = (length + 1 < left) ? at[length + 1] : 0;

            if (after != ';' && ((after >= 'a' && after <= 'z') ||
                                 (after >= 'A' && after <= 'Z') ||
                                 (after >= '0' && after <= '9'))) continue;
            if (!entities[index].codepoint) {
                *out_length = 0;
                return length + 1 + (after == ';' ? 1 : 0);
            }
            *out_length = write_utf8(out, entities[index].codepoint);
            return length + 1 + (after == ';' ? 1 : 0);
        }
    }
    return 0;
}

/* Entities inside an attribute value.
 *
 * `href="a.cgi?x=1&amp;y=2"` is one address with an ampersand in it, and
 * following it as written asks the server for a page called `&amp;y=2`.
 * `alt="&nbsp;Files"` is a caption, not the word "nbsp". Both are the same
 * job as the one the text goes through, done in place on a short string. */
static void decode_entities(char* value) {
    long length = (long)strlen(value);
    long from = 0;
    long out = 0;

    while (from < length) {
        if (value[from] == '&') {
            char decoded[4];
            int decoded_length = 0;
            int used = read_entity(value + from, length - from, decoded,
                                   &decoded_length);

            if (used) {
                from += used;
                for (int at = 0; at < decoded_length; at++)
                    value[out++] = decoded[at];
                continue;
            }
        }
        value[out++] = value[from++];
    }
    value[out] = 0;
}

/* ---- Alphabets -----------------------------------------------------------
 *
 * Everything above this point is UTF-8, because the font is found by code
 * point and the rest of the system is. The web is not: a Russian page written
 * in 2003 is windows-1251, one written on a Unix machine of the time is
 * KOI8-R, and a text file on this machine's own disk may well be code page
 * 866. Each is a table of 128 code points, and converting is one pass.
 *
 * The conversion is done in place and backwards. A byte that becomes two or
 * three cannot be written forwards over the bytes it has not read yet, and a
 * second buffer of sixty-four kilobytes to avoid that is sixty-four kilobytes
 * spent on the direction of a loop.
 */

#define ENCODING_UTF8 0
#define ENCODING_1251 1
#define ENCODING_KOI8 2
#define ENCODING_866 3
#define ENCODING_LATIN1 4
#define ENCODING_GREEK 5

static const unsigned short high_1251[128] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,
    0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x0020,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,
    0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,
    0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
    0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
    0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
    0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
    0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F
};

static const unsigned short high_koi8[128] = {
    0x2500,0x2502,0x250C,0x2510,0x2514,0x2518,0x251C,0x2524,
    0x252C,0x2534,0x253C,0x2580,0x2584,0x2588,0x258C,0x2590,
    0x2591,0x2592,0x2593,0x2320,0x25A0,0x2219,0x221A,0x2248,
    0x2264,0x2265,0x00A0,0x2321,0x00B0,0x00B2,0x00B7,0x00F7,
    0x2550,0x2551,0x2552,0x0451,0x2553,0x2554,0x2555,0x2556,
    0x2557,0x2558,0x2559,0x255A,0x255B,0x255C,0x255D,0x255E,
    0x255F,0x2560,0x2561,0x0401,0x2562,0x2563,0x2564,0x2565,
    0x2566,0x2567,0x2568,0x2569,0x256A,0x256B,0x256C,0x00A9,
    0x044E,0x0430,0x0431,0x0446,0x0434,0x0435,0x0444,0x0433,
    0x0445,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,
    0x043F,0x044F,0x0440,0x0441,0x0442,0x0443,0x0436,0x0432,
    0x044C,0x044B,0x0437,0x0448,0x044D,0x0449,0x0447,0x044A,
    0x042E,0x0410,0x0411,0x0426,0x0414,0x0415,0x0424,0x0413,
    0x0425,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,
    0x041F,0x042F,0x0420,0x0421,0x0422,0x0423,0x0416,0x0412,
    0x042C,0x042B,0x0417,0x0428,0x042D,0x0429,0x0427,0x042A
};

static const unsigned short high_866[128] = {
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,
    0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,
    0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,
    0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
    0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
    0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
    0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,
    0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F,
    0x0401,0x0451,0x0404,0x0454,0x0407,0x0457,0x040E,0x045E,
    0x00B0,0x2219,0x00B7,0x221A,0x2116,0x00A4,0x25A0,0x00A0
};

/* ISO 8859-7 and windows-1253, which differ from each other in a handful of
   punctuation marks and not at all in the letters. The font has Greek. */
static const unsigned short high_greek[128] = {
    0x0080,0x0081,0x0082,0x0083,0x0084,0x0085,0x0086,0x0087,
    0x0088,0x0089,0x008A,0x008B,0x008C,0x008D,0x008E,0x008F,
    0x0090,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
    0x0098,0x0099,0x009A,0x009B,0x009C,0x009D,0x009E,0x009F,
    0x00A0,0x2018,0x2019,0x00A3,0x20AC,0x20AF,0x00A6,0x00A7,
    0x00A8,0x00A9,0x037A,0x00AB,0x00AC,0x00AD,0x0020,0x2015,
    0x00B0,0x00B1,0x00B2,0x00B3,0x0384,0x0385,0x0386,0x00B7,
    0x0388,0x0389,0x038A,0x00BB,0x038C,0x00BD,0x038E,0x038F,
    0x0390,0x0391,0x0392,0x0393,0x0394,0x0395,0x0396,0x0397,
    0x0398,0x0399,0x039A,0x039B,0x039C,0x039D,0x039E,0x039F,
    0x03A0,0x03A1,0x0020,0x03A3,0x03A4,0x03A5,0x03A6,0x03A7,
    0x03A8,0x03A9,0x03AA,0x03AB,0x03AC,0x03AD,0x03AE,0x03AF,
    0x03B0,0x03B1,0x03B2,0x03B3,0x03B4,0x03B5,0x03B6,0x03B7,
    0x03B8,0x03B9,0x03BA,0x03BB,0x03BC,0x03BD,0x03BE,0x03BF,
    0x03C0,0x03C1,0x03C2,0x03C3,0x03C4,0x03C5,0x03C6,0x03C7,
    0x03C8,0x03C9,0x03CA,0x03CB,0x03CC,0x03CD,0x03CE,0x0020
};

static int encoding_named(const char* name) {
    if (same_ignoring_case(name, "utf-8", 5) ||
        same_ignoring_case(name, "utf8", 4)) return ENCODING_UTF8;
    if (same_ignoring_case(name, "windows-1251", 12) ||
        same_ignoring_case(name, "cp1251", 6) ||
        same_ignoring_case(name, "win-1251", 8) ||
        same_ignoring_case(name, "x-cp1251", 8)) return ENCODING_1251;
    if (same_ignoring_case(name, "koi8", 4)) return ENCODING_KOI8;
    if (same_ignoring_case(name, "ibm866", 6) ||
        same_ignoring_case(name, "cp866", 5) ||
        same_ignoring_case(name, "866", 3)) return ENCODING_866;
    if (same_ignoring_case(name, "iso-8859-7", 10) ||
        same_ignoring_case(name, "windows-1253", 12) ||
        same_ignoring_case(name, "greek", 5)) return ENCODING_GREEK;
    if (same_ignoring_case(name, "iso-8859-1", 10) ||
        same_ignoring_case(name, "latin1", 6) ||
        same_ignoring_case(name, "iso-8859-15", 11) ||
        same_ignoring_case(name, "windows-1252", 12) ||
        same_ignoring_case(name, "us-ascii", 8)) return ENCODING_LATIN1;
    return -1;
}

/* The name after `charset=`, wherever it was found. */
static int charset_after(const char* at, long left) {
    char name[24];
    int out = 0;
    long scan = 0;

    while (scan < left && (at[scan] == ' ' || at[scan] == '"' ||
                           at[scan] == '\'' || at[scan] == '=')) scan++;
    while (scan < left && out + 1 < (int)sizeof(name) && at[scan] != '"' &&
           at[scan] != '\'' && at[scan] != ';' && at[scan] != ' ' &&
           at[scan] != '>' && at[scan] != '\r' && at[scan] != '\n')
        name[out++] = at[scan++];
    name[out] = 0;
    return out ? encoding_named(name) : -1;
}

/* What the document itself claims, looked for in the head where a claim is
   allowed to be. A page that says nothing is treated as UTF-8, which in 2026
   is right far more often than it is wrong. */
static int encoding_declared(void) {
    long limit = page_length < 4096 ? page_length : 4096;

    for (long at = 0; at + 8 < limit; at++) {
        if (page[at] != 'c' && page[at] != 'C') continue;
        if (!same_ignoring_case(page + at, "charset", 7)) continue;
        {
            int found = charset_after(page + at + 7, limit - at - 7);

            if (found >= 0) return found;
        }
    }
    return -1;
}

static void convert_to_utf8(int encoding) {
    const unsigned short* table;
    long grown = 0;
    long out;

    if (encoding <= ENCODING_UTF8) return;
    if (encoding == ENCODING_1251) table = high_1251;
    else if (encoding == ENCODING_KOI8) table = high_koi8;
    else if (encoding == ENCODING_866) table = high_866;
    else if (encoding == ENCODING_GREEK) table = high_greek;
    else table = 0;              /* Latin-1: the byte is the code point */

    /* How long it becomes, before anything is moved. */
    for (long at = 0; at < page_length; at++) {
        unsigned char byte = (unsigned char)page[at];
        unsigned int codepoint;

        if (byte < 0x80) { grown += 1; continue; }
        codepoint = table ? table[byte - 0x80] : byte;
        grown += codepoint < 0x800 ? 2 : 3;
    }
    if (grown > PAGE_MAX) grown = PAGE_MAX;

    /* Backwards, so that what has not been read yet is never written over. */
    out = grown;
    for (long at = page_length - 1; at >= 0; at--) {
        unsigned char byte = (unsigned char)page[at];
        char encoded[4];
        int length;

        if (byte < 0x80) { encoded[0] = (char)byte; length = 1; }
        else length = write_utf8(encoded, table ? table[byte - 0x80] : byte);
        if (out - length < 0) break;
        out -= length;
        for (int index = 0; index < length; index++)
            page[out + index] = encoded[index];
    }
    page_length = grown;
}

/* ---- Reading a tag -------------------------------------------------------- */

/* The value of one attribute out of a tag's text. Quotes are honoured, and an
   unquoted value ends at whitespace - which is most of the href attributes
   written by hand in the years this browser is aimed at. */
static int attribute(const char* tag, const char* name, char* out, int size) {
    int length = (int)strlen(name);
    int at = 0;

    out[0] = 0;
    while (tag[at]) {
        /* Skip a quoted value whole, so that `title="href=no"` is not read as
           an href. */
        if (tag[at] == '"' || tag[at] == '\'') {
            char quote = tag[at++];

            while (tag[at] && tag[at] != quote) at++;
            if (tag[at]) at++;
            continue;
        }
        if ((tag[at] == name[0] || tag[at] == name[0] - 32) &&
            same_ignoring_case(tag + at, name, length)) {
            int after = at + length;
            int before = at ? tag[at - 1] : ' ';

            while (tag[after] == ' ') after++;
            if (tag[after] == '=' && (before == ' ' || before == '\t' ||
                                      before == '\n' || before == '\r' ||
                                      at == 0)) {
                char quote = 0;
                int out_at = 0;

                after++;
                while (tag[after] == ' ') after++;
                if (tag[after] == '"' || tag[after] == '\'')
                    quote = tag[after++];
                while (tag[after]) {
                    char c = tag[after];

                    if (quote ? c == quote : (c == ' ' || c == '\t')) break;
                    if (out_at + 1 < size) out[out_at++] = c;
                    after++;
                }
                out[out_at] = 0;
                return 1;
            }
        }
        at++;
    }
    return 0;
}

static int class_matches(const char* classes, const char* wanted) {
    int at = 0;

    if (!wanted[0]) return 1;
    while (classes[at]) {
        int start;

        while (classes[at] == ' ') at++;
        start = at;
        while (classes[at] && classes[at] != ' ') at++;
        if (at > start && (int)strlen(wanted) == at - start &&
            same_ignoring_case(classes + start, wanted, at - start))
            return 1;
    }
    return 0;
}

/* ---- The element stack ---------------------------------------------------
 *
 * The tree, for as long as it takes to walk past it. An element knows its
 * name, its class and id (which is what a selector asks about), the style it
 * computed to, and - for an ordered list - how far it has counted.
 */

typedef struct {
    char tag[12];
    char klass[48];
    char id[24];
    STYLE style;
    unsigned short style_index;
    unsigned short link;
    short counter;               /* the next number in an ordered list */
    int box;                     /* which box this element opened, plus one */
    unsigned char list_ordered;
    unsigned char block;
    unsigned char space;
} ELEMENT;

static ELEMENT stack[STACK_MAX];
static int depth;

static int rule_matches(const RULE* rule, int at) {
    if (rule->tag[0] && !same_word(stack[at].tag, rule->tag)) return 0;
    if (rule->klass[0] && !class_matches(stack[at].klass, rule->klass)) return 0;
    if (rule->id[0] && !same_word(stack[at].id, rule->id)) return 0;
    if (!rule->ancestor_tag[0] && !rule->ancestor_class[0]) return 1;

    for (int up = at - 1; up >= 0; up--) {
        if (rule->ancestor_tag[0] && !same_word(stack[up].tag, rule->ancestor_tag))
            continue;
        if (rule->ancestor_class[0] &&
            !class_matches(stack[up].klass, rule->ancestor_class)) continue;
        return 1;
    }
    return 0;
}

/* The cascade, for one element: every rule that matches, applied weakest
   first. Origin beats specificity beats order, which is the order the
   standard gives and the reason a page can override anything this browser
   thinks a paragraph should look like. */
static void cascade(int at, const char* inline_style, unsigned char* display,
                    unsigned char* space) {
    int matched[RULES_MAX];
    int count = 0;

    *display = DISPLAY_INLINE;
    *space = 0;

    for (int index = 0; index < rule_count; index++)
        if (rule_matches(&rules[index], at) && count < RULES_MAX)
            matched[count++] = index;

    for (int a = 1; a < count; a++) {
        int hold = matched[a];
        int key = rules[hold].origin * 100000 + rules[hold].specificity * 1000 +
                  rules[hold].order;
        int b = a - 1;

        while (b >= 0) {
            int other = rules[matched[b]].origin * 100000 +
                        rules[matched[b]].specificity * 1000 +
                        rules[matched[b]].order;

            if (other <= key) break;
            matched[b + 1] = matched[b];
            b--;
        }
        matched[b + 1] = hold;
    }

    for (int index = 0; index < count; index++) {
        const DECL* decl = &rules[matched[index]].decl;

        apply_declaration(&stack[at].style, decl);
        if (decl->set & DECL_DISPLAY) *display = decl->display;
        if (decl->set & DECL_SPACE) *space = decl->space;
    }

    if (inline_style && inline_style[0]) {
        DECL decl;

        memset(&decl, 0, sizeof(decl));
        parse_declarations(inline_style, (long)strlen(inline_style), &decl);
        apply_declaration(&stack[at].style, &decl);
        if (decl.set & DECL_DISPLAY) *display = decl.display;
        if (decl.set & DECL_SPACE) *space = decl.space;
    }
}

/* ---- Pass one: what the head says ---------------------------------------- */

static char page_title[WINDOW_TITLE_MAX];
/* Whether what arrived is a document or a text file.
 *
 * textfiles.com is a web site whose every page of interest is a .txt, and a
 * text file put through an HTML parser loses every line break it had and comes
 * out as one grey paragraph. The server usually says which it is; when it does
 * not, the first tag in the first kilobyte says it instead. */
/* What one fetch turned out to be. Overwritten by every fetch, including the
   pictures and stylesheets a page asks for - so anything about the page
   itself is copied out of these into the `page_` pair below before those
   errands run. */
static int content_is_html = 1;
static int last_status;
/* The page was longer than there is room for. Said out loud rather than left
   to look like a page that simply ends: a document cut off in the middle is
   indistinguishable, on screen, from one the browser could not read. */
static int page_truncated;
/* Set by the desktop's thread to ask the worker to stop early; read by the
   worker between chunks. One direction only, which is why one word is
   enough. */
static volatile int fetch_cancel;
/* Whether anybody checked who answered, for the page that is on screen.
 *
 * Said in the status line in words. There is no padlock and there will not be
 * one until this can be 1: a padlock is a promise to somebody who has no way
 * to check it, and drawing one over an unverified connection is the single
 * most dishonest thing a browser can do. */
static int identity_checked;
/* The page's own answer, kept apart from the flag above.
 *
 * `identity_checked` describes the connection that finished most recently -
 * and after the page comes a stylesheet, and after that another, each with a
 * connection of its own. So the page's verdict was being overwritten by the
 * verdict on its stylesheets, and a verified page reported itself as
 * unverified. What the status line is about is the page. */
static int page_identity_checked;
/* And the same three about the page rather than about the last thing
   fetched. */
static int page_is_html = 1;
static int page_status;
static int page_cut;
/* Where a server said the page really lives. Filled by http_get when it
   answers with a 3xx, read by the worker, which goes there instead. */
static char moved_to[URL_MAX];
static char sheets[SHEETS_MAX][URL_MAX];
static int sheet_count;

static void first_pass(void) {
    long at = 0;

    css_length = 0;
    sheet_count = 0;
    page_title[0] = 0;

    while (at < page_length) {
        char tag[1024];
        char name[16];
        int out = 0;
        int closing = 0;
        long from;

        if (page[at] != '<') { at++; continue; }
        if (at + 3 < page_length && page[at + 1] == '!' && page[at + 2] == '-' &&
            page[at + 3] == '-') {
            at += 4;
            while (at + 2 < page_length &&
                   !(page[at] == '-' && page[at + 1] == '-' &&
                     page[at + 2] == '>')) at++;
            at += 3;
            continue;
        }

        from = at + 1;
        if (from < page_length && page[from] == '/') { closing = 1; from++; }
        while (from < page_length && page[from] != '>') {
            if (page[from] == '"' || page[from] == '\'') {
                char quote = page[from];

                if (out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
                from++;
                while (from < page_length && page[from] != quote) {
                    if (out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
                    from++;
                }
            }
            if (from < page_length && page[from] != '>' &&
                out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
            from++;
        }
        tag[out] = 0;
        at = from + 1;

        out = 0;
        while (tag[out] && !is_space(tag[out]) && tag[out] != '/' &&
               out + 1 < (int)sizeof(name)) { name[out] = tag[out]; out++; }
        name[out] = 0;
        lower_text(name);

        if (closing) continue;

        if (same_word(name, "style")) {
            /* Straight into the stylesheet buffer, up to its close tag. */
            long start = at;

            while (at + 7 < page_length &&
                   !(page[at] == '<' && page[at + 1] == '/' &&
                     same_ignoring_case(page + at + 2, "style", 5))) at++;
            for (long scan = start; scan < at && css_length + 1 < CSS_MAX; scan++)
                css[css_length++] = page[scan];
            continue;
        }
        if (same_word(name, "title") && !page_title[0]) {
            long start = at;
            int written = 0;

            while (at + 7 < page_length &&
                   !(page[at] == '<' && page[at + 1] == '/' &&
                     same_ignoring_case(page + at + 2, "title", 5))) at++;
            for (long scan = start; scan < at &&
                 written + 1 < (int)sizeof(page_title); scan++) {
                char c = page[scan];

                if (is_space(c)) {
                    if (!written || page_title[written - 1] == ' ') continue;
                    c = ' ';
                }
                page_title[written++] = c;
            }
            while (written && page_title[written - 1] == ' ') written--;
            page_title[written] = 0;
            continue;
        }
        if (same_word(name, "img")) {
            char source[URL_MAX];

            if (image_count < IMAGE_MAX &&
                attribute(tag, "src", source, sizeof(source)) && source[0]) {
                decode_entities(source);
                /* A picture written into the page itself rather than fetched.
                   Not read here, and named as what it is so the reason is
                   visible rather than mysterious. */
                if (!same_ignoring_case(source, "data:", 5)) {
                    resolve_link(source, images[image_count].url);
                    images[image_count].failed = 0;
                    image_count++;
                }
            }
            continue;
        }
        if (same_word(name, "link")) {
            char rel[32];
            char href[URL_MAX];

            if (!attribute(tag, "rel", rel, sizeof(rel))) continue;
            if (!same_ignoring_case(rel, "stylesheet", 10)) continue;
            if (!attribute(tag, "href", href, sizeof(href))) continue;
            if (sheet_count < SHEETS_MAX)
                copy_text(sheets[sheet_count++], URL_MAX, href);
            continue;
        }
    }
}

/* ---- Pass two: the words -------------------------------------------------- */

/* Which picture the second pass is up to. Both passes walk the document in
   the same order, so the Nth <img> in one is the Nth in the other - which is
   how a piece finds its picture without carrying its address. */
static int image_at;

static long pending_word;        /* where the word being collected starts */
static int pending_cells;
static int pending_spaces;
static int pending_break;
static int pending_blank;

static void flush_word(void) {
    if (pending_word < 0) return;
    add_piece(PIECE_WORD, (unsigned short)pending_word,
              (unsigned short)(text_length - pending_word),
              stack[depth].style_index, stack[depth].link,
              (unsigned char)pending_spaces);
    pending_word = -1;
    pending_spaces = 0;
    /* Whatever ended this word was a tag unless the caller says otherwise:
       only whitespace clears it, and only whitespace means a space. */
    glue_next = 1;
    (void)pending_cells;
}

/* A break is asked for rather than emitted.
 *
 * A page is full of elements that begin and end with nothing between them -
 * a div holding a div holding a paragraph opens three blocks before a single
 * word arrives. Emitting a break for each would open the page with three
 * empty lines. Asking for one and emitting it when a word finally turns up
 * means a run of empty blocks costs one line, and the largest amount of space
 * anybody asked for is what is left above it. */
static void want_break(int blank) {
    pending_break = 1;
    if (blank > pending_blank) pending_blank = blank;
}

static void flush_break(void) {
    if (!pending_break) return;
    if (piece_count)
        add_piece(PIECE_BREAK, 0, 0, stack[depth].style_index, NO_LINK,
                  (unsigned char)pending_blank);
    pending_break = 0;
    pending_blank = 0;
    glue_next = 0;
}

static void begin_word(void) {
    if (pending_word >= 0) return;
    flush_break();
    pending_word = text_length;
}

static void add_character(char c) {
    begin_word();
    if (text_length + 1 < TEXT_MAX) text[text_length++] = c;
}

static void add_string(const char* what) {
    for (int at = 0; what[at]; at++) add_character(what[at]);
}

static const char* void_elements[] = {
    "br", "hr", "img", "meta", "link", "input", "source", "area", "base",
    "col", "embed", "param", "track", "wbr", 0
};

static int is_void(const char* name) {
    for (int at = 0; void_elements[at]; at++)
        if (same_word(name, void_elements[at])) return 1;
    return 0;
}

static void second_pass(void) {
    long at = 0;
    int hidden_depth = 0;        /* how deep inside a display:none we are */

    text_length = 0;
    piece_count = 0;
    link_count = 0;
    link_pool_used = 0;
    pending_word = -1;
    pending_spaces = 0;
    pending_break = 0;
    pending_blank = 0;
    glue_next = 0;
    image_at = 0;
    box_count = 0;
    depth = 0;
    page_has_background = 0;

    memset(&stack[0], 0, sizeof(ELEMENT));
    copy_text(stack[0].tag, sizeof(stack[0].tag), "html");
    stack[0].style.color = mizu->color(MIZU_COLOR_TEXT);
    stack[0].style.align = ALIGN_LEFT;
    stack[0].link = NO_LINK;
    stack[0].style_index = intern_style(&stack[0].style);

    while (at < page_length) {
        char c = page[at];

        if (c == '<' && at + 1 < page_length) {
            char tag[1024];
            char name[16];
            int out = 0;
            int closing = 0;
            long from;

            /* Comments and doctypes: neither is a tag and both look like one. */
            if (at + 3 < page_length && page[at + 1] == '!' &&
                page[at + 2] == '-' && page[at + 3] == '-') {
                at += 4;
                while (at + 2 < page_length &&
                       !(page[at] == '-' && page[at + 1] == '-' &&
                         page[at + 2] == '>')) at++;
                at += 3;
                continue;
            }

            from = at + 1;
            if (page[from] == '/') { closing = 1; from++; }
            /* A `<` that is not a tag - `a < b` in running text - is text. */
            if (!closing && !((page[from] >= 'a' && page[from] <= 'z') ||
                              (page[from] >= 'A' && page[from] <= 'Z') ||
                              page[from] == '!')) {
                if (!hidden_depth) add_character('<');
                at++;
                continue;
            }

            while (from < page_length && page[from] != '>') {
                if (page[from] == '"' || page[from] == '\'') {
                    char quote = page[from];

                    if (out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
                    from++;
                    while (from < page_length && page[from] != quote) {
                        if (out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
                        from++;
                    }
                }
                if (from < page_length && page[from] != '>' &&
                    out + 1 < (int)sizeof(tag)) tag[out++] = page[from];
                from++;
            }
            tag[out] = 0;
            at = from + 1;

            out = 0;
            while (tag[out] && !is_space(tag[out]) && tag[out] != '/' &&
                   out + 1 < (int)sizeof(name)) { name[out] = tag[out]; out++; }
            name[out] = 0;
            lower_text(name);
            if (!name[0]) continue;

            /* Raw text: what is inside is not text however much it looks like
               it. Skipped here, having been taken in pass one. */
            if (!closing && (same_word(name, "script") ||
                             same_word(name, "style") ||
                             same_word(name, "title") ||
                             same_word(name, "textarea"))) {
                int length = (int)strlen(name);

                while (at + length + 3 < page_length &&
                       !(page[at] == '<' && page[at + 1] == '/' &&
                         same_ignoring_case(page + at + 2, name, length))) at++;
                while (at < page_length && page[at] != '>') at++;
                at++;
                continue;
            }

            if (closing) {
                int found = -1;

                for (int scan = depth; scan > 0; scan--)
                    if (same_word(stack[scan].tag, name)) { found = scan; break; }
                if (found < 0) continue;    /* a close with no open: ignored */

                flush_word();
                if (same_word(name, "table")) {
                    flush_word();
                    add_piece(PIECE_TABLE_END, 0, 0, stack[depth].style_index,
                              NO_LINK, 0);
                }
                for (int scan = depth; scan >= found; scan--) {
                    if (stack[scan].box) {
                        flush_break();
                        add_piece(PIECE_BOX_END,
                                  (unsigned short)(stack[scan].box - 1), 0,
                                  stack[scan].style_index, NO_LINK, 0);
                        stack[scan].box = 0;
                    }
                    if (stack[scan].block) want_break(0);
                    if (hidden_depth && scan == hidden_depth) hidden_depth = 0;
                }
                depth = found - 1;
                if (depth < 0) depth = 0;
                continue;
            }

            /* An element opens. */
            {
                unsigned char display;
                unsigned char space;
                char klass[48];
                char id[24];
                char inline_style[192];
                int self_closing = out && tag[strlen(tag) - 1] == '/';

                if (depth + 1 >= STACK_MAX) {
                    /* Deeper than this is a page built out of nested divs; the
                       style stops changing rather than the page stopping. */
                    continue;
                }

                flush_word();
                depth++;
                stack[depth] = stack[depth - 1];
                copy_text(stack[depth].tag, sizeof(stack[depth].tag), name);
                stack[depth].counter = 1;
                stack[depth].list_ordered = 0;
                stack[depth].block = 0;
                stack[depth].space = 0;
                /* Not inherited: a background belongs to the element that set
                   it, and a paragraph inside a highlighted div is not itself
                   highlighted word by word. */
                stack[depth].style.has_background = 0;

                attribute(tag, "class", klass, sizeof(klass));
                attribute(tag, "id", id, sizeof(id));
                attribute(tag, "style", inline_style, sizeof(inline_style));
                copy_text(stack[depth].klass, sizeof(stack[depth].klass), klass);
                copy_text(stack[depth].id, sizeof(stack[depth].id), id);
                lower_text(stack[depth].klass);
                lower_text(stack[depth].id);

                cascade(depth, inline_style, &display, &space);

                if (display == DISPLAY_NONE) {
                    if (!hidden_depth) hidden_depth = depth;
                }
                if (display == DISPLAY_BLOCK) {
                    stack[depth].block = 1;
                    stack[depth].space = space;
                    want_break(space);

                    /* Worth drawing a box for? Only if something would be
                       visible: a colour, a line, or a width that makes it
                       narrower than the page. */
                    if (!hidden_depth && box_count < BOX_MAX &&
                        (stack[depth].style.has_background ||
                         stack[depth].style.has_border ||
                         stack[depth].style.width)) {
                        BOX* box = &boxes[box_count];

                        box->background = stack[depth].style.background;
                        box->border = stack[depth].style.border;
                        box->has_background = stack[depth].style.has_background;
                        box->has_border = stack[depth].style.has_border;
                        box->indent = stack[depth].style.indent;
                        box->width = stack[depth].style.width;
                        box->first_line = -1;
                        box->last_line = -1;
                        stack[depth].box = box_count + 1;
                        flush_break();
                        add_piece(PIECE_BOX_START, (unsigned short)box_count,
                                  0, stack[depth].style_index, NO_LINK, 0);
                        box_count++;
                        /* Room inside the line, made of blank space above and
                           an indent for everything within. */
                        if (stack[depth].style.padding) {
                            int inside = stack[depth].style.indent +
                                         stack[depth].style.padding;

                            stack[depth].style.indent =
                                (unsigned char)(inside > 60 ? 60 : inside);
                            stack[depth].style_index =
                                intern_style(&stack[depth].style);
                        }
                    }
                }

                /* The background of the page itself, which is the one box
                   this draws. Anything else keeps its colour behind its own
                   words. */
                if ((same_word(name, "body") || same_word(name, "html")) &&
                    stack[depth].style.has_background) {
                    page_background = stack[depth].style.background;
                    page_has_background = 1;
                }

                if (same_word(name, "a")) {
                    char href[URL_MAX];

                    /* One link never runs into the next.
                     *
                     * `</a><a>` with nothing between them is how a row of
                     * footer links is written, and gluing them - which is what
                     * the rule for `<b>bold</b>ish` correctly does - produced
                     * `AdvertisingBusiness solutionsAbout Google` as one
                     * unreadable line. Two links are two things whatever the
                     * markup does about whitespace. */
                    if (piece_count && pieces[piece_count - 1].kind == PIECE_WORD &&
                        pieces[piece_count - 1].link != NO_LINK)
                        glue_next = 0;

                    if (attribute(tag, "href", href, sizeof(href))) {
                        int which;

                        decode_entities(href);
                        which = add_link(href);

                        if (which >= 0) stack[depth].link = (unsigned short)which;
                    }
                }
                if (same_word(name, "ol")) stack[depth].list_ordered = 1;
                if (same_word(name, "ul")) stack[depth].list_ordered = 0;

                stack[depth].style_index = intern_style(&stack[depth].style);

                if (!hidden_depth) {
                    if (same_word(name, "br")) {
                        flush_word();
                        want_break(0);
                        /* A break with nothing after it still ends a line, so
                           the request cannot wait for a word that never
                           comes. */
                        flush_break();
                    } else if (same_word(name, "hr")) {
                        flush_word();
                        want_break(0);
                        flush_break();
                        add_piece(PIECE_RULE, 0, 0, stack[depth].style_index,
                                  NO_LINK, 0);
                        want_break(0);
                    } else if (same_word(name, "li")) {
                        int ordered = 0;
                        int number = 1;

                        for (int up = depth - 1; up > 0; up--) {
                            if (same_word(stack[up].tag, "ol")) {
                                ordered = 1;
                                number = stack[up].counter++;
                                break;
                            }
                            if (same_word(stack[up].tag, "ul")) break;
                        }
                        flush_break();
                        if (ordered) {
                            char marker[8];

                            koi_snprintf(marker, sizeof(marker), "%d.", number);
                            add_string(marker);
                        } else {
                            add_string("\xe2\x80\xa2");   /* a bullet, U+2022 */
                        }
                        flush_word();
                        glue_next = 0;
                    } else if (same_word(name, "img")) {
                        char alt[64];
                        int which = image_at++;

                        /* A picture that arrived takes room on the line; one
                           that did not falls back to what it was going to
                           say, which is what alt text is for. */
                        if (which < image_count && !images[which].failed &&
                            images[which].pixels) {
                            flush_word();
                            flush_break();
                            add_piece(PIECE_IMAGE, (unsigned short)which, 0,
                                      stack[depth].style_index,
                                      stack[depth].link, 0);
                            glue_next = 0;
                            continue;
                        }

                        /* What a picture would have said. A page of photographs
                           becomes a page of captions, which is more than a page
                           of nothing and is honest about which it is. */
                        if (attribute(tag, "alt", alt, sizeof(alt)) && alt[0]) {
                            int written = 0;

                            decode_entities(alt);
                            add_character('[');
                            for (int scan = 0; alt[scan]; scan++) {
                                /* A caption is text and wraps like text; the
                                   run of spaces somebody used to centre it
                                   under a picture is not one long word. */
                                if (is_space(alt[scan]) ||
                                    (unsigned char)alt[scan] == 0xC2) {
                                    if ((unsigned char)alt[scan] == 0xC2 &&
                                        (unsigned char)alt[scan + 1] == 0xA0) scan++;
                                    if (!written) continue;
                                    flush_word();
                                    glue_next = 0;
                                    continue;
                                }
                                add_character(alt[scan]);
                                written = 1;
                            }
                            glue_next = 1;      /* the bracket closes a word */
                            add_character(']');
                            flush_word();
                            glue_next = 0;
                        }
                    } else if (same_word(name, "table")) {
                        flush_word();
                        want_break(0);
                        flush_break();
                        add_piece(PIECE_TABLE_START, 0, 0,
                                  stack[depth].style_index, NO_LINK, 0);
                    } else if (same_word(name, "tr")) {
                        flush_word();
                        add_piece(PIECE_ROW, 0, 0, stack[depth].style_index,
                                  NO_LINK, 0);
                    } else if (same_word(name, "td") || same_word(name, "th")) {
                        flush_word();
                        add_piece(PIECE_CELL, 0, 0, stack[depth].style_index,
                                  NO_LINK, 0);
                        glue_next = 0;
                    }
                }

                if (is_void(name) || self_closing) {
                    /* A tag with no closing tag still closes its box.
                     *
                     * It did not, and `hr { background-color: ... }` - which
                     * is how half the small web draws its dividers - left a
                     * box open for the rest of the document. It swallowed
                     * every line after it, took the width of the first table
                     * cell it met, and painted a slab of colour across the
                     * middle of the page. */
                    if (stack[depth].box) {
                        add_piece(PIECE_BOX_END,
                                  (unsigned short)(stack[depth].box - 1), 0,
                                  stack[depth].style_index, NO_LINK, 0);
                        stack[depth].box = 0;
                    }
                    if (stack[depth].block) want_break(0);
                    if (hidden_depth == depth) hidden_depth = 0;
                    depth--;
                }
            }
            continue;
        }

        if (hidden_depth) { at++; continue; }

        if (c == '&') {
            char decoded[4];
            int length = 0;
            int used = read_entity(page + at, page_length - at, decoded, &length);

            if (used) {
                at += used;
                if (length == 2 && (unsigned char)decoded[0] == 0xC2 &&
                    (unsigned char)decoded[1] == 0xA0) {
                    /* A non-breaking space is a space that holds a line
                       together; it is not a letter and a word made of one is
                       a word made of nothing. */
                    if (pending_word < 0) pending_spaces++;
                    else add_string("\xc2\xa0");
                    continue;
                }
                for (int index = 0; index < length; index++)
                    add_character(decoded[index]);
                continue;
            }
        }

        if (stack[depth].style.pre) {
            if (c == '\n') {
                flush_word();
                want_break(0);
                flush_break();
                at++;
                continue;
            }
            if (c == ' ' || c == '\t') {
                int spaces = c == '\t' ? 4 : 1;

                if (pending_word < 0) pending_spaces += spaces;
                else { flush_word(); pending_spaces = spaces; }
                at++;
                continue;
            }
            if (c == '\r') { at++; continue; }
            add_character(c);
            at++;
            continue;
        }

        if (is_space(c)) {
            flush_word();
            glue_next = 0;
            at++;
            continue;
        }

        add_character(c);
        at++;
    }
    flush_word();
}

/* ---- Layout --------------------------------------------------------------
 *
 * Words into lines, at the width the window happens to be. Done here rather
 * than while painting because alignment needs the width of a line before its
 * first word is drawn, and because a page that is scrolled should not be laid
 * out again for every screenful.
 */

/* A line of the page, which is one row of the screen.
 *
 * It used to be one run of pieces, and that was true until tables: two cells
 * side by side put two runs on one row, from two places in the document.
 * So a line is a few runs, each with its own left edge - one for ordinary
 * text, one per column inside a table row.
 *
 * Four, because a table of more than four columns on a screen eighty
 * characters wide gives each of them sixteen characters, and at that point
 * the honest thing is to let the extra columns wrap into the flow rather than
 * to pretend. */
#define RUNS_MAX 4

typedef struct {
    int first;
    int count;
    unsigned char indent;
} RUN;

typedef struct {
    RUN runs[RUNS_MAX];
    unsigned char run_count;
    int cells;                   /* of the first run, for alignment */
    unsigned char align;
    unsigned char rule;          /* this line is an <hr> */
    unsigned char image;         /* this line is the top of a picture */
    unsigned char image_rows;    /* how many lines the picture occupies */
} LINE;

static LINE lines[LINES_MAX];
static int line_count;
static int laid_out_for;         /* the width the lines were packed at */
static int top_line;

/* ---- Tables --------------------------------------------------------------
 *
 * A table is measured before it is placed, because a column is as wide as the
 * widest thing in it and that is not known until every row has been looked at.
 * So the pieces are walked twice: once counting, once placing.
 *
 * What is not here: cells that span several columns or rows. A page that uses
 * them gets the cells side by side in the order they were written, which is
 * wrong in the way a missing feature is wrong rather than in the way a bug is.
 */
#define TABLE_COLUMNS_MAX 8


/* Where a table stands while it is being laid out.
 *
 * One of these per level, because a table inside a cell of another table is
 * ordinary on a page of this vintage - the outer one is the page's layout and
 * the inner one is the actual table. There was one set of these as plain
 * variables, and a nested table overwrote the outer table's row and column
 * while the outer table was still being filled: everything after it landed in
 * the wrong column, or outside the table altogether. */
#define TABLE_DEPTH_MAX 3

typedef struct {
    int row_top;                 /* the first line of the row being filled */
    int row_deepest;             /* the deepest line any cell has reached */
    int column;
    int cell_line;               /* where the current cell is writing */
    int columns;
    int width[TABLE_COLUMNS_MAX];
    int start[TABLE_COLUMNS_MAX];  /* left edge of each column, in characters */
    int left;                    /* where this table itself begins */
    int open_outside;            /* boxes already open when it began */
} TABLE;

static TABLE tables[TABLE_DEPTH_MAX];
static int table_depth;

/* Measure one table: every column is as wide as the widest thing in it.
 *
 * `room` is what the table has to fit in - the window, or the cell it is
 * inside - and `left` is where it starts. Pieces belonging to a table nested
 * inside this one are counted towards the cell that holds it, which
 * overestimates that cell and is corrected by the trimming at the end. */
static void measure_table(int from, int room, int left, TABLE* out) {
    int at = from;
    int column = -1;
    int width = 0;
    int depth = 0;
    int total;
    int declared[TABLE_COLUMNS_MAX];

    for (int index = 0; index < TABLE_COLUMNS_MAX; index++) declared[index] = 0;
    out->columns = 0;
    out->left = left;
    for (int index = 0; index < TABLE_COLUMNS_MAX; index++) {
        out->width[index] = 0;
        out->start[index] = left;
    }

    while (at < piece_count) {
        PIECE* piece = &pieces[at];

        if (piece->kind == PIECE_TABLE_END) {
            if (!depth) break;
            depth--;
        } else if (piece->kind == PIECE_TABLE_START) {
            depth++;
        } else if (!depth && piece->kind == PIECE_ROW) {
            if (column >= 0 && column < TABLE_COLUMNS_MAX &&
                width > out->width[column]) out->width[column] = width;
            column = -1;
            width = 0;
        } else if (!depth && piece->kind == PIECE_CELL) {
            if (column >= 0 && column < TABLE_COLUMNS_MAX &&
                width > out->width[column]) out->width[column] = width;
            column++;
            width = 0;
            if (column + 1 > out->columns && column < TABLE_COLUMNS_MAX)
                out->columns = column + 1;
            /* A cell that says how wide it is, is that wide. `width: 30%` on
               a sidebar is the whole of what makes a page laid out as a table
               look like the page its author saw. */
            if (column >= 0 && column < TABLE_COLUMNS_MAX) {
                int said = styles[piece->style].width;

                if (said > declared[column]) declared[column] = said;
            }
        } else if (piece->kind == PIECE_BREAK ||
                   (depth && piece->kind == PIECE_ROW)) {
            /* A paragraph ends a line, so a cell is as wide as its widest
               paragraph rather than as wide as all of them laid end to end.
               Summing them made a cell holding three sentences claim three
               sentences' worth of the page, and squeezed everything beside it
               to the four-character minimum. */
            if (column >= 0 && column < TABLE_COLUMNS_MAX &&
                width > out->width[column]) out->width[column] = width;
            width = 0;
        } else if (piece->kind == PIECE_WORD) {
            width += piece->cells + piece->spaces + (width ? 1 : 0);
        } else if (piece->kind == PIECE_IMAGE && piece->at < IMAGE_MAX) {
            int wide = (images[piece->at].width + WINDOW_CHAR_W - 1) /
                       WINDOW_CHAR_W;

            if (wide > width) width = wide;
        }
        at++;
    }
    if (column >= 0 && column < TABLE_COLUMNS_MAX && width > out->width[column])
        out->width[column] = width;

    if (!out->columns) return;

    for (int index = 0; index < out->columns; index++)
        if (declared[index]) out->width[index] = declared[index];

    /* Wider than there is room for is the ordinary case for a table written
       for a screen of 1995. Every column is trimmed by the same proportion,
       and none below a few characters - a column of one letter is a column
       nobody can read. */
    total = 0;
    for (int index = 0; index < out->columns; index++)
        total += out->width[index] + 2;
    if (total > room && total > 2 * out->columns) {
        for (int index = 0; index < out->columns; index++) {
            out->width[index] = out->width[index] * (room - 2 * out->columns)
                                / (total - 2 * out->columns);
            if (out->width[index] < 4) out->width[index] = 4;
        }
    }

    {
        int edge = left;

        for (int index = 0; index < out->columns; index++) {
            out->start[index] = edge;
            edge += out->width[index] + 2;
        }
    }
}

/* An empty line, which is the same seven assignments everywhere.
 *
 * `runs` is how many runs it starts with: one for the ordinary flow, where
 * the run is the line itself, and none inside a table, where the first cell
 * to write on this line takes the first run. A line whose first run is empty
 * is skipped when drawing, so a cell that landed in the second run of a line
 * whose first was blank was invisible - a table of nothing but its own
 * borders. */
static void blank_line_with(LINE* line, int at, int runs) {
    line->run_count = (unsigned char)runs;
    line->runs[0].first = at;
    line->runs[0].count = 0;
    line->runs[0].indent = 0;
    line->cells = 0;
    line->align = ALIGN_LEFT;
    line->rule = 0;
    line->image = 0;
    line->image_rows = 0;
}

static void blank_line(LINE* line, int at) { blank_line_with(line, at, 1); }

/* How much of a column a run has used, counted from its pieces - the line
   itself keeps a total only for the first run, which is the one alignment
   cares about. */
static int cell_width_so_far(const RUN* run, const LINE* line) {
    int total = 0;

    (void)line;
    for (int at = 0; at < run->count; at++) {
        const PIECE* piece = &pieces[run->first + at];

        total += piece->cells + piece->spaces + (at ? 1 : 0);
    }
    return total;
}

static void layout(int columns) {
    LINE* line;
    int usable;
    int open_boxes[16];
    int open_count = 0;
    static int box_right[BOX_MAX];  /* how far right each box's contents go */
    int image_line = -1;          /* the line pictures are being laid on */
    int image_used = 0;           /* how many characters wide they are so far */
    int image_next = -1;          /* the piece that would continue that row */

    if (columns == laid_out_for) return;
    laid_out_for = columns;
    line_count = 0;
    table_depth = 0;
    for (int at = 0; at < BOX_MAX; at++) box_right[at] = 0;

    line = &lines[0];
    line->runs[0].first = 0;
    line->runs[0].count = 0;
    line->cells = 0;
    line->runs[0].indent = 0;
    line->run_count = 1;
    line->align = ALIGN_LEFT;
    line->rule = 0;
    line->image = 0;
    line->image_rows = 0;

    for (int at = 0; at < piece_count; at++) {
        PIECE* piece = &pieces[at];
        STYLE* style = &styles[piece->style];

        /* Whatever is about to go on a line, that line is inside every box
         * still open - which is the only definition of "covers" that survives
         * nested blocks, padding and blank lines.
         *
         * Not inside a cell, though: there the line is `cell_line` and not
         * this counter, and the cell's own branch below records it. Doing
         * both gave every cell's box a first line at the top of the whole
         * table, so the colour behind a sidebar started level with the table
         * and its text sat at the bottom of it. */
        if ((!table_depth || tables[table_depth - 1].column < 0) &&
            (piece->kind == PIECE_WORD || piece->kind == PIECE_IMAGE ||
             piece->kind == PIECE_RULE))
            for (int open = 0; open < open_count; open++) {
                BOX* box = &boxes[open_boxes[open]];
                int right = line->runs[0].indent + line->cells + piece->cells;

                if (box->first_line < 0) box->first_line = line_count;
                box->last_line = line_count;
                if (right > box_right[open_boxes[open]])
                    box_right[open_boxes[open]] = right;
            }

        if (piece->kind == PIECE_BREAK) {
            if (line_count + 1 + piece->spaces >= LINES_MAX) break;
            line_count++;
            for (int blank = 0; blank < piece->spaces; blank++) {
                lines[line_count].run_count = 1;
                lines[line_count].runs[0].first = at;
                lines[line_count].runs[0].count = 0;
                lines[line_count].cells = 0;
                lines[line_count].runs[0].indent = 0;
                lines[line_count].align = ALIGN_LEFT;
                lines[line_count].rule = 0;
                line_count++;
            }
            line = &lines[line_count];
            line->run_count = 1;
            line->runs[0].first = at + 1;
            line->runs[0].count = 0;
            line->cells = 0;
            line->runs[0].indent = style->indent;
            line->align = style->align;
            line->rule = 0;
            continue;
        }

        if (piece->kind == PIECE_TABLE_START) {
            TABLE* outer = table_depth ? &tables[table_depth - 1] : (TABLE*)0;
            TABLE* self;
            int room = columns;
            int left = 0;

            if (table_depth >= TABLE_DEPTH_MAX) continue;
            if (outer) {
                /* A table inside a cell begins where that cell begins and is
                   as wide as that cell. */
                if (outer->column < 0) continue;
                left = outer->start[outer->column];
                room = outer->width[outer->column];
            } else if (line->runs[0].count) {
                if (line_count + 1 >= LINES_MAX) break;
                line_count++;
                line = &lines[line_count];
                line->runs[0].count = 0;
                line->cells = 0;
                line->run_count = 0;
            }

            self = &tables[table_depth];
            measure_table(at + 1, room, left, self);
            if (self->columns <= 0) continue;
            self->open_outside = open_count;
            self->row_top = outer ? outer->cell_line : line_count;
            self->row_deepest = self->row_top - 1;
            self->column = -1;
            self->cell_line = self->row_top;
            table_depth++;
            continue;
        }

        if (piece->kind == PIECE_TABLE_END) {
            TABLE* self;

            if (!table_depth) continue;
            self = &tables[--table_depth];
            if (table_depth) {
                /* Back into the cell that held it, below what it filled. */
                TABLE* outer = &tables[table_depth - 1];

                outer->cell_line = self->row_deepest + 1;
                if (outer->cell_line > outer->row_deepest)
                    outer->row_deepest = self->row_deepest;
                if (outer->column >= 0 &&
                    lines[outer->cell_line].run_count < RUNS_MAX) {
                    LINE* target = &lines[outer->cell_line];
                    RUN* run = &target->runs[target->run_count++];

                    run->first = at + 1;
                    run->count = 0;
                    run->indent = (unsigned char)outer->start[outer->column];
                }
                continue;
            }
            line_count = self->row_deepest + 1;
            if (line_count >= LINES_MAX) line_count = LINES_MAX - 1;
            line = &lines[line_count];
            line->run_count = 1;
            line->runs[0].first = at + 1;
            line->runs[0].count = 0;
            line->runs[0].indent = 0;
            line->cells = 0;
            line->align = ALIGN_LEFT;
            line->rule = 0;
            line->image = 0;
            line->image_rows = 0;
            continue;
        }

        if (piece->kind == PIECE_ROW) {
            TABLE* self;

            if (!table_depth) continue;
            self = &tables[table_depth - 1];
            self->row_top = self->row_deepest + 1;
            if (self->row_top < 0) self->row_top = 0;
            self->row_deepest = self->row_top - 1;
            self->column = -1;
            self->cell_line = self->row_top;
            continue;
        }

        if (piece->kind == PIECE_CELL) {
            TABLE* self;

            if (!table_depth) continue;
            self = &tables[table_depth - 1];
            self->column++;
            if (self->column >= self->columns || self->column >= RUNS_MAX) {
                /* More columns than there is room for: the rest of the row
                   goes underneath rather than off the edge. */
                self->column = self->columns - 1;
                if (self->column < 0) self->column = 0;
            }
            self->cell_line = self->row_top;
            if (self->cell_line >= LINES_MAX) { table_depth = 0; continue; }
            if (self->cell_line > self->row_deepest) {
                for (int fill = self->row_deepest + 1; fill <= self->cell_line;
                     fill++)
                    blank_line_with(&lines[fill], at, 0);
                self->row_deepest = self->cell_line;
            }
            {
                LINE* target = &lines[self->cell_line];

                if (target->run_count < RUNS_MAX) {
                    RUN* run = &target->runs[target->run_count++];

                    run->first = at + 1;
                    run->count = 0;
                    run->indent = (unsigned char)self->start[self->column];
                }
            }
            continue;
        }

        /* Inside a cell. Words wrap within their column, and a paragraph or a
         * heading moves down a line inside it rather than being dropped.
         *
         * Dropped is what used to happen - every break inside a table was
         * thrown away, so a cell holding three paragraphs came out as one
         * long line of them run together, and the sidebar of a page laid out
         * as a table was one line tall. */
        if (table_depth && tables[table_depth - 1].column >= 0 &&
            (piece->kind == PIECE_WORD || piece->kind == PIECE_BREAK)) {
            TABLE* self = &tables[table_depth - 1];
            LINE* target = &lines[self->cell_line];
            RUN* run = target->run_count ? &target->runs[target->run_count - 1]
                                         : (RUN*)0;
            int room = self->width[self->column];
            int width;
            int down = 0;

            if (!run) continue;
            if (piece->kind == PIECE_BREAK) {
                /* Nothing has been written in this cell yet, so there is
                   nothing to break from: a heading as the first thing in a
                   cell would otherwise push its own text down a line and
                   leave the colour behind the cell starting above it. */
                if (!run->count && self->cell_line == self->row_top) continue;
                down = 1 + piece->spaces;
            } else {
                width = piece->cells + piece->spaces + (run->count ? 1 : 0);
                if (run->count &&
                    cell_width_so_far(run, target) + width > room) down = 1;
            }

            if (down) {
                if (self->cell_line + down >= LINES_MAX) continue;
                for (int step = 0; step < down; step++) {
                    self->cell_line++;
                    if (self->cell_line > self->row_deepest) {
                        blank_line_with(&lines[self->cell_line], at, 0);
                        self->row_deepest = self->cell_line;
                    }
                }
                target = &lines[self->cell_line];
                if (target->run_count >= RUNS_MAX) continue;
                run = &target->runs[target->run_count++];
                run->first = at;
                run->count = 0;
                run->indent = (unsigned char)self->start[self->column];
            }
            if (piece->kind == PIECE_BREAK) continue;

            if (!run->count) run->first = at;
            run->count++;
            for (int open = 0; open < open_count; open++) {
                BOX* box = &boxes[open_boxes[open]];

                if (box->first_line < 0) box->first_line = self->cell_line;
                box->last_line = self->cell_line;
                /* A cell's edges belong to boxes inside the cell. A box that
                   was already open when the table started - the div the whole
                   page is in, usually - is not one column wide, and giving it
                   the column's edges turned it into a stripe beside the
                   text. */
                if (open >= self->open_outside) {
                    box->indent = (unsigned char)self->start[self->column];
                    box->width = (unsigned char)room;
                } else if (self->start[self->column] + room >
                           box_right[open_boxes[open]]) {
                    box_right[open_boxes[open]] =
                        self->start[self->column] + room;
                }
            }
            continue;
        }

        /* Anything else inside a table - a picture, a rule - is not placed
           in the cell yet, and going through the ordinary path would put it
           outside the table entirely. */
        if (table_depth && piece->kind != PIECE_BOX_START &&
            piece->kind != PIECE_BOX_END)
            continue;

        if (piece->kind == PIECE_BOX_START) {
            /* Opened, and nothing more: which lines it covers is decided by
             * what turns out to be inside it.
             *
             * Taking the line counter at this moment was the first attempt
             * and drew boxes that began before their contents and ended after
             * them - because a box opens before the break that starts its
             * first line, and closes after the one that ends its last. */
            if (piece->at < BOX_MAX && open_count < 16) {
                boxes[piece->at].first_line = -1;
                boxes[piece->at].last_line = -1;
                open_boxes[open_count++] = piece->at;
            }
            continue;
        }
        if (piece->kind == PIECE_BOX_END) {
            /* A box reaches down to where it closes, not merely to the last
             * word that happened to be counted into it.
             *
             * The difference is a box holding a table: its words all belong
             * to cells, and a cell keeps its own line counter, so the box
             * that wraps the whole page came out two lines tall - a white
             * stripe behind the heading with the rest of the page beside it
             * rather than inside it. */
            if (piece->at < BOX_MAX) {
                BOX* box = &boxes[piece->at];
                int here = table_depth ? tables[table_depth - 1].row_deepest
                                       : line_count - 1;

                if (box->first_line >= 0 && here > box->last_line)
                    box->last_line = here;
                /* And it is at least as wide as what is inside it. A box may
                   say how wide it is - `width: 800px` on a page written for a
                   screen of 1995 - and that is a wish, not a promise: what it
                   holds decides the rest. */
                if (box_right[piece->at] > box->indent + box->width)
                    box->width = (unsigned char)(box_right[piece->at] -
                                                 box->indent > 200
                                                 ? 200
                                                 : box_right[piece->at] -
                                                   box->indent);
            }
            /* The one that closed, not whichever was on top.
             *
             * Popping the top was right while every box nested tidily, and a
             * page with `<p><h3>...</h3></p>` in it does not: the inner box
             * closes first and took the outer one's place off the stack, so
             * the div wrapping the whole page stopped collecting lines
             * halfway through and was painted as a stripe behind the
             * heading. */
            for (int open = open_count - 1; open >= 0; open--)
                if (open_boxes[open] == (int)piece->at) {
                    for (int move = open; move + 1 < open_count; move++)
                        open_boxes[move] = open_boxes[move + 1];
                    open_count--;
                    break;
                }
            continue;
        }

        if (piece->kind == PIECE_IMAGE) {
            IMAGE* picture = &images[piece->at];
            int room = (columns - style->indent) * WINDOW_CHAR_W;
            int rows;
            int cells;
            int gap;
            int fits;

            /* Fitted to the column it is in, never enlarged: a badge of
               eighty-eight pixels is eighty-eight pixels, and a photograph
               wider than the window comes down to it. */
            picture->shown_width = picture->width;
            picture->shown_height = picture->height;
            if (room > 0 && picture->width > room) {
                picture->shown_width = room;
                picture->shown_height = picture->height * room / picture->width;
                if (picture->shown_height < 1) picture->shown_height = 1;
            }
            rows = (picture->shown_height + WINDOW_CHAR_H - 1) / WINDOW_CHAR_H;
            if (rows < 1) rows = 1;
            cells = (picture->shown_width + WINDOW_CHAR_W - 1) / WINDOW_CHAR_W;
            if (cells < 1) cells = 1;

            /* Pictures next to each other stay next to each other.
             *
             * Every picture used to take a line of its own, which is right
             * for a photograph in an article and wrong for the row of 88x31
             * badges at the bottom of every page of the small web - six of
             * them came out as a column six deep. They are laid side by side
             * now while they fit, exactly as words are, and the line is as
             * tall as the tallest of them. */
            gap = (image_line >= 0 && at == image_next && image_used) ? 1 : 0;
            fits = image_line >= 0 && at == image_next &&
                   image_used + gap + cells <=
                   columns - lines[image_line].runs[0].indent;

            if (!fits) {
                if (line->runs[0].count) {
                    if (line_count + 1 >= LINES_MAX) break;
                    line_count++;
                    line = &lines[line_count];
                    line->runs[0].count = 0;
                    line->cells = 0;
                }
                image_line = line_count;
                image_used = cells;
                line->run_count = 1;
                line->runs[0].first = at;
                line->runs[0].count = 1;
                line->runs[0].indent = style->indent;
                line->cells = cells;
                line->align = style->align;
                line->rule = 0;
                line->image = 1;
                line->image_rows = (unsigned char)(rows > 255 ? 255 : rows);
            } else {
                LINE* target = &lines[image_line];

                target->runs[0].count++;
                image_used += gap + cells;
                target->cells = image_used;
                if (rows > target->image_rows)
                    target->image_rows = (unsigned char)(rows > 255 ? 255
                                                                    : rows);
            }

            /* The rows the picture stands on, as empty lines - so that
               scrolling and the scrollbar count them like any others and the
               text after it starts below rather than through it. The line
               after them is where anything following goes, and a second
               picture on the same row reuses it rather than making another. */
            {
                int want = image_line + lines[image_line].image_rows;

                while (line_count < want) {
                    if (line_count + 2 >= LINES_MAX) break;
                    line_count++;
                    lines[line_count].run_count = 1;
                    lines[line_count].runs[0].first = at;
                    lines[line_count].runs[0].count = 0;
                    lines[line_count].runs[0].indent = style->indent;
                    lines[line_count].cells = 0;
                    lines[line_count].align = ALIGN_LEFT;
                    lines[line_count].rule = 0;
                    lines[line_count].image = 0;
                    lines[line_count].image_rows = 0;
                }
                if (line_count + 2 >= LINES_MAX) break;
                line = &lines[line_count];
                line->run_count = 1;
                line->runs[0].first = at + 1;
                line->runs[0].count = 0;
                line->runs[0].indent = style->indent;
                line->cells = 0;
                line->align = style->align;
                line->rule = 0;
                line->image = 0;
                line->image_rows = 0;
            }
            image_next = at + 1;
            continue;
        }

        if (piece->kind == PIECE_RULE) {
            if (line->runs[0].count) {
                if (line_count + 2 >= LINES_MAX) break;
                line_count++;
                line = &lines[line_count];
                line->runs[0].count = 0;
                line->cells = 0;
            }
            line->runs[0].first = at;
            line->runs[0].count = 1;
            line->runs[0].indent = style->indent;
            line->align = ALIGN_LEFT;
            line->rule = 1;
            if (line_count + 2 >= LINES_MAX) break;
            line_count++;
            line = &lines[line_count];
            line->run_count = 1;
            line->runs[0].first = at + 1;
            line->runs[0].count = 0;
            line->cells = 0;
            line->runs[0].indent = style->indent;
            line->align = style->align;
            line->rule = 0;
            continue;
        }

        usable = columns - style->indent;
        if (usable < 8) usable = 8;

        {
            int gap = (line->runs[0].count && !piece->glue) ? 1 : 0;
            int width = piece->cells + piece->spaces + gap;

            if (line->runs[0].count && line->cells + width > usable) {
                /* What is glued to this word comes with it.
                 *
                 * `<a>link</a>.` is a word and a full stop with nothing
                 * between them, and wrapping between the two put the full
                 * stop alone at the start of the next line - which is what a
                 * line breaker that knows about spaces and not about glue
                 * does to every marked-up page there is. */
                int start = at;
                int moved;

                while (start > line->runs[0].first && pieces[start].glue) start--;
                if (start == line->runs[0].first) start = at;   /* one long unit */
                moved = at - start;
                line->runs[0].count -= moved;

                line->cells = 0;
                for (int scan = 0; scan < line->runs[0].count; scan++) {
                    PIECE* had = &pieces[line->runs[0].first + scan];

                    line->cells += had->cells + had->spaces +
                                   ((scan && !had->glue) ? 1 : 0);
                }

                if (line_count + 1 >= LINES_MAX) break;
                line_count++;
                line = &lines[line_count];
                line->run_count = 1;
                line->runs[0].first = start;
                line->runs[0].count = moved;
                line->cells = 0;
                line->runs[0].indent = style->indent;
                line->align = style->align;
                line->rule = 0;
                for (int scan = 0; scan < moved; scan++) {
                    PIECE* had = &pieces[start + scan];

                    line->cells += had->cells + had->spaces +
                                   ((scan && !had->glue) ? 1 : 0);
                }
                width = piece->cells + piece->spaces +
                        ((line->runs[0].count && !piece->glue) ? 1 : 0);
            }
            if (!line->runs[0].count) {
                line->runs[0].first = at;
                line->runs[0].indent = style->indent;
                line->align = style->align;
            }
            line->runs[0].count++;
            line->cells += width;
        }
    }
    if (line->runs[0].count || line_count == 0) line_count++;
    if (top_line > line_count - 1) top_line = line_count - 1;
    if (top_line < 0) top_line = 0;
}

static void reset_layout(void) {
    laid_out_for = -1;
}

/* ---- The window ----------------------------------------------------------- */

#define BAR_HEIGHT 26
#define PAGE_TOP (BAR_HEIGHT + 4)
#define BUTTON_WIDTH 34
#define BUTTONS 4

static char address[URL_MAX] = "koi:home";

/* What a relative link is relative to.
 *
 * Not `address`, which is what the window shows and belongs to the desktop's
 * thread: it is written when the fetch has finished, and the worker resolves
 * the page's pictures and stylesheets before that - against the address of
 * the page *before* this one. Every relative picture on every page was
 * therefore fetched from the wrong place, or from nowhere at all, and only
 * pages whose pictures were written out in full ever showed any. Written by
 * whoever starts a page, before anything is resolved against it. */
static char base[URL_MAX] = "koi:home";
static char status[128] = "Ready.";
static char typed[URL_MAX];      /* what is in the address bar being edited */
static int focused;              /* the address bar has the keyboard */
static int caret;                /* where in `typed`, in bytes */
static int caret_on;
/* Everything is selected, as it is the moment a browser's address bar takes
   focus. There is no selection to draw here, so it lives as one bit: the next
   thing typed replaces the address, and anything else - an arrow, a click -
   means the address was meant to be edited and keeps it. It is the difference
   between typing a new address and finding it appended to the old one. */
static int select_all;
static int field_left;           /* first cell of `typed` that is visible */

static char back_stack[HISTORY_MAX][URL_MAX];
static int back_count;
static char forward_stack[HISTORY_MAX][URL_MAX];
static int forward_count;

typedef struct { int x, y, width, link; } HOTSPOT;
static HOTSPOT hotspots[HOTSPOTS_MAX];
static int hotspot_count;
static int shown_rows;

static void report(const char* what) {
    copy_text(status, sizeof(status), what);
}

static void button_rect(int index, int x, int y, int* out_x, int* out_width) {
    (void)y;
    *out_x = x + 4 + index * (BUTTON_WIDTH + 2);
    *out_width = BUTTON_WIDTH;
}

/* One picture on the screen, clipped to the window and scaled to the width it
 * was given.
 *
 * Scaled a row at a time through a small buffer rather than into a second copy
 * of the whole picture: a page of badges would otherwise hold every one of
 * them twice, and the nearest-neighbour sampling that fits a picture to a
 * column costs nothing per row.
 *
 * `top` is where the picture begins, which may be above the visible area when
 * the page is scrolled; `first` and `last` are the rows of the window it may
 * draw between. */
static void draw_image(IMAGE* picture, int x, int top, int first, int last);

/* Every picture on one line, in the order they were written, with the line's
   own alignment - a row of badges under `text-align: center` sits in the
   middle of the page the way the page asked. */
static void draw_image_row(LINE* line, int columns, int left, int top,
                           int first, int last) {
    int cell = line->runs[0].indent;

    if (line->align == ALIGN_CENTRE)
        cell += (columns - line->runs[0].indent - line->cells) / 2;
    else if (line->align == ALIGN_RIGHT)
        cell += columns - line->runs[0].indent - line->cells;
    if (cell < line->runs[0].indent) cell = line->runs[0].indent;

    for (int which = 0; which < line->runs[0].count; which++) {
        PIECE* piece = &pieces[line->runs[0].first + which];
        IMAGE* picture;
        int cells;

        if (piece->kind != PIECE_IMAGE || piece->at >= IMAGE_MAX) continue;
        picture = &images[piece->at];
        cells = (picture->shown_width + WINDOW_CHAR_W - 1) / WINDOW_CHAR_W;
        if (cells < 1) cells = 1;
        draw_image(picture, left + cell * WINDOW_CHAR_W, top, first, last);
        cell += cells + 1;                 /* the space between two of them */
    }
}

static void draw_image(IMAGE* picture, int x, int top, int first, int last) {
    static koi_uint32 row[2048];

    if (!picture->pixels || picture->shown_width <= 0) return;

    for (int line = 0; line < picture->shown_height; line++) {
        int at_y = top + line;
        int source_row;

        if (at_y < first) continue;
        if (at_y >= last) break;

        source_row = picture->shown_height == picture->height
                     ? line
                     : line * picture->height / picture->shown_height;
        if (source_row >= picture->height) source_row = picture->height - 1;

        if (picture->shown_width == picture->width &&
            picture->width <= (int)(sizeof(row) / sizeof(row[0]))) {
            koi_gfx_blit(x, at_y, picture->width, 1,
                         picture->pixels + (long)source_row * picture->width,
                         picture->width);
            continue;
        }
        {
            int wide = picture->shown_width;

            if (wide > (int)(sizeof(row) / sizeof(row[0])))
                wide = (int)(sizeof(row) / sizeof(row[0]));
            for (int column = 0; column < wide; column++) {
                int source = column * picture->width / picture->shown_width;

                if (source >= picture->width) source = picture->width - 1;
                row[column] = picture->pixels[(long)source_row * picture->width +
                                              source];
            }
            koi_gfx_blit(x, at_y, wide, 1, row, wide);
        }
    }
}

static void paint(WINDOW* self, int x, int y, int width, int height) {
    int columns;
    int rows;
    int field_x;
    int field_width;
    static const char* labels[BUTTONS] = { "<", ">", "R", "^" };

    (void)self;
    koi_gfx_fill(x, y, width, height,
                 page_has_background ? page_background
                                     : mizu->color(MIZU_COLOR_PAPER));
    /* The bar is the desktop's colour whatever the page's is: it is part of
       the window rather than part of the document, and a page that sets a
       black background must not be able to hide the address it is at. */
    koi_gfx_fill(x, y, width, BAR_HEIGHT, mizu->color(MIZU_COLOR_FACE));

    for (int index = 0; index < BUTTONS; index++) {
        int at_x, button_width;

        button_rect(index, x, y, &at_x, &button_width);
        mizu->raised(at_x, y + 3, button_width, BAR_HEIGHT - 7);
        {
            int enabled = 1;

            if (index == 0) enabled = back_count > 0;
            if (index == 1) enabled = forward_count > 0;
            mizu->label(at_x + button_width / 2 - WINDOW_CHAR_W / 2,
                        y + 3 + (BAR_HEIGHT - 7 - WINDOW_CHAR_H) / 2,
                        labels[index],
                        enabled ? mizu->color(MIZU_COLOR_TEXT)
                                : mizu->color(MIZU_COLOR_SHADOW));
        }
    }

    field_x = x + 4 + BUTTONS * (BUTTON_WIDTH + 2) + 4;
    field_width = width - (field_x - x) - 4;
    if (field_width < 40) field_width = 40;
    mizu->sunken(field_x, y + 3, field_width, BAR_HEIGHT - 7);

    /* The address, and the caret when it is being typed in.
     *
     * The text is scrolled rather than clipped: a URL longer than the bar was
     * unreadable from the middle onwards, and worse, the caret went off the
     * end and typing became blind. */
    {
        const char* showing = focused ? typed : address;
        int inner_x = field_x + 3;
        int inner_cells = (field_width - 6) / WINDOW_CHAR_W;
        int caret_cell = focused ? cells_of(showing, caret) : 0;
        int from;

        if (!focused) field_left = 0;
        if (caret_cell < field_left) field_left = caret_cell;
        if (caret_cell > field_left + inner_cells - 1)
            field_left = caret_cell - inner_cells + 1;
        if (field_left < 0) field_left = 0;

        from = bytes_for_cells(showing, field_left);
        {
            char visible[URL_MAX];
            int length = bytes_for_cells(showing + from, inner_cells);

            for (int at = 0; at < length && at + 1 < (int)sizeof(visible); at++)
                visible[at] = showing[from + at];
            visible[length < (int)sizeof(visible) - 1 ? length
                                                      : (int)sizeof(visible) - 1] = 0;
            mizu->label(inner_x, y + 3 + (BAR_HEIGHT - 7 - WINDOW_CHAR_H) / 2,
                        visible, mizu->color(MIZU_COLOR_TEXT));
        }
        if (focused && caret_on)
            koi_gfx_fill(inner_x + (caret_cell - field_left) * WINDOW_CHAR_W,
                         y + 3 + (BAR_HEIGHT - 7 - WINDOW_CHAR_H) / 2, 1,
                         WINDOW_CHAR_H, mizu->color(MIZU_COLOR_TEXT));
    }

    rows = (height - PAGE_TOP - WINDOW_CHAR_H - 2) / WINDOW_CHAR_H;
    if (rows < 1) rows = 1;
    shown_rows = rows;
    columns = (width - 16 - WINDOW_SCROLLBAR_W) / WINDOW_CHAR_W;
    if (columns < 8) columns = 8;
    layout(columns);

    mizu->scrollbar(x + width - WINDOW_SCROLLBAR_W, y + PAGE_TOP,
                    rows * WINDOW_CHAR_H, top_line, rows, line_count);

    /* The boxes first, so the words land on top of them.
     *
     * Drawn from the outside in, in the order they were opened: a box inside
     * another must not be painted over by its parent, and the order the page
     * declares them in is exactly the order that gets that right. */
    for (int at = 0; at < box_count; at++) {
        BOX* box = &boxes[at];
        int top, bottom, left, wide;

        if (box->first_line < 0) continue;
        if (box->last_line < top_line) continue;
        if (box->first_line > top_line + rows) continue;

        top = y + PAGE_TOP + (box->first_line - top_line) * WINDOW_CHAR_H;
        bottom = y + PAGE_TOP + (box->last_line - top_line + 1) * WINDOW_CHAR_H;
        if (top < y + PAGE_TOP) top = y + PAGE_TOP;
        if (bottom > y + PAGE_TOP + rows * WINDOW_CHAR_H)
            bottom = y + PAGE_TOP + rows * WINDOW_CHAR_H;
        if (bottom <= top) continue;

        left = x + 8 + box->indent * WINDOW_CHAR_W;
        wide = box->width ? box->width * WINDOW_CHAR_W
                          : (columns - box->indent) * WINDOW_CHAR_W;
        if (left + wide > x + width - WINDOW_SCROLLBAR_W - 4)
            wide = x + width - WINDOW_SCROLLBAR_W - 4 - left;
        if (wide <= 0) continue;

        /* The colour behind it, now that a cell knows how wide it is.
         *
         * This was drawn as an outline only for one build, because without
         * column widths both cells of a two-column table claimed the whole
         * page and the second painted over the first - and a filled rectangle
         * in the wrong place hides the text under it, which is worse than no
         * boxes at all. With the columns measured, a box inside a cell takes
         * that cell's edges. */
        if (box->has_background)
            koi_gfx_fill(left, top, wide, bottom - top, box->background);
        if (box->has_border) {
            koi_gfx_fill(left, top, wide, 1, box->border);
            koi_gfx_fill(left, bottom - 1, wide, 1, box->border);
            koi_gfx_fill(left, top, 1, bottom - top, box->border);
            koi_gfx_fill(left + wide - 1, top, 1, bottom - top, box->border);
        }
    }

    hotspot_count = 0;

    /* A picture taller than a line may begin above what is on screen. The
       loop below starts at the first visible line and would miss it entirely,
       so the lines just above are looked at for one that reaches down here. */
    for (int back = 1; back < 64 && top_line - back >= 0; back++) {
        LINE* earlier = &lines[top_line - back];

        if (!earlier->image) continue;
        if (earlier->image_rows <= back) break;
        draw_image_row(earlier, columns,
                       x + 8, y + PAGE_TOP - back * WINDOW_CHAR_H,
                       y + PAGE_TOP, y + PAGE_TOP + rows * WINDOW_CHAR_H);
        break;
    }

    for (int row = 0; row < rows; row++) {
        int index = top_line + row;
        LINE* line;
        int at_y = y + PAGE_TOP + row * WINDOW_CHAR_H;
        int cell;

        if (index < 0 || index >= line_count) break;
        line = &lines[index];
        if (!line->runs[0].count) continue;

        if (line->rule) {
            koi_gfx_fill(x + 8, at_y + WINDOW_CHAR_H / 2,
                         width - 16 - WINDOW_SCROLLBAR_W, 1,
                         mizu->color(MIZU_COLOR_SHADOW));
            continue;
        }

        if (line->image) {
            draw_image_row(line, columns, x + 8, at_y,
                           y + PAGE_TOP, y + PAGE_TOP + rows * WINDOW_CHAR_H);
            continue;
        }

        /* Every run of the line, each with its own left edge: one for
           ordinary text, one per column when the line is inside a table
           row. */
        for (int which_run = 0; which_run < line->run_count; which_run++) {
            RUN* run = &line->runs[which_run];

            cell = run->indent;
            if (!which_run && line->align == ALIGN_CENTRE)
                cell += (columns - run->indent - line->cells) / 2;
            else if (!which_run && line->align == ALIGN_RIGHT)
                cell += columns - run->indent - line->cells;
            if (cell < 0) cell = 0;

            for (int which = 0; which < run->count; which++) {
                PIECE* piece = &pieces[run->first + which];
                STYLE* style;
                char word[192];
                int length;
                int at_x;

                if (piece->kind != PIECE_WORD) continue;
                style = &styles[piece->style];
                if (which && !piece->glue) cell++;   /* the space between */
                cell += piece->spaces;
                at_x = x + 8 + cell * WINDOW_CHAR_W;

                length = piece->length < (int)sizeof(word) - 1
                         ? piece->length : (int)sizeof(word) - 1;
                for (int scan = 0; scan < length; scan++)
                    word[scan] = text[piece->at + scan];
                word[length] = 0;

                if (style->has_background)
                    koi_gfx_fill(at_x, at_y, piece->cells * WINDOW_CHAR_W,
                                 WINDOW_CHAR_H, style->background);

                mizu->label_styled(at_x, at_y, word, style->color,
                                   (style->bold ? KOI_TEXT_BOLD : 0) |
                                   (style->italic ? KOI_TEXT_ITALIC : 0) |
                                   (style->underline ? KOI_TEXT_UNDERLINE : 0));

                if (piece->link != NO_LINK && hotspot_count < HOTSPOTS_MAX) {
                    hotspots[hotspot_count].x = at_x;
                    hotspots[hotspot_count].y = at_y;
                    hotspots[hotspot_count].width = piece->cells * WINDOW_CHAR_W;
                    hotspots[hotspot_count].link = piece->link;
                    hotspot_count++;
                }
                cell += piece->cells;
            }
        }
    }

    /* And in words, beside the status: the bar says where in the page this
       is, and a number says it exactly. */
    {
        char line[160];

        if (line_count > rows)
            koi_snprintf(line, sizeof(line), "%s   [%d%%]", status,
                         top_line * 100 / (line_count - rows));
        else copy_text(line, sizeof(line), status);
        mizu->label(x + 8, y + height - WINDOW_CHAR_H, line,
                    mizu->color(MIZU_COLOR_SHADOW));
    }
}

/* ---- Going somewhere ------------------------------------------------------ */

static void render(void);
static void render_errands(void);
static void render_layout(void);
static void go(const char* url, int remember);

/* Split "host:port/path" out of whatever was typed, and refuse what cannot
   work. The port is separate because a name with a colon in it resolves to
   nothing, and "it does not resolve" is a wretched thing to be told about an
   address that is perfectly good apart from where this stopped reading. */
/* Whether what was typed is a file on this machine rather than a name on the
 * network.
 *
 * `Z:\KOI.HTM` and `KOI.HTM` are both files, and both were handed to the
 * resolver as host names - which asks a DNS server about "z:\koi.htm", waits
 * for the timeout with the whole desktop stopped behind it, and then says the
 * name does not resolve. A browser that cannot open a file on the disk it is
 * running from is a strange browser, and a browser that freezes the machine
 * to find that out is a broken one. */
static int looks_like_a_file(const char* what) {
    if (!what[0]) return 0;
    if (same_ignoring_case(what, "http://", 7) ||
        same_ignoring_case(what, "https://", 8) ||
        same_ignoring_case(what, "koi:", 4)) return 0;
    if (what[0] == '\\') return 1;
    if (what[1] == ':' && ((what[0] >= 'A' && what[0] <= 'Z') ||
                           (what[0] >= 'a' && what[0] <= 'z'))) return 1;
    for (int at = 0; what[at]; at++) if (what[at] == '\\') return 1;
    /* A bare name with a file's ending and no dots in between: HELLO.HTM is a
       file, and example.com is not. */
    if (ends_with_ignoring_case(what, ".htm") ||
        ends_with_ignoring_case(what, ".html") ||
        ends_with_ignoring_case(what, ".txt") ||
        ends_with_ignoring_case(what, ".md") ||
        ends_with_ignoring_case(what, ".log")) {
        for (int at = 0; what[at]; at++) if (what[at] == '/') return 0;
        return 1;
    }
    return 0;
}

static int split(const char* url, char* host, char* path, int* port,
                 int* secure) {
    int at = 0;
    int out = 0;

    if (port) *port = 80;
    if (secure) *secure = 0;
    if (same_ignoring_case(url, "https://", 8)) {
        if (secure) *secure = 1;
        if (port) *port = 443;
        url += 8;
    } else if (same_ignoring_case(url, "http://", 7)) {
        url += 7;
    }

    while (url[at] && url[at] != '/' && url[at] != ':' && out + 1 < URL_MAX)
        host[out++] = url[at++];
    host[out] = 0;
    if (url[at] == ':') {
        int number = 0;

        at++;
        while (url[at] >= '0' && url[at] <= '9')
            number = number * 10 + (url[at++] - '0');
        if (port && number > 0 && number < 65536) *port = number;
    }
    out = 0;
    while (url[at] && out + 1 < URL_MAX) path[out++] = url[at++];
    path[out] = 0;
    if (!path[0]) { path[0] = '/'; path[1] = 0; }
    return host[0] != 0;
}

/* One GET, into a buffer, with the headers taken off and the charset the
   server claimed handed back. Shared by the page and its stylesheets, which
   are the same errand to the same server. */
static long http_get(const char* url, char* into, long size, int* charset) {
    char host[URL_MAX];
    char path[URL_MAX];
    char request[URL_MAX * 2 + 128];
    unsigned int ip = 0;
    int connection;
    int port = 80;
    int secure = 0;
    long have = 0;
    long body = 0;
    int code = 0;

    if (charset) *charset = -1;
    moved_to[0] = 0;
    last_status = 0;
    page_truncated = 0;
    identity_checked = 0;
    content_is_html = -1;             /* until the server says, or the bytes do */
    if (!split(url, host, path, &port, &secure)) return -1;

    /* No repaints from here down: this runs in the worker, and drawing
       belongs to the thread that owns the screen. The status line is written
       and the desktop picks it up on its own next turn. */
    report("Looking up the name...");
    if (koi_net_resolve(host, &ip) != 0 || !ip) {
        report("That name does not resolve.");
        return -1;
    }

    report(secure ? "Connecting, in private..." : "Connecting...");
    connection = secure ? koi_tls_connect(ip, port, host, 12000)
                        : koi_tcp_connect(ip, port, 8000);
    if (connection < 0) {
        char line[80];

        if (secure)
            koi_snprintf(line, sizeof(line),
                         "The private connection could not be set up.");
        else
            koi_snprintf(line, sizeof(line), "Nothing answered on port %d.",
                         port);
        report(line);
        return -1;
    }
    if (secure) identity_checked = koi_tls_checked(connection);

    request[0] = 0;
    strcat(request, "GET ");
    strcat(request, path);
    strcat(request, " HTTP/1.0\r\nHost: ");
    strcat(request, host);
    /* The port goes in the Host header only when it is not the default for
       the scheme. It was appended whenever it was not 80, so every https
       request said `Host: name:443` - which a content delivery network reads
       as a site it has never heard of, and answers with its own "not found"
       page. Koi's own site was unreachable from this browser and perfectly
       reachable from everything else, which is exactly how a wrong Host
       header looks from the outside. */
    if (port != (secure ? 443 : 80)) {
        char suffix[8];

        koi_snprintf(suffix, sizeof(suffix), ":%d", port);
        strcat(request, suffix);
    }
    strcat(request, "\r\nUser-Agent: Nami/0.3 (Koi-DOS)\r\n"
                    "Accept: text/html,text/css,text/plain\r\n"
                    "Connection: close\r\n\r\n");
    if (secure)
        koi_tls_send(connection, request, (unsigned int)strlen(request), 8000);
    else
        koi_tcp_send(connection, request, (unsigned int)strlen(request), 8000);

    report("Reading...");
    for (;;) {
        char chunk[1460];
        int got = secure
                  ? koi_tls_receive(connection, chunk, sizeof(chunk), 8000)
                  : koi_tcp_receive(connection, chunk, sizeof(chunk), 8000);

        if (got <= 0) break;
        {
            char line[64];

            koi_snprintf(line, sizeof(line), "Reading... %ld bytes", have);
            report(line);
        }
        for (int index = 0; index < got && have + 1 < size; index++)
            into[have++] = chunk[index];
        /* No yielding either: this thread giving up its turn is the kernel's
           business now, and it takes it away by the clock. The desktop is
           drawing on its own thread the whole time. */
        if (fetch_cancel) break;
        if (have + 1 >= size) { page_truncated = 1; break; }
    }
    if (secure) koi_tls_close(connection);
    else koi_tcp_close(connection);

    for (long at = 0; at + 3 < have; at++) {
        if (into[at] == 13 && into[at + 1] == 10 && into[at + 2] == 13 &&
            into[at + 3] == 10) { body = at + 4; break; }
        if (into[at] == 10 && into[at + 1] == 10) { body = at + 2; break; }
    }
    if (have > 12 && same_ignoring_case(into, "http/", 5)) {
        code = (into[9] - '0') * 100 + (into[10] - '0') * 10 + (into[11] - '0');
        last_status = code;
    }

    /* Where it moved to, when it says it moved. Read before the headers are
       thrown away, and acted on by the caller rather than here: this function
       fetches one thing from one place, and following a trail is a decision
       about how many hops are too many. */
    if (code >= 300 && code < 400)
        for (long at = 0; at + 10 < body; at++)
            if ((into[at] == 'l' || into[at] == 'L') &&
                same_ignoring_case(into + at, "location:", 9)) {
                long scan = at + 9;
                int out = 0;

                while (scan < body && (into[scan] == ' ' || into[scan] == '\t'))
                    scan++;
                while (scan < body && into[scan] != 13 && into[scan] != 10 &&
                       out + 1 < (int)sizeof(moved_to))
                    moved_to[out++] = into[scan++];
                moved_to[out] = 0;
                break;
            }

    /* What kind of thing this is, if the server was willing to say. */
    for (long at = 0; at + 14 < body; at++)
        if ((into[at] == 'c' || into[at] == 'C') &&
            same_ignoring_case(into + at, "content-type:", 13)) {
            long scan = at + 13;
            long stop = scan + 64 < body ? scan + 64 : body;

            content_is_html = 0;
            for (; scan < stop; scan++)
                if (same_ignoring_case(into + scan, "html", 4) ||
                    same_ignoring_case(into + scan, "xml", 3)) {
                    content_is_html = 1;
                    break;
                }
            break;
        }

    /* What the server says the alphabet is, which outranks what the document
       says about itself - the server is the one that just read the file. */
    if (charset)
        for (long at = 0; at + 8 < body; at++)
            if ((into[at] == 'c' || into[at] == 'C') &&
                same_ignoring_case(into + at, "charset", 7)) {
                *charset = charset_after(into + at + 7, body - at - 7);
                break;
            }

    if (body) {
        for (long at = 0; at + body < have; at++) into[at] = into[at + body];
        have -= body;
    }

    if (code && (code < 200 || code >= 300)) {
        char line[80];

        /* A redirect is a page this cannot follow yet and can name, which is
           better than showing the empty body a redirect usually has. */
        if (code >= 300 && code < 400 && moved_to[0]) {
            /* Said quietly: the caller is about to follow it, and a line
               about a redirect that worked is noise. */
            return -1;
        }
        koi_snprintf(line, sizeof(line), "The server answered %d.", code);
        report(line);
        if (code >= 300 && code < 400) return -1;
    }
    return have;
}

/* Where a link points, made absolute against where we are now. */
static void resolve_link(const char* href, char* out) {
    char host[URL_MAX];
    char path[URL_MAX];
    int port = 80;
    int secure = 0;

    if (same_ignoring_case(href, "http://", 7) ||
        same_ignoring_case(href, "https://", 8) ||
        same_ignoring_case(href, "koi:", 4)) {
        copy_text(out, URL_MAX, href);
        return;
    }
    if (href[0] == '/' && href[1] == '/') {
        koi_snprintf(out, URL_MAX, "http:%s", href);
        return;
    }
    if (href[0] == '#') {
        /* An anchor within this page. Nothing here can scroll to a name yet,
           so it is the page itself - which at least does not leave. */
        copy_text(out, URL_MAX, base);
        return;
    }
    if (!split(base, host, path, &port, &secure)) {
        copy_text(out, URL_MAX, href);
        return;
    }

    /* The scheme comes with it.
     *
     * A relative link on an https page is an https link, and building it out
     * of the host and the path alone loses that - so every picture and every
     * stylesheet on a secure page was fetched as though it were plain, on
     * port 443, which no server answers. It went unnoticed while pages were
     * only text, because a page's own address always carried its scheme. */
    {
        char with_scheme[URL_MAX];

        if (port != (secure ? 443 : 80))
            koi_snprintf(with_scheme, sizeof(with_scheme), "%s://%s:%d",
                         secure ? "https" : "http", host, port);
        else
            koi_snprintf(with_scheme, sizeof(with_scheme), "%s://%s",
                         secure ? "https" : "http", host);
        copy_text(host, URL_MAX, with_scheme);
    }

    if (href[0] == '/') {
        koi_snprintf(out, URL_MAX, "%s%s", host, href);
        return;
    }
    {
        int cut = 0;

        for (int at = 0; path[at]; at++) if (path[at] == '/') cut = at + 1;
        path[cut] = 0;
        koi_snprintf(out, URL_MAX, "%s%s%s", host, path, href);
    }
}

/* The page this opens with, and what Home goes back to.
 *
 * Written in HTML with a stylesheet, because the first thing anybody should
 * see is the thing the browser does. It is also the test: if this page comes
 * out centred, coloured and indented, the cascade below works. */
static const char* home_page =
    "<html><head><title>Nami Explorer</title><style>"
    "body{background:paper;color:text}"
    "h1{text-align:center;color:accent}"
    ".subtitle{text-align:center;color:shadow}"
    ".card{margin-left:2}"
    ".note{color:shadow;margin-left:2}"
    "code{color:accent}"
    "</style></head><body>"
    "<h1>Nami Explorer</h1>"
    /* No kanji here, however much the name deserves one: the font carries
       Latin, Cyrillic and Greek, and a shape it has not got would be a hole in
       the middle of the one line that introduces the thing. */
    "<p class='subtitle'>a wave, over the water</p>"
    "<hr>"
    "<h2>Somewhere to start</h2>"
    "<ul class='card'>"
    "<li><a href='http://info.cern.ch/'>info.cern.ch</a> &#x2014; "
    "the first web site there was, and still served over plain HTTP</li>"
    "<li><a href='http://example.com/'>example.com</a> &#x2014; "
    "one paragraph, useful when something looks wrong</li>"
    "<li><a href='http://neverssl.com/'>neverssl.com</a> &#x2014; "
    "a page that promises never to move to https</li>"
    "<li><a href='http://textfiles.com/'>textfiles.com</a> &#x2014; "
    "the whole of the bulletin board era, in plain text</li>"
    "<li><a href='http://195.133.195.44/INDEX'>the package server</a> &#x2014; "
    "what <code>dosget</code> reads</li>"
    "</ul>"
    "<h2>What works here</h2>"
    "<ul class='card'>"
    "<li>Stylesheets &#x2014; <code>&lt;style&gt;</code>, "
    "<code>style=</code> and <code>&lt;link&gt;</code>, with the cascade</li>"
    "<li>Russian, in UTF-8, windows-1251, KOI8-R or code page 866</li>"
    "<li>Back, forward and the address bar you can type in</li>"
    "</ul>"
    "<h2>About https</h2>"
    "<p class='note'>It works: TLS 1.3, X25519, ChaCha20-Poly1305, all of it "
    "written here. What is <b>not</b> done is checking the certificate - so a "
    "page is private from anybody watching the wire, and worth nothing "
    "against somebody able to answer in the server's place. The status line "
    "says which, every time, and there is no padlock until it can be "
    "earned.</p>"
    "<h2>What does not work</h2>"
    "<p class='note'>Pictures on a page - the decoder is written, the layout "
    "is next. Scripts, on purpose.</p>"
    "</body></html>";

/* Whether these bytes look like a document, for a server that did not say.
   The first tag inside the first kilobyte decides it: a text file may well
   contain a `<` but very rarely one followed straight by a letter. */
static int looks_like_html(void) {
    long limit = page_length < 1024 ? page_length : 1024;

    for (long at = 0; at + 1 < limit; at++) {
        char next = page[at + 1];

        if (page[at] != '<') continue;
        if (next == '!' || next == '/' ||
            (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'))
            return 1;
    }
    return 0;
}

/* A text file, shown as one: every line where it was written, every space
   kept, nothing marked up. This is what the whole of textfiles.com is, and
   what a README fetched off a server is. */
static void plain_pass(void) {
    STYLE plain;

    text_length = 0;
    piece_count = 0;
    link_count = 0;
    link_pool_used = 0;
    pending_word = -1;
    pending_spaces = 0;
    pending_break = 0;
    pending_blank = 0;
    glue_next = 0;
    depth = 0;
    page_has_background = 0;
    style_count = 0;

    memset(&plain, 0, sizeof(plain));
    plain.color = mizu->color(MIZU_COLOR_TEXT);
    plain.pre = 1;
    memset(&stack[0], 0, sizeof(ELEMENT));
    stack[0].style = plain;
    stack[0].link = NO_LINK;
    stack[0].style_index = intern_style(&plain);
    copy_text(stack[0].tag, sizeof(stack[0].tag), "pre");

    for (long at = 0; at < page_length; at++) {
        char c = page[at];

        if (c == '\r') continue;
        if (c == '\n') {
            flush_word();
            want_break(0);
            flush_break();
            continue;
        }
        if (c == ' ' || c == '\t') {
            int spaces = c == '\t' ? 8 - (pending_spaces % 8) : 1;

            if (pending_word < 0) pending_spaces += spaces;
            else { flush_word(); pending_spaces = spaces; }
            continue;
        }
        add_character(c);
    }
    flush_word();
    page_title[0] = 0;
}

/* ---- Where the work is done, and by whom --------------------------------
 *
 * Reading a page is two jobs with a line between them, and the line is what
 * makes threads safe here.
 *
 * `render_errands` is everything that touches the network: the head is read to
 * find the stylesheets, and each of those is fetched. It runs in the worker
 * thread and touches nothing the desktop owns - no windows, no repaints, no
 * calls into Mizu at all.
 *
 * `render_layout` is everything that turns those bytes into words on a screen:
 * the cascade, the pieces, the title. It runs on the desktop's own thread, in
 * `tick`, when the worker has finished - so nothing it touches is being
 * written by anybody else at the time.
 */
static void fetch_image(IMAGE* image);

static void render_errands(void) {
    if (!page_is_html) return;

    first_pass();

    /* The page's own stylesheets, each of which is another errand. Fetched
       before anything is styled, because a rule cannot be applied to a word
       that has already been made. */
    for (int at = 0; at < sheet_count; at++) {
        char url[URL_MAX];
        long got;

        if (css_length + 256 >= CSS_MAX) break;
        resolve_link(sheets[at], url);
        if (same_ignoring_case(url, "koi:", 4)) continue;
        report("Fetching a stylesheet...");
        got = http_get(url, css + css_length, CSS_MAX - css_length - 1, 0);
        if (got > 0) css_length += got;
    }

    /* And then the pictures, in the order they appear. Each is announced, so
       that a page of many says what it is doing rather than seeming stuck. */
    for (int at = 0; at < image_count; at++) {
        char line[96];

        if (fetch_cancel) break;
        koi_snprintf(line, sizeof(line), "Picture %d of %d...", at + 1,
                     image_count);
        report(line);
        fetch_image(&images[at]);
    }
}

/* One picture: fetched, decoded, and measured. Runs in the worker, so it
 * touches nothing but this table and the buffer it asks for.
 *
 * PNG and GIF are told apart by what is in the file rather than by the name
 * it was fetched under - a server that calls a GIF ".png" is a server, not a
 * reason to refuse. */
static void fetch_image(IMAGE* image) {
    /* One buffer for every picture on the page, taken once and kept.
     *
     * This used to be two megabytes taken and given back per picture, and an
     * animation kept its two megabytes for as long as the page was open -
     * six badges on a page held twelve megabytes to carry forty kilobytes of
     * GIF. The file a picture keeps is a copy of exactly its own size now,
     * and this buffer is only ever borrowed. */
    static koi_uint8* download;
    long size;
    koi_uint8* file;
    koi_uint8* scratch = (koi_uint8*)0;
    int width = 0, height = 0;

    if (image_bytes >= IMAGE_BYTES_MAX) { image->failed = 1; return; }

    if (!download) download = (koi_uint8*)koi_alloc(IMAGE_FILE_MAX);
    file = download;
    if (!file) { image->failed = 1; return; }
    size = http_get(image->url, (char*)file, IMAGE_FILE_MAX, (int*)0);
    if (size <= 0) { image->failed = 1; return; }

    if (gif_is_gif(file, size)) {
        GIF gif;

        if (!gif_open(file, size, &gif)) { image->failed = 1; return; }
        width = gif.width;
        height = gif.height;
        if ((long)width * height * 4 + image_bytes > IMAGE_BYTES_MAX) {
            image->failed = 1;
            return;
        }
        image->pixels = (koi_uint32*)koi_alloc((long)width * height * 4);
        scratch = (koi_uint8*)koi_alloc((long)width * height + 64);
        if (!image->pixels || !scratch) {
            if (scratch) koi_free(scratch);
            image->failed = 1;
            return;
        }
        if (!gif_frame(&gif, 0, image->pixels, 0xFFFFFF, scratch,
                       (long)width * height + 64)) {
            koi_free(scratch);
            image->failed = 1;
            return;
        }
        koi_free(scratch);
        image->frame_count = gif.frame_count;
        image->frame = 0;
        image->delay_ms = gif.frames[0].delay_ms;
        /* An animation keeps its file: the frames after the first are changes
           to this canvas and cannot be decoded without the bytes they came
           from. Its own copy, of its own size - the buffer above is borrowed
           and the next picture will want it. */
        if (gif.frame_count > 1) {
            image->file = (koi_uint8*)koi_alloc(size);
            if (image->file) {
                for (long at = 0; at < size; at++) image->file[at] = file[at];
                image->file_length = size;
            } else {
                image->frame_count = 1;    /* still, rather than nothing */
            }
        }
    } else if (png_is_png(file, size)) {
        PNG picture;
        long work_size;
        koi_uint8* work;

        if (!png_size(file, size, &width, &height)) {
            image->failed = 1;
            return;
        }
        if ((long)width * height * 4 + image_bytes > IMAGE_BYTES_MAX) {
            image->failed = 1;
            return;
        }
        work_size = size + ((long)width * 4 + 1) * height + 64;
        work = (koi_uint8*)koi_alloc(work_size);
        image->pixels = (koi_uint32*)koi_alloc((long)width * height * 4);
        if (!work || !image->pixels) {
            if (work) koi_free(work);
            image->failed = 1;
            return;
        }
        for (long at = 0; at < size; at++) work[at] = file[at];
        png_scratch(work, work_size);
        if (!png_decode(work, size, image->pixels, (long)width * height,
                        0xFFFFFF, &picture)) {
            koi_free(work);
            image->failed = 1;
            return;
        }
        koi_free(work);
    } else {
        /* A JPEG, most likely - the one common format this cannot read yet. */
        image->failed = 1;
        return;
    }

    image->width = width;
    image->height = height;
    image_bytes += (long)width * height * 4;
}

static void render_layout(void) {
    if (!page_is_html) {
        plain_pass();
        reset_layout();
        top_line = 0;
        if (window) copy_text(window->title, WINDOW_TITLE_MAX, "Nami Explorer");
        return;
    }

    rule_count = 0;
    style_count = 0;
    parse_stylesheet(browser_stylesheet, (long)strlen(browser_stylesheet), 0);
    if (css_length) parse_stylesheet(css, css_length, 1);

    second_pass();
    reset_layout();
    top_line = 0;

    if (window) {
        if (page_title[0])
            copy_text(window->title, WINDOW_TITLE_MAX, page_title);
        else copy_text(window->title, WINDOW_TITLE_MAX, "Nami Explorer");
    }
}

/* Both halves, for the pages that come from the disk or from memory: there is
   no waiting in those, so there is nothing to move off this thread. */
static void render(void) {
    if (page_is_html < 0) page_is_html = looks_like_html();
    render_errands();
    render_layout();
}

static void load_from_disk(const char* path);

/* ---- Fetching in a thread of its own -------------------------------------
 *
 * A page took the whole machine with it while it loaded: the desktop's one
 * thread went into a system call, and everything that thread does - windows,
 * the clock, the pointer - stopped until a server on the other side of the sea
 * answered. Most of that wait is not work, it is waiting.
 *
 * So the waiting happens in a worker, and the rule that keeps this from
 * becoming a mess is written here rather than remembered:
 *
 *   The worker owns `page`, `css`, the status line and `fetch_state`, and
 *   touches nothing else. It never calls mizu->anything: no repaint, no
 *   window, no yield. Not because it would be slow - because those belong to
 *   the desktop's thread and two owners is how this ends badly.
 *
 *   The desktop's thread owns everything on screen. It watches `fetch_state`
 *   from `tick`, and when the worker says it has finished, *then* it lays the
 *   page out and draws it - at which point nobody else is writing anything.
 *
 * One flag between two threads, written by one and read by the other, is the
 * whole of the shared state. That is not an accident; it is the smallest
 * amount of sharing the job can be done with.
 */

#define FETCH_IDLE 0
#define FETCH_RUNNING 1
#define FETCH_DONE 2

#define WORKER_STACK 65536

static volatile int fetch_state;
static char fetch_url[URL_MAX];
static void* worker_stack;
static int fetch_encoding;
static long fetch_result;

static void fetch_worker(void* argument) {
    int from_server = -1;

    (void)argument;

    /* Follow where a server says the page has moved, a few times.
     *
     * Five, because a site that bounces more than that is a site with a loop
     * in it, and stopping with a sentence is better than going round until
     * somebody notices. `http://name` answering "it is at https://name" is
     * the ordinary case now and the reason this exists at all. */
    for (int hop = 0; ; hop++) {
        copy_text(base, sizeof(base), fetch_url);
        fetch_result = http_get(fetch_url, page, PAGE_MAX, &from_server);
        if (fetch_result >= 0 || !moved_to[0]) break;
        if (hop >= 4) {
            report("That page moved more times than this will follow.");
            break;
        }
        {
            char next[URL_MAX];

            /* Resolved against where we just were, because a server is
               allowed to answer with "/elsewhere" rather than a whole
               address. `base` already says where that is - set at the top of
               this loop, once per hop. */
            resolve_link(moved_to, next);
            copy_text(fetch_url, sizeof(fetch_url), next);
            report("It moved. Following...");
        }
    }

    if (fetch_result >= 0) {
        page_length = fetch_result;
        /* Everything about *this* page, taken before the errands below fetch
         * anything else.
         *
         * `identity_checked` was already copied here for exactly this reason;
         * the rest was not, and a page with a picture on it therefore ended
         * up described by its last picture. A GIF is not HTML, so the page
         * was drawn as its own source code - which is what a neocities page
         * with six badges on it looked like, and only once the badges could
         * be fetched at all. The ones that failed left the flag alone and the
         * page rendered, which is why this survived every earlier test.
         */
        page_identity_checked = identity_checked;
        page_status = last_status;
        page_cut = page_truncated;
        /* What the server said, then what the document says, then UTF-8. */
        fetch_encoding = from_server;
        if (fetch_encoding < 0) fetch_encoding = encoding_declared();
        if (fetch_encoding < 0) fetch_encoding = ENCODING_UTF8;
        convert_to_utf8(fetch_encoding);
        page_is_html = content_is_html < 0 ? looks_like_html() : content_is_html;
        render_errands();
    } else {
        page_length = 0;
    }

    /* Written last, and it is what the other thread is watching: everything
       above must be finished before this says so. */
    fetch_state = FETCH_DONE;
    koi_thread_exit();
}

/* Called from the desktop's own thread, when the worker has put the flag
   down: the page becomes words on a screen here and nowhere else. */
static void finish_fetch(void) {
    /* Where the page came from in the end, which after a redirect is not
       where it was asked for. Copied here rather than in the worker: the
       address is what this thread draws. */
    if (fetch_result >= 0) copy_text(address, sizeof(address), fetch_url);

    if (fetch_result < 0) {
        piece_count = 0;
        line_count = 0;
        reset_layout();
    } else {
        if (!page_status || (page_status >= 200 && page_status < 300)) {
            char line[80];

            koi_snprintf(line, sizeof(line), "%s%ld bytes%s%s",
                         same_ignoring_case(address, "https://", 8)
                         ? (page_identity_checked
                            ? "private, identity checked. "
                            : "private, but nobody checked who answered. ")
                         : "",
                         page_length,
                         page_cut ? ", and more than fits - cut short" : "",
                         fetch_encoding == ENCODING_1251 ? ", windows-1251" :
                         fetch_encoding == ENCODING_KOI8 ? ", KOI8-R" :
                         fetch_encoding == ENCODING_866 ? ", code page 866" :
                         fetch_encoding == ENCODING_GREEK ? ", Greek" :
                         fetch_encoding == ENCODING_LATIN1 ? ", Latin-1" : "");
            report(line);
        }
        render_layout();
    }
    fetch_state = FETCH_IDLE;
    if (window) {
        int animated = 0;

        for (int at = 0; at < image_count; at++)
            if (images[at].frame_count > 1) animated = 1;
        /* Only a page that moves asks to be woken. A still one goes back to
           being drawn when something happens to it, which is most pages and
           all of the cost of this feature. */
        window->repaint_ms = animated ? 60 : (focused ? 500 : 0);
    }
    mizu->repaint();
}

static void fetch(const char* url) {
    if (looks_like_a_file(url)) { load_from_disk(url); return; }

    if (fetch_state == FETCH_RUNNING) {
        report("Still fetching the last one.");
        mizu->repaint();
        return;
    }

    if (same_ignoring_case(url, "koi:", 4)) {
        page_length = 0;
        for (const char* at = home_page; *at && page_length + 1 < PAGE_MAX; at++)
            page[page_length++] = *at;
        report("The page this browser starts on.");
        render();
        mizu->repaint();
        return;
    }

    /* Off to the worker. The stack is asked for once and kept: a thread's
       stack has to outlive the thread, and asking for one every time is a
       way to run out of memory slowly. */
    if (!worker_stack) worker_stack = koi_alloc(WORKER_STACK);
    if (!worker_stack) {
        report("Not enough memory to fetch anything.");
        mizu->repaint();
        return;
    }

    /* The old page's pictures go here, on the desktop's own thread, before
     * the worker starts.
     *
     * Not in the worker, where it was first written: the worker would be
     * freeing buffers while this thread was still blitting them onto the
     * screen - the exact shape of bug the ownership rule exists to prevent,
     * and one that would have shown up as a machine that dies on the second
     * page rather than the first.
     *
     * The cost is that the window goes blank while the next page loads
     * instead of holding the old one. Every browser did that until pages got
     * big enough for it to matter. */
    forget_images();
    piece_count = 0;
    line_count = 0;
    reset_layout();

    copy_text(fetch_url, sizeof(fetch_url), url);
    fetch_cancel = 0;
    fetch_result = 0;
    fetch_state = FETCH_RUNNING;
    report("Looking up the name...");
    /* The window asks to be woken while this runs: that is how the desktop
       gets to notice the worker has finished, and how the status line moves
       while it has not. */
    if (window) window->repaint_ms = 100;
    mizu->repaint();

    if (!koi_thread_start(fetch_worker, (char*)worker_stack + WORKER_STACK,
                          (void*)0)) {
        /* No thread to be had - so do it here rather than not at all, and
           the desktop stands still for it as it used to. */
        fetch_worker((void*)0);
        finish_fetch();
    }
}

static void go(const char* url, int remember) {
    if (remember) {
        if (back_count >= HISTORY_MAX) {
            /* The oldest one falls off the bottom rather than the newest
               failing to be remembered: a history that stops recording is a
               Back button that stops working. */
            for (int at = 1; at < HISTORY_MAX; at++)
                copy_text(back_stack[at - 1], URL_MAX, back_stack[at]);
            back_count = HISTORY_MAX - 1;
        }
        copy_text(back_stack[back_count++], URL_MAX, address);
        forward_count = 0;
    }
    copy_text(address, sizeof(address), url);
    focused = 0;
    if (window) window->repaint_ms = 0;
    fetch(address);
}

static void go_back(void) {
    if (!back_count) return;
    if (forward_count < HISTORY_MAX)
        copy_text(forward_stack[forward_count++], URL_MAX, address);
    copy_text(address, sizeof(address), back_stack[--back_count]);
    fetch(address);
}

static void go_forward(void) {
    if (!forward_count) return;
    if (back_count < HISTORY_MAX)
        copy_text(back_stack[back_count++], URL_MAX, address);
    copy_text(address, sizeof(address), forward_stack[--forward_count]);
    fetch(address);
}

/* ---- The address bar as a place to type ----------------------------------
 *
 * A modal box asked "where to?", which worked and was wrong: an address bar
 * that cannot be typed in is not an address bar, and a browser where the URL
 * you are looking at cannot be edited into the one next to it is a browser
 * that makes you retype the whole thing.
 */

static void focus_address(void) {
    copy_text(typed, sizeof(typed), address);
    caret = (int)strlen(typed);
    focused = 1;
    caret_on = 1;
    select_all = 1;
    if (window) window->repaint_ms = 500;
    report("Type an address, then Enter. Escape leaves it alone.");
    mizu->repaint();
}

static void unfocus_address(void) {
    focused = 0;
    if (window) window->repaint_ms = 0;
    mizu->repaint();
}

static void address_key(int pressed) {
    /* Ctrl+L again, with the bar already taken: select the whole address
       rather than being swallowed as a control character. Pressed twice in a
       row it used to do nothing the second time, so what was typed after it
       joined onto whatever was already there - which reads as the browser
       mangling the address. */
    if (pressed == 12) {
        select_all = 1;
        caret = (int)strlen(typed);
        caret_on = 1;
        mizu->repaint();
        return;
    }

    if (select_all) {
        select_all = 0;
        if ((pressed >= ' ' && pressed < 0x100) ||
            pressed == KOI_KEY_BACKSPACE || pressed == KOI_KEY_DELETE) {
            typed[0] = 0;
            caret = 0;
            if (pressed == KOI_KEY_BACKSPACE || pressed == KOI_KEY_DELETE) {
                caret_on = 1;
                mizu->repaint();
                return;
            }
        }
    }

    switch (pressed) {
    case KOI_KEY_ENTER:
    case '\r': {
        char target[URL_MAX];

        copy_text(target, sizeof(target), typed);
        unfocus_address();
        if (target[0]) go(target, 1);
        return;
    }
    case KOI_KEY_ESCAPE:
        report("Ready.");
        unfocus_address();
        return;
    case KOI_KEY_LEFT:
        while (caret > 0 && ((unsigned char)typed[--caret] & 0xC0) == 0x80) { }
        break;
    case KOI_KEY_RIGHT:
        if (typed[caret]) {
            caret++;
            while (typed[caret] && ((unsigned char)typed[caret] & 0xC0) == 0x80)
                caret++;
        }
        break;
    case KOI_KEY_HOME: caret = 0; break;
    case KOI_KEY_END: caret = (int)strlen(typed); break;
    case KOI_KEY_BACKSPACE: {
        int from = caret;

        while (from > 0 && ((unsigned char)typed[--from] & 0xC0) == 0x80) { }
        if (caret > from) {
            int length = (int)strlen(typed);

            for (int at = from; at <= length - (caret - from); at++)
                typed[at] = typed[at + (caret - from)];
            caret = from;
        }
        break;
    }
    case KOI_KEY_DELETE: {
        int length = (int)strlen(typed);

        if (caret < length) {
            int width = 1;

            while (caret + width < length &&
                   ((unsigned char)typed[caret + width] & 0xC0) == 0x80) width++;
            for (int at = caret; at <= length - width; at++)
                typed[at] = typed[at + width];
        }
        break;
    }
    default:
        if (pressed >= ' ' && pressed < 0x100) {
            int length = (int)strlen(typed);

            if (length + 2 < (int)sizeof(typed)) {
                for (int at = length + 1; at > caret; at--)
                    typed[at] = typed[at - 1];
                typed[caret++] = (char)pressed;
            }
        }
        break;
    }
    caret_on = 1;
    mizu->repaint();
}

static void click(WINDOW* self, int x, int y, int clicks) {
    int client_x, client_y, client_w, client_h;

    (void)clicks;
    mizu->window_client(self, &client_x, &client_y, &client_w, &client_h);

    if (y < BAR_HEIGHT) {
        int field_x = 4 + BUTTONS * (BUTTON_WIDTH + 2) + 4;

        for (int index = 0; index < BUTTONS; index++) {
            int at_x = 4 + index * (BUTTON_WIDTH + 2);

            if (x < at_x || x >= at_x + BUTTON_WIDTH) continue;
            unfocus_address();
            switch (index) {
            case 0: go_back(); break;
            case 1: go_forward(); break;
            case 2: fetch(address); break;
            case 3: go("koi:home", 1); break;
            default: break;
            }
            mizu->repaint();
            return;
        }
        if (x >= field_x) {
            /* Clicking into the text puts the caret where the pointer is,
               which is what every other text field on the machine does. */
            if (!focused) focus_address();
            select_all = 0;
            {
                int cell = (x - field_x - 3) / WINDOW_CHAR_W + field_left;

                if (cell < 0) cell = 0;
                caret = bytes_for_cells(typed, cell);
            }
            mizu->repaint();
        }
        return;
    }

    if (focused) unfocus_address();

    if (x >= client_w - WINDOW_SCROLLBAR_W && y >= PAGE_TOP) {
        top_line = mizu->scrollbar_press(PAGE_TOP, shown_rows * WINDOW_CHAR_H,
                                         top_line, shown_rows, line_count, y);
        mizu->repaint();
        return;
    }

    for (int index = 0; index < hotspot_count; index++) {
        HOTSPOT* spot = &hotspots[index];

        if (x + client_x < spot->x || x + client_x >= spot->x + spot->width)
            continue;
        if (y + client_y < spot->y ||
            y + client_y >= spot->y + WINDOW_CHAR_H) continue;
        {
            char target[URL_MAX];

            resolve_link(link_text(spot->link), target);
            go(target, 1);
        }
        return;
    }
}

static void drag(WINDOW* self, int x, int y) {
    int client_x, client_y, client_w, client_h;

    mizu->window_client(self, &client_x, &client_y, &client_w, &client_h);
    if (x < client_w - WINDOW_SCROLLBAR_W - 8) return;
    top_line = mizu->scrollbar_drag(PAGE_TOP, shown_rows * WINDOW_CHAR_H,
                                    shown_rows, line_count, y);
    mizu->repaint();
}

static void key(WINDOW* self, int pressed) {
    (void)self;

    if (focused) { address_key(pressed); return; }

    switch (pressed) {
    case KOI_KEY_DOWN:
        if (top_line + shown_rows < line_count) top_line++;
        break;
    case KOI_KEY_UP: if (top_line) top_line--; break;
    case KOI_KEY_PAGE_DOWN:
        if (top_line + shown_rows < line_count) top_line += shown_rows;
        if (top_line + shown_rows > line_count) top_line = line_count - shown_rows;
        if (top_line < 0) top_line = 0;
        break;
    case KOI_KEY_PAGE_UP:
        top_line -= shown_rows;
        if (top_line < 0) top_line = 0;
        break;
    case KOI_KEY_HOME: top_line = 0; break;
    case KOI_KEY_END:
        top_line = line_count - shown_rows;
        if (top_line < 0) top_line = 0;
        break;
    case KOI_KEY_BACKSPACE: go_back(); return;
    case KOI_KEY_F1 + 4: fetch(address); return;        /* F5 */
    case 12: focus_address(); return;                   /* Ctrl+L */
    case KOI_KEY_ESCAPE: return;
    default:
        /* Anything printable starts typing an address, the way it does in a
           list of files: the alternative is a key that does nothing. */
        if (pressed >= ' ' && pressed < 0x100) {
            focus_address();
            typed[0] = 0;
            caret = 0;
            address_key(pressed);
            return;
        }
        return;
    }
    mizu->repaint();
}

/* The next frame of every picture that has one, when its time has come.
 *
 * Decoding happens here, on the thread that owns the screen, because a GIF
 * frame is a change to the canvas the last frame left - and that canvas is
 * what the paint below blits. Two threads writing one canvas would be the
 * same mistake as before, in prettier clothes.
 *
 * The work is small: a badge is eighty-eight pixels wide and a frame of it
 * decodes in well under a millisecond. Returns whether anything moved, so
 * that a page of still pictures costs one comparison per tick. */
static int advance_animations(void) {
    static koi_uint8* scratch;
    static long scratch_size;
    koi_uint64 now = koi_uptime();
    int moved = 0;

    for (int at = 0; at < image_count; at++) {
        IMAGE* picture = &images[at];
        GIF gif;
        long needed;

        if (picture->frame_count < 2 || !picture->file || !picture->pixels)
            continue;
        if (picture->due && now < picture->due) continue;

        needed = (long)picture->width * picture->height + 64;
        if (needed > scratch_size) {
            if (scratch) koi_free(scratch);
            scratch = (koi_uint8*)koi_alloc(needed);
            scratch_size = scratch ? needed : 0;
            if (!scratch) { picture->frame_count = 1; continue; }
        }
        if (!gif_open(picture->file, picture->file_length, &gif)) {
            picture->frame_count = 1;
            continue;
        }
        picture->frame = (picture->frame + 1) % gif.frame_count;
        if (!gif_frame(&gif, picture->frame, picture->pixels, 0xFFFFFF,
                       scratch, scratch_size)) {
            picture->frame_count = 1;
            continue;
        }
        {
            int delay = gif.frames[picture->frame].delay_ms;

            /* A frame that asks for nothing gets a tenth of a second, which
               is what every browser settled on: a zero delay in a file means
               "as fast as you can", and as fast as this can is not a thing
               anybody wants to watch. */
            if (delay < 20) delay = 100;
            picture->delay_ms = delay;
            picture->due = now + (koi_uint64)delay;
        }
        moved = 1;
    }
    return moved;
}

static void tick(WINDOW* self) {
    (void)self;

    /* The one place the two threads meet: the worker has put its flag down
       and this is the desktop's own thread, so laying the page out here is
       safe in the only sense that matters - nobody else is writing it. */
    if (fetch_state == FETCH_DONE) { finish_fetch(); return; }
    if (fetch_state == FETCH_RUNNING) { mizu->repaint(); return; }

    if (advance_animations()) { mizu->repaint(); return; }

    if (!focused) return;
    caret_on = !caret_on;
}

static void menu(WINDOW* self, int id) {
    (void)self;
    switch (id) {
    case NAMI_GO: focus_address(); break;
    case NAMI_BACK: go_back(); break;
    case NAMI_FORWARD: go_forward(); break;
    case NAMI_HOME: go("koi:home", 1); break;
    case NAMI_RELOAD: fetch(address); break;
    case NAMI_STYLES: {
        char line[96];

        koi_snprintf(line, sizeof(line),
                     "%d rules, %d appearances, %d words, %d links.",
                     rule_count, style_count, piece_count, link_count);
        mizu->message("Nami Explorer", line, mizu->say(DIALOG_OK));
        break;
    }
    case NAMI_CLOSE:
        mizu->window_delete(window);
        window = (WINDOW*)0;
        break;
    default: break;
    }
    mizu->repaint();
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
    window = mizu->window_new("Nami Explorer", 70, 50, 660, 470);
    if (!window) return (WINDOW*)0;
    window->paint = paint;
    window->click = click;
    window->key = key;
    window->drag = drag;
    window->tick = tick;
    window->repaint_ms = 0;
    window->menu_count = 1;
    window->menus[0] = (WINDOW_MENU){ "Go",
        { { "Address...", NAMI_GO }, { "Back", NAMI_BACK },
          { "Forward", NAMI_FORWARD }, { "Home", NAMI_HOME },
          { "Reload", NAMI_RELOAD }, { 0, 0 },
          { "This page", NAMI_STYLES }, { 0, 0 },
          { "Close", NAMI_CLOSE } }, 9 };
    copy_text(address, sizeof(address), "koi:home");
    copy_text(base, sizeof(base), "koi:home");
    fetch(address);
    return window;
}

/* A page already on disk, which is how a browser with no TLS still has
   something to show - and what the address bar does with a path. */
static void load_from_disk(const char* path) {
    long handle = koi_open(path, OPEN_READ);
    int encoding;

    if (handle < 0) {
        char line[128];

        koi_snprintf(line, sizeof(line), "There is no file called %s.", path);
        report(line);
        page_length = 0;
        piece_count = 0;
        line_count = 0;
        reset_layout();
        mizu->repaint();
        return;
    }
    page_length = koi_read(handle, page, PAGE_MAX - 1);
    koi_close(handle);
    if (page_length < 0) page_length = 0;

    /* A file on this disk is as likely to be code page 866 as anything: it
       may have been written by a program on this machine. */
    content_is_html = -1;
    page_is_html = -1;
    page_status = 0;
    page_cut = 0;
    encoding = encoding_declared();
    if (encoding < 0) encoding = ENCODING_UTF8;
    convert_to_utf8(encoding);

    copy_text(address, sizeof(address), path);
    copy_text(base, sizeof(base), path);
    report("From this disk.");
    render();
    mizu->repaint();
}

static WINDOW* open_with(const char* path) {
    WINDOW* opened = open();

    if (!opened || !path) return opened;
    back_count = 0;
    forward_count = 0;
    load_from_disk(path);
    return opened;
}

/* 2, not 1: this structure has `open_with` in it, and a desktop asked to open
   a file checks that number before it looks for the function. Declared as 1
   with the function present, the browser was refused every .HTM the file
   manager offered it - silently, because "this application is too old to open
   a file" is not something worth a dialogue box. */
static MIZU_APP me = { "Nami Explorer", 2, open, menu, closing, open_with };

MIZU_APPLICATION(start)

static MIZU_APP* start(const MIZU_API* api) {
    /* 8 brought the scrollbar, which this draws down the side of every page.
       An older desktop has no such call in its table and there is nothing
       sensible to do about it here but say so. */
    if (!api || api->version < 8) return (MIZU_APP*)0;
    mizu = api;
    return &me;
}
