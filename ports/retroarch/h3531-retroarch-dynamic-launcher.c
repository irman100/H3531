/* H3531 RetroArch dynamic-core launcher.
 *
 * RETROARCH.APP stays a tiny, fully static FILES/session entry point.
 * RETROARCH.BIN is a dynamically linked musl RetroArch frontend whose ELF
 * interpreter lives on the USB stick beside it.  The launcher supplies the
 * matching runtime search path, then replaces itself with RetroArch.
 *
 * This preserves the physically proven lifecycle:
 *   Monitor -> FILES -> RETROARCH.APP -> exec RetroArch -> exit -> Monitor
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define H3531_RETROARCH_BIN "/mnt/usb/H3531/APPS/retroarch/RETROARCH.BIN"
#define H3531_RETROARCH_CFG "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg"
#define H3531_RETROARCH_RUNTIME "/mnt/usb/H3531/APPS/retroarch/runtime"

int main(void)
{
   char *const argv[] = {
      (char *)H3531_RETROARCH_BIN,
      (char *)"--config",
      (char *)H3531_RETROARCH_CFG,
      (char *)"-v",
      NULL
   };

   if (setenv("LD_LIBRARY_PATH", H3531_RETROARCH_RUNTIME, 1) != 0)
   {
      fprintf(stderr,
            "H3531 RetroArch launcher: setenv LD_LIBRARY_PATH failed: %s\n",
            strerror(errno));
      return 126;
   }

   fprintf(stderr,
         "H3531 RetroArch dynamic launcher: %s --config %s -v\n",
         H3531_RETROARCH_BIN, H3531_RETROARCH_CFG);
   fprintf(stderr,
         "H3531 RetroArch dynamic launcher: LD_LIBRARY_PATH=%s\n",
         H3531_RETROARCH_RUNTIME);
   fflush(stderr);

   execv(H3531_RETROARCH_BIN, argv);

   fprintf(stderr,
         "H3531 RetroArch dynamic launcher: execv failed: %s\n",
         strerror(errno));
   return 127;
}
