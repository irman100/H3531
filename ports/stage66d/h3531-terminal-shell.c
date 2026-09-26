#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static const char *LOG_PATH = "/var/h3531-terminal-input.log";

static void log_termios(const char *tag, int fd, const struct termios *t)
{
   FILE *f = fopen(LOG_PATH, "a");
   if (!f) return;
   fprintf(f,
      "[TERM] %s fd=%d isatty=%d iflag=0x%lx oflag=0x%lx cflag=0x%lx lflag=0x%lx erase=0x%02x eof=0x%02x\n",
      tag, fd, isatty(fd),
      (unsigned long)t->c_iflag,
      (unsigned long)t->c_oflag,
      (unsigned long)t->c_cflag,
      (unsigned long)t->c_lflag,
      (unsigned)t->c_cc[VERASE],
      (unsigned)t->c_cc[VEOF]);
   fclose(f);
}

static int fix_terminal_fd(int fd)
{
   struct termios t;

   if (!isatty(fd))
      return 0;

   if (tcgetattr(fd, &t) != 0)
   {
      FILE *f = fopen(LOG_PATH, "a");
      if (f) {
         fprintf(f, "[TERM] tcgetattr failed fd=%d errno=%d (%s)\n",
               fd, errno, strerror(errno));
         fclose(f);
      }
      return -1;
   }

   log_termios("before", fd, &t);

   /* Canonical interactive terminal semantics for VTE -> BusyBox ash.
    * In particular:
    * - Enter is CR from VTE and ICRNL turns it into newline;
    * - Backspace is DEL (0x7f) and VERASE consumes it;
    * - echo/editing/signals are handled by the PTY line discipline. */
   t.c_iflag |= BRKINT | ICRNL | IXON;
   t.c_iflag &= ~(IGNBRK | PARMRK | INPCK | ISTRIP | INLCR | IGNCR);

   t.c_oflag |= OPOST | ONLCR;

   t.c_cflag |= CREAD | CS8;
   t.c_cflag &= ~(PARENB | CSTOPB);

   t.c_lflag |= ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
#ifdef ECHOCTL
   t.c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
   t.c_lflag |= ECHOKE;
#endif
#ifdef ECHOPRT
   t.c_lflag &= ~ECHOPRT;
#endif

   t.c_cc[VINTR]  = 3;     /* Ctrl-C */
   t.c_cc[VQUIT]  = 28;    /* Ctrl-\\ */
   t.c_cc[VERASE] = 127;   /* VTE Backspace: DEL */
   t.c_cc[VKILL]  = 21;    /* Ctrl-U */
   t.c_cc[VEOF]   = 4;     /* Ctrl-D */
   t.c_cc[VSTART] = 17;    /* Ctrl-Q */
   t.c_cc[VSTOP]  = 19;    /* Ctrl-S */
#ifdef VSUSP
   t.c_cc[VSUSP]  = 26;    /* Ctrl-Z */
#endif
#ifdef VEOL
   t.c_cc[VEOL]   = 0;
#endif
#ifdef VMIN
   t.c_cc[VMIN]   = 1;
#endif
#ifdef VTIME
   t.c_cc[VTIME]  = 0;
#endif

   if (tcsetattr(fd, TCSANOW, &t) != 0)
   {
      FILE *f = fopen(LOG_PATH, "a");
      if (f) {
         fprintf(f, "[TERM] tcsetattr failed fd=%d errno=%d (%s)\n",
               fd, errno, strerror(errno));
         fclose(f);
      }
      return -1;
   }

   if (tcgetattr(fd, &t) == 0)
      log_termios("after", fd, &t);

   return 0;
}

static int selftest(void)
{
   int master = -1, slave = -1;
   char *name;
   struct termios t;

   master = posix_openpt(O_RDWR | O_NOCTTY);
   if (master < 0) {
      perror("posix_openpt");
      return 10;
   }
   if (grantpt(master) != 0 || unlockpt(master) != 0) {
      perror("grantpt/unlockpt");
      close(master);
      return 11;
   }
   name = ptsname(master);
   if (!name) {
      perror("ptsname");
      close(master);
      return 12;
   }
   slave = open(name, O_RDWR | O_NOCTTY);
   if (slave < 0) {
      perror("open slave");
      close(master);
      return 13;
   }

   if (tcgetattr(slave, &t) != 0) {
      perror("tcgetattr");
      close(slave);
      close(master);
      return 14;
   }

   /* Deliberately reproduce the broken symptom class. */
   t.c_iflag &= ~ICRNL;
   t.c_lflag &= ~(ICANON | ECHOE | ECHOK);
   t.c_cc[VERASE] = 8;
   if (tcsetattr(slave, TCSANOW, &t) != 0) {
      perror("tcsetattr broken");
      close(slave);
      close(master);
      return 15;
   }

   if (fix_terminal_fd(slave) != 0 ||
       tcgetattr(slave, &t) != 0) {
      close(slave);
      close(master);
      return 16;
   }

   if (!(t.c_iflag & ICRNL) ||
       !(t.c_lflag & ICANON) ||
       !(t.c_lflag & ECHO) ||
       !(t.c_lflag & ECHOE) ||
       t.c_cc[VERASE] != 127) {
      fprintf(stderr,
         "SELFTEST_FAIL iflag=0x%lx lflag=0x%lx erase=0x%02x\n",
         (unsigned long)t.c_iflag,
         (unsigned long)t.c_lflag,
         (unsigned)t.c_cc[VERASE]);
      close(slave);
      close(master);
      return 17;
   }

   /* End-to-end line discipline check:
    * type abc, Backspace(DEL), d, Enter(CR) -> child must read "abd\\n". */
   {
      const unsigned char keys[] = { 'a', 'b', 'c', 0x7f, 'd', '\r' };
      char line[16];
      ssize_t n;

      if (write(master, keys, sizeof(keys)) != (ssize_t)sizeof(keys)) {
         perror("selftest write");
         close(slave);
         close(master);
         return 18;
      }

      n = read(slave, line, sizeof(line));
      if (n != 4 || memcmp(line, "abd\n", 4) != 0) {
         fprintf(stderr, "SELFTEST_LINE_FAIL n=%ld bytes=", (long)n);
         if (n > 0) {
            ssize_t k;
            for (k = 0; k < n; ++k)
               fprintf(stderr, "%02x", (unsigned char)line[k]);
         }
         fputc('\n', stderr);
         close(slave);
         close(master);
         return 19;
      }
   }

   printf("H3531_TERMINAL_TERMIOS_SELFTEST_OK erase=DEL icrnl=1 canonical=1 echo=1 line=abd\\n\n");
   close(slave);
   close(master);
   return 0;
}

int main(int argc, char **argv)
{
   int i;

   if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
      return selftest();

   if (argc >= 2 && strcmp(argv[1], "--fix-only") == 0)
      return fix_terminal_fd(STDIN_FILENO) == 0 ? 0 : 1;

   if (argc >= 2 && strcmp(argv[1], "--status") == 0)
   {
      struct termios t;
      if (tcgetattr(STDIN_FILENO, &t) != 0) {
         perror("tcgetattr");
         return 2;
      }
      printf("isatty=%d iflag=0x%lx oflag=0x%lx cflag=0x%lx lflag=0x%lx erase=0x%02x\n",
         isatty(STDIN_FILENO),
         (unsigned long)t.c_iflag,
         (unsigned long)t.c_oflag,
         (unsigned long)t.c_cflag,
         (unsigned long)t.c_lflag,
         (unsigned)t.c_cc[VERASE]);
      return 0;
   }

   (void)fix_terminal_fd(STDIN_FILENO);
   (void)fix_terminal_fd(STDOUT_FILENO);
   (void)fix_terminal_fd(STDERR_FILENO);

   if (argc >= 2 && strcmp(argv[1], "--") == 0)
   {
      if (argc < 3)
         return 64;
      execvp(argv[2], &argv[2]);
      fprintf(stderr, "h3531-terminal-shell: exec %s failed: %s\n",
            argv[2], strerror(errno));
      return 126;
   }

   /* LXTerminal launches $SHELL directly. Keep the user-facing shell simple
    * and interactive after normalizing the PTY. */
   {
      char *sh_argv[] = { (char*)"sh", (char*)"-i", NULL };
      execv("/bin/sh", sh_argv);
   }

   fprintf(stderr, "h3531-terminal-shell: exec /bin/sh failed: %s\n",
         strerror(errno));
   return 126;
}
