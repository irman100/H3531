/* H3531 RetroArch dynamic-core launcher with default FCEUmm core.
 *
 * RETROARCH.APP stays a tiny static session entry point. The frontend and
 * libretro core remain separate dynamic components on the USB stick.
 *
 * Stage 3.3 usability fix:
 *   - FCEUmm is selected automatically at RetroArch startup;
 *   - when no content path is supplied, --menu keeps RetroArch alive even
 *     though FCEUmm itself requires content;
 *   - when FILES passes a .nes path, RetroArch launches it directly with
 *     FCEUmm without showing the core-selection menu.
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

   h3531_prepare_user_dirs();

   ra_argv[n++] = (char *)H3531_RETROARCH_BIN;
   ra_argv[n++] = (char *)"--config";
   ra_argv[n++] = (char *)H3531_RETROARCH_CFG;
   ra_argv[n++] = (char *)"-L";
   ra_argv[n++] = (char *)H3531_FCEUMM_CORE;

   if (has_content)
      ra_argv[n++] = argv[1];
   else
      ra_argv[n++] = (char *)"--menu";

   ra_argv[n++] = (char *)"-v";
   ra_argv[n] = NULL;

   fprintf(stderr,
         "H3531 RetroArch launcher: frontend=%s default_core=%s mode=%s\n",
         H3531_RETROARCH_BIN, H3531_FCEUMM_CORE,
         has_content ? "direct-content" : "menu-with-core");
   if (has_content)
      fprintf(stderr, "H3531 RetroArch launcher: content=%s\n", argv[1]);
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
