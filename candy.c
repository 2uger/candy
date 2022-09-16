#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/ioctl.h>

#define CTRLKEY(k) ((k) & 0x1f)

typedef enum Mode {
    INSERT,
    VIEW
} Mode;

typedef struct erow {
    int size;
    char *chars;
} erow;

struct EditorConfig {
    Mode mode;
    int cx, cy;  // cursor position
    int rowoff;  // row offset
    int coloff;  // column offset
    int screen_rows;
    int screen_cols;
    int numrows;  // total amount of rows in current buffer
    char *filename;
    char status_msg[80];
    time_t status_msg_time;
    erow *row;
    struct termios orig_termios;
};

struct EditorConfig config;

void editor_set_status_message(const char *fmt, ...);

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

/*** row operations ***/

void
editor_append_row(char *s, size_t len)
{
    config.row = realloc(config.row, sizeof(erow) * (config.numrows + 1));

    struct erow *r = &config.row[config.numrows];
    r->size = len;
    r->chars = malloc(len + 1);
    memcpy(r->chars, s, len);

    r->chars[len] = '\0';

    config.numrows++;
}

void
editor_row_insert_char(erow *row, int at, int c)
{
    if (at < 0 || at > row->size)
        at = row->size;
    row->chars = realloc(row->chars, row->size + 2);  // 2 = new char and \0
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
}

/*** editor operations ***/
void
editor_insert_char(int c)
{
    // TODO: it shouldnt be possible for vim like editor
    if (config.cy == config.numrows)
        editor_append_row("", 0);

    editor_row_insert_char(&config.row[config.cy], config.cx, c);
    config.cx++;
}

/*** file i/o ***/

char*
editor_rows_to_string(int *len)
{
    int total = 0;
    for (int i = 0; i < config.numrows; i++) {
        total += config.row[i].size + 1;
    }
    *len = total;

    char *buf = malloc(total);
    char *p = buf;
    for (int i = 0; i < config.numrows; i++) {
        memcpy(p, config.row[i].chars, config.row[i].size);
        p += config.row[i].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void
editor_open(char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        die("fopen");

    config.filename = strdup(filename);

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

void
editor_save()
{
    if (config.filename == NULL)
        return;

    int len;
    char *buf = editor_rows_to_string(&len);

    int fd = open(config.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                editor_set_status_message("Save file: %d bytes written to disk", len);
                return;
            }
        }
        close(fd);

    }
    free(buf);
    editor_set_status_message("Can't save!");
}

/*** input ***/

#define MV_UP    'k'
#define MV_DOWN  'j'
#define MV_LEFT  'h'
#define MV_RIGHT 'l'

void
editor_move_cursor(char key)
{
    erow *row = (config.cy >= config.numrows) ? NULL : &config.row[config.cy];

    switch (key) {
        case MV_DOWN:
            config.cy = (config.cy < config.numrows - 1 ? config.cy + 1 : config.cy);
            break;
        case MV_UP:
            config.cy = (config.cy != 0 ? config.cy - 1 : config.cy);
            break;
        case MV_LEFT:
            config.cx = (config.cx > 0 ? config.cx - 1 : config.cx);
            break;
        case MV_RIGHT:
            if (row && config.cx <= row->size - 2)
                config.cx++;
            break;
        case CTRLKEY('d'):
            config.cy = (config.cy < config.numrows - 1 - 10 ? config.cy + 10 : config.numrows - 1);
            break;
        case CTRLKEY('u'):
            config.cy = (config.cy > 10 ? config.cy - 10 : 0);
            break;
        case '$':
            config.cx = row->size - 1; 
            return;
        case '0':
            config.cx = 0;
            return;
    }

    row = (config.cy >= config.numrows) ? NULL : &config.row[config.cy];
    int rowlen = row ? row->size : 0;
    if (config.cx > rowlen)
        config.cx = rowlen;
}

void
editor_process_keypress()
{
    char c = editor_read_key();
    if (config.mode == VIEW) {
        switch (c) {
            case CTRLKEY('q'):
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                exit(0);
                break;
            case '\r':
                break;
            case '$':
            case '0':
            case CTRLKEY('d'):
            case CTRLKEY('u'):
            case MV_DOWN:
            case MV_UP:
            case MV_LEFT:
            case MV_RIGHT:
                editor_move_cursor(c);
                break;
            case 'i':
                config.mode = INSERT;
                break;
            case 'Z':
                editor_save();
                break;
        }
    } else if (config.mode == INSERT) {
        switch (c) {
            // TODO: for debug purposes
            case CTRLKEY('q'):
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                exit(0);
                break;
            case '\x1b':
            case CTRLKEY('c'):
                config.mode = VIEW;
                break;
            case '\r':
            case 127:
                break;
            default:
                editor_insert_char(c);
                break;
        }
    }
}

/*** output ***/

void
editor_draw_status_bar(struct abuf *ab)
{
    ab_append(ab, "\x1b[7m", 4);

    char buf[80];
    int len = snprintf(buf, sizeof(buf),
                       "%.20s - %d lines, mode: %s\x1b[m\x1b[7m",
                       config.filename ? config.filename : "No name",
                       config.numrows,
                       config.mode == VIEW ? "\x1b[32mVIEW" : "\x1b[31mINSERT");
    len = len > config.screen_rows ? config.screen_rows : len;
    ab_append(ab, buf, len);
    while (len < config.screen_cols) {
        ab_append(ab, " ", 1);
        len++;
    }
    ab_append(ab, "\x1b[m", 4);
}

void
editor_draw_message_bar(struct abuf *ab)
{
    int msglen = strlen(config.status_msg);
    msglen = msglen > config.screen_cols ? config.screen_cols : msglen;
    if (msglen && time(NULL) - config.status_msg_time < 3) {
        ab_append(ab, config.status_msg, msglen);
        return;
    }
    int l = 0;
    while (l < config.screen_cols) {
        ab_append(ab, " ", 1);
        l++;
    }
}

void
editor_scroll()
{
    if (config.cy < config.rowoff)
        config.rowoff = config.cy;

    if (config.cy >= config.rowoff + config.screen_rows)
        config.rowoff = config.cy - config.screen_rows + 1;

    if (config.cx < config.coloff)
        config.coloff = config.cx;
        
    if (config.cx >= config.coloff + config.screen_cols)
        config.coloff = config.cx - config.screen_cols + 1;
}

void
editor_draw_rows(struct abuf *ab)
{
    for (int y = 0; y < config.screen_rows; y++) {
        int filerow = y + config.rowoff;
        if (y >= config.numrows) {
            ab_append(ab, "~", 1);
        } else {
            int len = config.row[filerow].size - config.coloff;
            len = (len < 0) ? 0 : len;

            ab_append(ab, &config.row[filerow].chars[config.coloff], len);
        }
        ab_append(ab, "\x1b[K", 3);  // clear current line

        ab_append(ab, "\r\n", 2);
    }
}

void
editor_refresh_screen()
{
    editor_scroll();

    struct abuf ab = ABUF_INIT;

    ab_append(&ab, "\x1b[?25l", 6); // hide cursor
    ab_append(&ab, "\x1b[H", 3);    // change position of cursor to 0,0

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (config.cy - config.rowoff) + 1,
                                              (config.cx - config.coloff) + 1);  // move cursor to certain position
    ab_append(&ab, buf, strlen(buf));

    ab_append(&ab, "\x1b[?25h", 6);  // show cursor (to avoid flickering when redraw)

    write(STDOUT_FILENO, ab.b, ab.len);
    ab_free(&ab);
}

void
editor_set_status_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(config.status_msg, sizeof(config.status_msg), fmt, ap);
    va_end(ap);
    config.status_msg_time = time(NULL);
}

/*** init ***/

void
init_editor()
{
    config.mode = VIEW;
    config.cx = 0;
    config.cy = 0;
    config.rowoff = 0;
    config.coloff = 0;
    config.numrows = 0;
    config.row = NULL;
    config.filename = NULL;
    config.status_msg[0] = '\0';
    config.status_msg_time = time(NULL);

    if (get_window_size(&config.screen_rows, &config.screen_cols) == -1)
        die("get_window_size");
    config.screen_rows -= 2;
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
