/* H3531 RetroArch launcher.
 *
 * Keep RETROARCH.APP as the file-manager/session entry point while the real
 * RetroArch frontend lives beside it as RETROARCH.BIN.  This guarantees that
 * a normal Enter on RETROARCH.APP always uses the H3531-specific config and
 * therefore the proven application lifecycle (Monitor -> exec -> app -> exit
 * -> fresh Monitor) can be tested without starting RetroArch from a second
 * shell that also competes for the framebuffer.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define H3531_RETROARCH_BIN "/mnt/usb/H3531/APPS/retroarch/RETROARCH.BIN"
#define H3531_RETROARCH_CFG "/mnt/usb/H3531/APPS/retroarch/retroarch.cfg"

int main(void)
{
   char *const argv[] = {
      (char *)H3531_RETROARCH_BIN,
      (char *)"--config",
      (char *)H3531_RETROARCH_CFG,
      (char *)"-v",
      NULL
   };

   fprintf(stderr,
         "H3531 RetroArch launcher: %s --config %s -v\n",
         H3531_RETROARCH_BIN, H3531_RETROARCH_CFG);
   fflush(stderr);

   execv(H3531_RETROARCH_BIN, argv);

   fprintf(stderr,
         "H3531 RetroArch launcher: execv failed: %s\n",
         strerror(errno));
   return 127;
}
