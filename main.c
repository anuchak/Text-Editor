/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define TAB_STOP 8

#include <string.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/*** defines ***/

#define VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)  // sets the upper 3 bits to 0, Crtl key strips bits 5
				  // and 6 from the pressed key, and sends that

#define ABUF_INIT {NULL, 0}       // an empty buffer definition

enum editorKey {
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};


// usually terminal is in canonial/cooked mode, where it lets user
// to properly set its input, and then press ENTER for passing the args
// Canonical mode reads input line by line, we want to read input byte by byte
// But in this case, we want the program to receive each keystroke as it is.
// So we set the raw mode using the struct termios and the tcgetattr() function


/*** data ***/

typedef struct erow {      // editor row, stores a line of text as a pointer to dynamically allocated character data and a length
       int size;
       char *chars;
       int rsize;         // size of render contents
       char *render;      // contain actual characters to draw on screen for that row
} erow;

struct editorConfig {
        struct termios orig_termios; // original state of terminal, to be restored later
        int screenrows;              // total rows of the screen
        int rowoff;                  // row offset, to keep track of the row of the file the user is scrolled to
        int coloff;                  // column offset
        int screencols;              // total columns of the screen
        int cx, cy;                  // co-ordinates of cursor at any point
        int rx;                      // render x-coordinate
        int numrows;                 //  no. of rows in the file being read
        erow *row;                   // editorRow(erow) type to store each row
        char *filename;
        char statusmsg[80];
        time_t statusmsg_time;
};

struct editorConfig E;

struct abuf {      // append buffer
    char *b;       // pointer to our buffer in memory
    int len;       // length of buffer
};

/*** terminal ***/


void die(const char *s) {

        write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3);
        perror(s);
	// looks at the global errno no. and prints an error mssg
	exit(1);
	// exit status 1, indicating failure
}


void disableRawMode()
{
   tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
   die("tcsetattr");
}

void enableRawMode()
{
   if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) 
	   die("tcgetattr");
   atexit(disableRawMode);

   // echo feature causes each key typed, to be printed to the screen
   // Useful in canonical mode
   // Turning off ECHO, just doesn't display what we are typing
   
   struct termios raw = E.orig_termios;
   raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
   // input flags
   // BRKINT turned on, break condition will cause SIGINT to be sent
   
   raw.c_lflag &= ~(ECHO | ICANON | IEXTEN  | ISIG);   
   // local flags

   raw.c_cflag |= (CS8);
   // sets character size to 8 bits per byte
   
   raw.c_oflag &= ~(OPOST);
   // output flag

   raw.c_cc[VMIN] = 0;
   raw.c_cc[VTIME] = 1;
   // c_cc -> control characters
   // VMIN -> min no. of bytes of input for read() to return, set to 0, so that read()    // returns as soon as there is any input to be read
   // VTIME -> sets the max amt. of time to wait before read() returns

   // ICANON flag ,used to turn off canonical mode
   // ISIG flag ,used to turn off Ctrl-C & Ctrl-Z flags
   // IXON flag ,used to turn off Ctrl-S & Ctrl-Q flags
   // IEXTEN flag ,used to turn off Ctrl-V flag
   // ICRNL -> input flag, CR stands for carriage return, NL is newline
   // Carriage return moves the cursor to the beginning of the current line
   // Newline moves the cursor down a line, scrolling the screen, if reqd.
   // O -> output flag, POST -> post processing of output
   
   if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
	 die("tcsetattr");  
}

int editorReadKey() {
  int nread;
  char c;
  while((nread = read(STDIN_FILENO, &c, 1)) != 1)
  {
     if(nread == -1 && errno != EAGAIN) die("read");
  }

  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if(seq[2] == '~'){
         switch(seq[1])
         {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
         }
       }
    } else {
    switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
   } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

// this func waits for 1 keypress, returns it, deals with low level terminal input

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

   if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

   while ( i < sizeof(buf) - 1) {
      if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
      if(buf[i] == 'R') break;
      i++;
   }
   buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

   return 0;
}


int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
      if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
      // C command- moves the cursor to the right, 999 is argument
      // B command- moves the cursor to the bottom, 999 is argument
      // to ensure it reaches the right bottom corner

      return getCursorPosition(rows, cols);
    } else {
      *cols = ws.ws_col;
      *rows = ws.ws_row;
       return 0;
    }
}

/**** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (TAB_STOP - 1) - (rx % TAB_STOP);
    rx++;
  }
  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for(j = 0; j < row->size; j++)
     if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(TAB_STOP-1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));

  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);
  E.numrows++;
}

/*** file i/o ***/

void editorOpen(char *filename) {
      free(E.filename);
      E.filename = strdup(filename);

      FILE *fp = fopen(filename, "r");
      if (!fp) die("fopen");

      char *line = NULL;
      size_t linecap = 0;
      ssize_t linelen;
      while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' ||
                                line[linelen - 1]=='\r'))
            linelen--;
        editorAppendRow(line, linelen);
      }
      free(line);
      fclose(fp);
}

/*** append buffer ***/

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
     free(ab -> b);
}

/*** output ***/

void editorScroll() {
   E.rx = 0;
   if (E.cy < E.numrows) {
      E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

   if (E.cy < E.rowoff) {

   // checks whether the cursor is above the visible window, then scrolls to it
   // E.rowoff refers to what's at the top of the screen
   // E.screenrows refers to what's at the bottom of the screen
   // if cursor position is above the visible window(reference here is the top of the screen
   // which is the offset. IF, E.cy goes below the (E.row-offset + size of visible screen
   // the row-offset adjusted to the new top of the screen which is
   // (Cursor_position - size_of_screen(rows)) + 1.

    E.rowoff = E.cy;
   }
   if (E.cy >= E.rowoff + E.screenrows) {
     E.rowoff = E.cy - E.screenrows + 1;
   }

   if(E.rx < E.coloff) {
      E.coloff = E.rx;
   }
   if(E.rx >= E.coloff + E.screencols) {
     E.coloff = E.rx - E.screencols + 1;
   }
}

void editorDrawRows(struct abuf *ab) {
  int y;
  for(y = 0; y < E.screenrows; y++) {
   int filerow = y + E.rowoff;
   if (filerow >= E.numrows) {
     if (E.numrows ==  0 && y == E.screenrows / 3) {
       char welcome[80];
       int welcome_len = snprintf(welcome, sizeof(welcome), "Text editor -- version %s", KILO_VERSION);

       if(welcome_len > E.screencols) welcome_len = E.screencols;
       int padding = (E.screencols - welcome_len) / 2;
       if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) abAppend(ab, " ", 1);
      abAppend(ab, welcome, welcome_len);
    } else {
    abAppend(ab,"~", 1);
    }
   } else {
     int len = E.row[filerow].rsize - E.coloff;
     if (len < 0) len = 0;
     if (len > E.screencols) len = E.screencols;
     abAppend(ab, &E.row[filerow].render[E.coloff], len);
   }

    abAppend(ab, "\x1b[K", 3); // K command -> erase in line, default arg is 0, which means erase the part of line to the right of the cursor
                               // 2 is the arg, then erase whole; 1 is the arg, then erase the part of the line to the left of the cursor
    abAppend(ab,"\r\n",2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80],rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No Name]", E.numrows);
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d %d",E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
       abAppend(ab, rstatus, rlen);
       break;
    } else {
    abAppend(ab, " ", 1);
    len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // ?25l used to hide the cursor

    abAppend(&ab, "\x1b[H", 3); // H command- Shifts cursor pos to specified arg

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy-E.rowoff+1, E.rx-E.coloff+1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // ?25h used to show the cursor
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// '4' used, to indicate we write 4 bytes out to the terminal
// 1st byte is \x1b = 27, the escape character. The whole 4 bytes is a
// escape sequence, which always start with an escape character(27) and [
// J command- Erase in Display to clear the screen, 2 is arg, to clear the whole screen// VT100 escape sequence, ncurses library also there. 

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

// The ... argument makes editorSetStatusMessage() a variadic function,
// meaning it can take any number of arguments. C’s way of dealing with these
// arguments is by calling va_start() and va_end() on a value of type
// va_list. The last argument before the ... (in this case, fmt) must be passed
// to va_start(), so that the address of the next arguments is known. Then,
// between the va_start() and va_end() calls, you would call va_arg() and
// pass it the type of the next argument (which you usually get from the given
// format string) and it would return the value of that argument. In this case, we
// pass fmt and ap to vsnprintf() and it takes care of reading the format
// string and calling va_arg() to get each argument.


/*** input ***/

void editorMoveCursor(int key) {
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
     case ARROW_LEFT:
        if(E.cx != 0)
        E.cx--;
        else if (E.cy > 0){
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
     case ARROW_RIGHT:
        if(row && E.cx < row->size)
        E.cx++;
        else if (row && E.cx == row->size){
         E.cy++;
         E.cx = 0;
        }
        break;
     case ARROW_UP:
        if(E.cy != 0)
        E.cy--;
        break;
     case ARROW_DOWN:
        if(E.cy < E.numrows)
        E.cy++;
        break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
      E.cx = rowlen;
    }
}

void editorProcessKeypress() {
     int c = editorReadKey();
     int num;
     switch(c) {
       case CTRL_KEY('q'):
               write(STDOUT_FILENO, "\x1b[2J", 4);
               write(STDOUT_FILENO, "\x1b[H", 3);
               exit(0);
	       break;
       case HOME_KEY:
           E.cx = 0;
           break;
       case END_KEY:
           if (E.cy < E.numrows)
              E.cx = E.row[E.cy].size;
           break;

       case PAGE_UP:
       case PAGE_DOWN:
               if (c == PAGE_UP) {
                    E.cy = E.rowoff;
               } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
               }
               num = E.screenrows;
               while(num--){
                  if(c == PAGE_UP)
                    editorMoveCursor(ARROW_UP);
                  else
                    editorMoveCursor(ARROW_DOWN);
               }
               break;
       case ARROW_UP:
       case ARROW_DOWN:
       case ARROW_LEFT:
       case ARROW_RIGHT:
              editorMoveCursor(c);
              break;
     }
}

// This func waits for a keypress, handles it. Deals with mapping keys to higher
// functions at a higher level



/*** init ***/

void initEditor() {
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.rx = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 1;
}

int main(int argc, char **argv)
{
   enableRawMode();
   initEditor();
   if (argc >= 2) {
       editorOpen(argv[1]);
   }

   editorSetStatusMessage("HELP: Crtl -Q = quit");

   while(1) {
	   editorRefreshScreen();
	   editorProcessKeypress();
   }

   return 0;
}
