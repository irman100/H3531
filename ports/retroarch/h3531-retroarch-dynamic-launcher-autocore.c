/* H3531 RetroArch dynamic-core launcher.
 *
 * Stage 3.5 keeps the Stage 3.4 core-autodetect behavior:
 * - menu launch starts with no preloaded core so RetroArch's normal core-info
 *   matcher selects the compatible external core when content is chosen;
 * - direct content launch selects a deterministic core by file extension:
 *   NES -> FCEUmm, Mega Drive/32X -> PicoDrive.
 *
 * Video scaling is no longer selected through a H3531-specific environment
 * cap. RetroArch's normal video_force_aspect/video_scale_integer/aspect-ratio
 * settings determine the viewport; the H3531 video driver only presents it to
 * the board framebuffer.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define H3531_RETROARCH_BIN "/mnt/usb/H3531/APPS/retroarch/RETROARCH.BIN"
#define H3531_RETROARCH_CFG "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg"
#define H3531_RETROARCH_RUNTIME "/mnt/usb/H3531/APPS/retroarch/runtime"
#define H3531_FCEUMM_CORE "/mnt/usb/H3531/APPS/retroarch/cores/fceumm_libretro.so"
#define H3531_PICODRIVE_CORE "/mnt/usb/H3531/APPS/retroarch/cores/picodrive_libretro.so"

static void h3531_ensure_dir(const char *path)
{
   if (mkdir(path, 0777) != 0 && errno != EEXIST)
      fprintf(stderr, "H3531 RetroArch launcher: mkdir %s failed: %s\n",
            path, strerror(errno));
}

static const char *h3531_core_for_content(const char *path)
{
   const char *ext;

   if (!path)
      return NULL;

   ext = strrchr(path, '.');
   if (!ext)
      return NULL;

   if (!strcasecmp(ext, ".nes"))
      return H3531_FCEUMM_CORE;

   if (!strcasecmp(ext, ".gen") ||
       !strcasecmp(ext, ".smd") ||
       !strcasecmp(ext, ".md") ||
       !strcasecmp(ext, ".32x") ||
       !strcasecmp(ext, ".bin"))
      return H3531_PICODRIVE_CORE;

   return NULL;
}

static void h3531_prepare_user_dirs(void)
{
   h3531_ensure_dir("/mnt/usb/H3531/USER");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/saves");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/states");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/playlists");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/playlists/builtin");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/system");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/config");
   h3531_ensure_dir("/mnt/usb/H3531/USER/retroarch/screenshots");
}

int main(int argc, char **argv)
{
   char *ra_argv[11];
   const char *content_core = NULL;
   int n = 0;
   int has_content = argc > 1 && argv[1] && argv[1][0];

   if (has_content)
   {
      content_core = h3531_core_for_content(argv[1]);
      if (!content_core)
      {
         fprintf(stderr,
               "H3531 StayPlaytion launcher: unsupported direct content: %s\n",
               argv[1]);
         return 65;
      }
   }

   if (setenv("LD_LIBRARY_PATH", H3531_RETROARCH_RUNTIME, 1) != 0)
   {
      fprintf(stderr,
            "H3531 RetroArch launcher: setenv LD_LIBRARY_PATH failed: %s\n",
            strerror(errno));
      return 126;
   }

   h3531_prepare_user_dirs();

   ra_argv[n++] = (char *)H3531_RETROARCH_BIN;
   ra_argv[n++] = (char *)"--config";
   ra_argv[n++] = (char *)H3531_RETROARCH_CFG;

   if (has_content)
   {
      ra_argv[n++] = (char *)"-L";
      ra_argv[n++] = (char *)content_core;
      ra_argv[n++] = argv[1];
   }
   else
   {
      /* Do NOT pass -L here. A command-line core without content leaves a
       * misleading core state on this pinned RetroArch build. Let the normal
       * menu core-info matcher select the sole compatible core after content
       * is chosen instead. */
      ra_argv[n++] = (char *)"--menu";
   }

   ra_argv[n++] = (char *)"-v";
   ra_argv[n] = NULL;

   fprintf(stderr,
         "H3531 RetroArch launcher: frontend=%s mode=%s scaling=retroarch\n",
         H3531_RETROARCH_BIN,
         has_content ? "direct-extension-core" : "menu-autodetect-core");
   if (has_content)
      fprintf(stderr,
            "H3531 RetroArch launcher: core=%s content=%s\n",
            content_core, argv[1]);
   fprintf(stderr,
         "H3531 RetroArch launcher: LD_LIBRARY_PATH=%s\n",
         H3531_RETROARCH_RUNTIME);
   fflush(stderr);

   execv(H3531_RETROARCH_BIN, ra_argv);

   fprintf(stderr,
         "H3531 RetroArch launcher: execv failed: %s\n",
         strerror(errno));
   return 127;
}
