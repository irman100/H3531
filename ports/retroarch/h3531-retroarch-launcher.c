/* H3531 RetroArch dynamic-runtime launcher.
 *
 * RETROARCH.APP remains a small fully static session entry point.  It does
 * not rely on the recorder rootfs C library.  Instead it execs the musl
 * dynamic loader shipped beside RetroArch and gives that loader an explicit
 * private library search directory.  This lets RETROARCH.BIN use normal
 * dlopen()-based libretro .so cores without installing anything into /lib or
 * changing the proven Monitor -> exec -> app -> exit -> Monitor lifecycle.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define H3531_RETROARCH_ROOT    "/mnt/usb/H3531/APPS/retroarch"
#define H3531_MUSL_LOADER       H3531_RETROARCH_ROOT "/runtime/ld-musl-arm.so.1"
#define H3531_RUNTIME_DIR       H3531_RETROARCH_ROOT "/runtime"
#define H3531_RETROARCH_BIN     H3531_RETROARCH_ROOT "/RETROARCH.BIN"
#define H3531_RETROARCH_CFG     H3531_RETROARCH_ROOT "/retroarch.cfg"

int main(void)
{
   char *const argv[] = {
      (char *)H3531_MUSL_LOADER,
      (char *)"--library-path",
      (char *)H3531_RUNTIME_DIR,
      (char *)H3531_RETROARCH_BIN,
      (char *)"--config",
      (char *)H3531_RETROARCH_CFG,
      (char *)"-v",
      NULL
   };

   fprintf(stderr,
         "H3531 RetroArch launcher: %s --library-path %s %s --config %s -v\n",
         H3531_MUSL_LOADER,
         H3531_RUNTIME_DIR,
         H3531_RETROARCH_BIN,
         H3531_RETROARCH_CFG);
   fflush(stderr);

   execv(H3531_MUSL_LOADER, argv);

   fprintf(stderr,
         "H3531 RetroArch launcher: execv loader failed: %s\n",
         strerror(errno));
   return 127;
}
