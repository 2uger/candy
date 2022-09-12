#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>

#define CTRLKEY(k) ((k) & 0x1f)

typedef struct erow {
    int size;
    char *chars;
} erow;

struct EditorConfig {
    int cx, cy;
    int screen_rows;
    int screen_cols;
    int numrows;
    erow *row;
    struct termios orig_termios;
};

struct EditorConfig config;

/*** terminal ***/

void
die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void
disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.orig_termios) == -1)
        die("tcsetattr");
}

void
enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &config.orig_termios) == -1)
        die("tcgetattr");

    atexit(disable_raw_mode);

    struct termios raw = config.orig_termios;

    // man termios (for flag meanings)
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP | ICRNL | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

char
editor_read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }
    return c;
}

int
get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    return 0;
}

int
get_window_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    
}

/*** Append buffer ***/

#define ABUF_INIT {NULL, 0}

// Append buffer
struct abuf {
    char *b;
    int len;
};

void
ab_append(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL)
        return;
    
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void
ab_free(struct  abuf *ab)
{
    free(ab->b);
}

/*** input ***/

#define MV_UP    'k'
#define MV_DOWN  'j'
#define MV_LEFT  'h'
#define MV_RIGHT 'l'

void
editor_move_cursor(char key)
{
    switch (key) {
        case MV_DOWN:
            config.cy = (config.cy != config.screen_rows - 1 ? config.cy + 1 : config.cy);
            break;
        case MV_UP:
            config.cy = (config.cy != 0 ? config.cy - 1 : config.cy);
            break;
        case MV_LEFT:
            config.cx = (config.cx != 0 ? config.cx - 1 : config.cx);
            break;
        case MV_RIGHT:
            config.cx = (config.cx != config.screen_cols - 1 ? config.cx + 1 : config.cx);
            break;
    }

}

void
editor_process_keypress()
{
    char c = editor_read_key();
    switch (c) {
        case CTRLKEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case MV_DOWN:
        case MV_UP:
        case MV_LEFT:
        case MV_RIGHT:
            editor_move_cursor(c);
            break;
    }
}

/*** output ***/

void
editor_draw_rows(struct abuf *ab)
{
    for (int y = 0; y < config.screen_rows; y++) {
        if (y >= config.numrows) {
            ab_append(ab, "~", 1);
        } else {
            int len = config.row[y].size;
            if (len > config.screen_cols)
                len = config.screen_cols;
            ab_append(ab, config.row[y].chars, len);
        }
        ab_append(ab, "\x1b[K", 3);  // clear current line

        if (y < config.screen_rows - 1) {
            ab_append(ab, "\r\n", 2);
        }
    }
}

void
editor_refresh_screen()
{
    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6); // hide cursor
    ab_append(&ab, "\x1b[H", 3);    // change position of cursor to 0,0

    editor_draw_rows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", config.cy + 1, config.cx + 1);  // move cursor to certain position
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);  // show cursor (to avoid flickering when redraw)

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

/*** row operations ***/

void
editor_append_row(char *s, size_t len)
{
    config.row = realloc(config.row, sizeof(erow) + (config.numrows + 1));

    struct erow *r = &config.row[config.numrows];
    r->size = len;
    r->chars = malloc(len + 1);
    memcpy(r->chars, s, len);

    r->chars[len] = '\0';

    config.numrows++;
}

/*** file i/o ***/

void
editor_open(char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
}

/*** init ***/

void
init_editor()
{
    config.cx = 0;
    config.cy = 0;
    config.numrows = 0;
    config.row = NULL;

    if (get_window_size(&config.screen_rows, &config.screen_cols) == -1)
        die("get_window_size");
}

int
main(int argc, char *argv[])
{
    enable_raw_mode();
    init_editor();
    if (argc >= 2)
        editor_open(argv[1]);

    char c;
    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    };
    return 0;
}
