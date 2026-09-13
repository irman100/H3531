from pathlib import Path
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
p = root / "src" / "keyboard.c"
s = p.read_text()

# H3531 uses one history file shared with Monitor PTY/Shell/other BASICs.
# Keep the patch completely H3531-specific so upstream behaviour is unchanged.
if "#include <stdlib.h>" not in s:
    marker = "#include <stdio.h>\n"
    if s.count(marker) != 1:
        raise SystemExit(f"stdio include marker count={s.count(marker)}")
    s = s.replace(marker, marker + "#include <stdlib.h>\n", 1)

hist_marker = "static int32 histlength[MAXHIST];   /* Table of sizes of entries in history buffer      */\n"
if s.count(hist_marker) != 1:
    raise SystemExit(f"history declaration marker count={s.count(hist_marker)}")

helpers = r'''
#ifdef TARGET_H3531
/* H3531 Home Computer: persist the same last-20 line history used by Monitor,
 * Shell, Tiny BASIC and Bywater.  The exclusive exec lifecycle guarantees that
 * only one interactive application writes this file at a time. */
static void add_history(char command[], int32 cmdlen);
static int h3531_history_loading = 0;
static int h3531_history_loaded = 0;

static const char *h3531_history_path(void) {
  const char *p = getenv("H3531_HISTORY");
  if (p != NULL && *p != '\0') return p;
  return "/mnt/usb/H3531/USER/COMMAND.HST";
}

static void h3531_history_save(void) {
  FILE *f;
  int i, off = 0;
  if (h3531_history_loading) return;
  f = fopen(h3531_history_path(), "w");
  if (f == NULL) return;
  for (i = 0; i < histindex; i++) {
    const char *line = &histbuffer[off];
    if (*line != '\0') {
      fputs(line, f);
      fputc('\n', f);
    }
    off += histlength[i];
  }
  fclose(f);
}

static void h3531_history_load(void) {
  FILE *f;
  char line[256];
  if (h3531_history_loaded) return;
  h3531_history_loaded = 1;
  f = fopen(h3531_history_path(), "r");
  if (f == NULL) return;
  h3531_history_loading = 1;
  while (fgets(line, sizeof(line), f) != NULL) {
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
    if (n > 0) add_history(line, (int32)n);
  }
  h3531_history_loading = 0;
  fclose(f);
  fprintf(stderr, "H3531 BASIC: shared history loaded from %s\n", h3531_history_path());
}
#endif
'''
s = s.replace(hist_marker, hist_marker + helpers, 1)

add_tail = "  histlength[histindex] = cmdlen+1;\n  highbuffer += cmdlen+1;\n  histindex += 1;\n}"
add_new = "  histlength[histindex] = cmdlen+1;\n  highbuffer += cmdlen+1;\n  histindex += 1;\n#ifdef TARGET_H3531\n  h3531_history_save();\n#endif\n}"
if s.count(add_tail) != 1:
    raise SystemExit(f"add_history tail count={s.count(add_tail)}")
s = s.replace(add_tail, add_new, 1)

# There is also a RISC OS kbd_init; patch only the final generic/Unix one.
kbd_marker = "boolean kbd_init() {\n  int n;\n"
pos = s.rfind(kbd_marker)
if pos < 0:
    raise SystemExit("generic kbd_init marker not found")
insert_at = pos + len(kbd_marker)
s = s[:insert_at] + "\n#ifdef TARGET_H3531\n  h3531_history_load();\n#endif\n" + s[insert_at:]

p.write_text(s)
print("H3531 Matrix Brandy shared-history patch applied")
