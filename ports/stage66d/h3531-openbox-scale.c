#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(const char *path, size_t *len_out)
{
   FILE *f = fopen(path, "rb");
   long n;
   char *buf;
   if (!f) return NULL;
   if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
   n = ftell(f);
   if (n < 0) { fclose(f); return NULL; }
   if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
   buf = (char*)malloc((size_t)n + 1);
   if (!buf) { fclose(f); return NULL; }
   if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
      free(buf); fclose(f); return NULL;
   }
   fclose(f);
   buf[n] = '\0';
   if (len_out) *len_out = (size_t)n;
   return buf;
}

static int write_all(const char *path, const char *buf, size_t len)
{
   FILE *f = fopen(path, "wb");
   if (!f) return -1;
   if (len && fwrite(buf, 1, len, f) != len) { fclose(f); return -1; }
   if (fclose(f) != 0) return -1;
   return 0;
}

int main(int argc, char **argv)
{
   const char *in_path;
   const char *out_path;
   const char *size_text;
   char *src;
   size_t src_len;
   const char *p;
   size_t out_cap;
   size_t out_len = 0;
   char *out;
   unsigned replacements = 0;

   if (argc != 4) {
      fprintf(stderr, "usage: %s INPUT_RC OUTPUT_RC FONT_SIZE\n", argv[0]);
      return 64;
   }

   in_path = argv[1];
   out_path = argv[2];
   size_text = argv[3];

   if (atoi(size_text) < 8 || atoi(size_text) > 32) {
      fprintf(stderr, "invalid font size: %s\n", size_text);
      return 65;
   }

   src = read_all(in_path, &src_len);
   if (!src) {
      fprintf(stderr, "cannot read %s: %s\n", in_path, strerror(errno));
      return 2;
   }

   out_cap = src_len + 1024;
   out = (char*)malloc(out_cap);
   if (!out) {
      free(src);
      return 3;
   }

   p = src;
   while (*p) {
      const char *tag = strstr(p, "<size>");
      if (!tag) {
         size_t rest = strlen(p);
         if (out_len + rest + 1 > out_cap) {
            out_cap = out_len + rest + 1024;
            out = (char*)realloc(out, out_cap);
            if (!out) { free(src); return 3; }
         }
         memcpy(out + out_len, p, rest);
         out_len += rest;
         break;
      }

      const char *end = strstr(tag, "</size>");
      if (!end) {
         size_t rest = strlen(p);
         if (out_len + rest + 1 > out_cap) {
            out_cap = out_len + rest + 1024;
            out = (char*)realloc(out, out_cap);
            if (!out) { free(src); return 3; }
         }
         memcpy(out + out_len, p, rest);
         out_len += rest;
         break;
      }

      size_t prefix = (size_t)(tag - p);
      size_t need = prefix + strlen("<size>") + strlen(size_text) + strlen("</size>");
      if (out_len + need + 1 > out_cap) {
         out_cap = out_len + need + 1024;
         out = (char*)realloc(out, out_cap);
         if (!out) { free(src); return 3; }
      }

      memcpy(out + out_len, p, prefix);
      out_len += prefix;
      memcpy(out + out_len, "<size>", 6);
      out_len += 6;
      memcpy(out + out_len, size_text, strlen(size_text));
      out_len += strlen(size_text);
      memcpy(out + out_len, "</size>", 7);
      out_len += 7;
      replacements++;

      p = end + 7;
   }

   out[out_len] = '\0';
   if (write_all(out_path, out, out_len) != 0) {
      fprintf(stderr, "cannot write %s: %s\n", out_path, strerror(errno));
      free(out);
      free(src);
      return 4;
   }

   printf("[UI-SCALE] openbox fonts=%s replacements=%u input=%s output=%s\n",
          size_text, replacements, in_path, out_path);

   free(out);
   free(src);
   return replacements ? 0 : 5;
}
