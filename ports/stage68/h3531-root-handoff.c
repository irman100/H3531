#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pivot_root
#ifdef __NR_pivot_root
#define SYS_pivot_root __NR_pivot_root
#endif
#endif

static void msg(const char *s)
{
    int fd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (fd >= 0) {
        write(fd, s, strlen(s));
        write(fd, "\n", 1);
        close(fd);
    }
}

static int move_mount(const char *src, const char *newroot, const char *leaf)
{
    char dst[PATH_MAX];
    if (snprintf(dst, sizeof(dst), "%s/%s", newroot, leaf) >= (int)sizeof(dst))
        return -1;
    if (mount(src, dst, NULL, MS_MOVE, NULL) < 0) {
        fprintf(stderr, "move mount %s -> %s failed: %s\n", src, dst, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *newroot;
    const char *init;

    if (argc != 3) {
        msg("STAYFW: root handoff usage error");
        return 64;
    }

    newroot = argv[1];
    init = argv[2];

    if (getpid() != 1) {
        msg("STAYFW: root handoff must run as PID 1");
        return 65;
    }

    if (chdir(newroot) < 0) {
        perror("chdir newroot");
        return 66;
    }
    if (chdir("/") < 0)
        return 67;

    if (move_mount("/dev", newroot, "dev") < 0)
        return 68;
    if (move_mount("/proc", newroot, "proc") < 0)
        return 69;
    if (move_mount("/sys", newroot, "sys") < 0)
        return 70;

    if (chdir(newroot) < 0) {
        perror("chdir newroot before pivot");
        return 71;
    }

#ifdef SYS_pivot_root
    if (syscall(SYS_pivot_root, ".", "oldroot") < 0) {
        perror("pivot_root");
        return 72;
    }
#else
    msg("STAYFW: pivot_root syscall number unavailable");
    return 73;
#endif

    if (chdir("/") < 0)
        return 74;

    msg("STAYFW: pivot_root complete; executing external init");
    execl(init, init, (char *)NULL);

    perror("exec external init");
    msg("STAYFW: external init failed; opening firmware rescue shell");
    execl("/oldroot/bin/sh", "sh", (char *)NULL);
    return 75;
}
