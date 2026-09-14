/* H3531 RetroArch dynamic-core launcher.
 *
 * Stage 3.4 fixes the misleading "preloaded" FCEUmm state seen in Stage 3.3.
 * When RetroArch is opened without content we deliberately start the menu with
 * NO core loaded. RetroArch's normal content detection then scans /cores +
 * /info; with a single matching .nes core it selects FCEUmm automatically.
 * When FILES passes a ROM path, the launcher still supplies FCEUmm explicitly
 * so direct .nes launch is deterministic.
 *
 * Stage 3.4 also selects the H3531 performance video profile (integer scale
 * capped at 2x) to reduce framebuffer bandwidth while we finish the 60 FPS
 * path. The video driver can still be run without this cap by starting
 * RETROARCH.BIN directly and leaving RETROARCH_H3531_SCALE_MAX unset.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define H3531_RETROARCH_BIN "/mnt/usb/H3531/APPS/retroarch/RETROARCH.BIN"
#define H3531_RETROARCH_CFG "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg"
#define H3531_RETROARCH_RUNTIME "/mnt/usb/H3531/APPS/retroarch/runtime"
#define H3531_FCEUMM_CORE "/mnt/usb/H3531/APPS/retroarch/cores/fceumm_libretro.so"

static void h3531_ensure_dir(const char *path)
{
   if (mkdir(path, 0777) != 0 && errno != EEXIST)
      fprintf(stderr, "H3531 RetroArch launcher: mkdir %s failed: %s\n",
            path, strerror(errno));
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
   int n = 0;
   int has_content = argc > 1 && argv[1] && argv[1][0];

   if (setenv("LD_LIBRARY_PATH", H3531_RETROARCH_RUNTIME, 1) != 0)
   {
      fprintf(stderr,
            "H3531 RetroArch launcher: setenv LD_LIBRARY_PATH failed: %s\n",
            strerror(errno));
      return 126;
   }

   if (setenv("RETROARCH_H3531_SCALE_MAX", "2", 1) != 0)
   {
      fprintf(stderr,
            "H3531 RetroArch launcher: setenv scale profile failed: %s\n",
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
      ra_argv[n++] = (char *)H3531_FCEUMM_CORE;
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
         "H3531 RetroArch launcher: frontend=%s mode=%s scale_max=2\n",
         H3531_RETROARCH_BIN,
         has_content ? "direct-fceumm-content" : "menu-autodetect-core");
   if (has_content)
      fprintf(stderr,
            "H3531 RetroArch launcher: core=%s content=%s\n",
            H3531_FCEUMM_CORE, argv[1]);
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
