/* H3531 RetroArch dynamic-core launcher with default FCEUmm core.
 *
 * RETROARCH.APP stays a tiny static session entry point.  The frontend and
 * libretro core remain separate dynamic components on the USB stick.
 *
 * Stage 3.2 usability change:
 *   - FCEUmm is selected automatically at RetroArch startup, so the user can
 *     choose Load Content immediately.
 *   - If FILES later passes a content path to RETROARCH.APP, the launcher
 *     forwards it to RetroArch after selecting FCEUmm.
 *   - Load Core remains available, so another core may still be selected from
 *     the RetroArch menu.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define H3531_RETROARCH_BIN "/mnt/usb/H3531/APPS/retroarch/RETROARCH.BIN"
#define H3531_RETROARCH_CFG "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg"
#define H3531_RETROARCH_RUNTIME "/mnt/usb/H3531/APPS/retroarch/runtime"
#define H3531_FCEUMM_CORE "/mnt/usb/H3531/APPS/retroarch/cores/fceumm_libretro.so"

int main(int argc, char **argv)
{
   char *ra_argv[9];
   int n = 0;

   if (setenv("LD_LIBRARY_PATH", H3531_RETROARCH_RUNTIME, 1) != 0)
   {
      fprintf(stderr,
            "H3531 RetroArch launcher: setenv LD_LIBRARY_PATH failed: %s\n",
            strerror(errno));
      return 126;
   }

   ra_argv[n++] = (char *)H3531_RETROARCH_BIN;
   ra_argv[n++] = (char *)"--config";
   ra_argv[n++] = (char *)H3531_RETROARCH_CFG;
   ra_argv[n++] = (char *)"-L";
   ra_argv[n++] = (char *)H3531_FCEUMM_CORE;
   ra_argv[n++] = (char *)"-v";

   /* Future FILES association path: RETROARCH.APP /mnt/usb/games/nes/game.nes */
   if (argc > 1 && argv[1] && argv[1][0])
      ra_argv[n++] = argv[1];

   ra_argv[n] = NULL;

   fprintf(stderr,
         "H3531 RetroArch launcher: frontend=%s default_core=%s\n",
         H3531_RETROARCH_BIN, H3531_FCEUMM_CORE);
   if (argc > 1 && argv[1] && argv[1][0])
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
