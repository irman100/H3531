#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>

/* H3531-only process preparation and fatal-signal diagnostics.
 * Timing is deliberately NOT implemented here. The emulator owns an explicit
 * per-video-frame CLOCK_MONOTONIC clock while AO runs independently.
 *
 * Diagnostics are always redirected to /var/fbzx.log. /var is the same
 * writable RAM area already used by Monitor for FILES resume state, so this
 * does not write SPI NOR. This deliberately does not depend on Monitor's
 * environment because real hardware showed that environment-based detection
 * was not reliable enough for post-launch UART diagnostics.
 *
 * For a deliberate direct UART run where terminal output is preferred, set
 * H3531_LOG_STDIO=1 before launching FBZX; then stdout/stderr are left alone.
 */

static size_t append_text(char *dst, size_t pos, const char *src)
{
    while (*src)
        dst[pos++] = *src++;
    return pos;
}

static size_t append_hex32(char *dst, size_t pos, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;
    for (shift = 28; shift >= 0; shift -= 4)
        dst[pos++] = hex[(value >> shift) & 0x0fU];
    return pos;
}

static size_t append_reg(char *dst, size_t pos, const char *name, uint32_t value)
{
    pos = append_text(dst, pos, name);
    pos = append_text(dst, pos, "=0x");
    return append_hex32(dst, pos, value);
}

static int keep_stdio_requested(void)
{
    const char *v = getenv("H3531_LOG_STDIO");
    return v && v[0] == '1' && v[1] == '\0';
}

static void prepare_session_log(void)
{
    static const char path[] = "/var/fbzx.log";
    static const char marker[] =
        "H3531 log: persistent RAM diagnostics -> /var/fbzx.log (log-v13)\n";
    static const char fail[] =
        "H3531 log: cannot open /var/fbzx.log; keeping inherited stdout/stderr\n";
    int fd;

    if (keep_stdio_requested())
        return;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        (void)write(STDERR_FILENO, fail, sizeof(fail) - 1U);
        return;
    }

    /* Redirect both streams before any normal diagnostics. */
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        if (fd > STDERR_FILENO)
            close(fd);
        return;
    }
    if (fd > STDERR_FILENO)
        close(fd);

    /* stdout would otherwise become fully buffered when redirected to a file.
       Keep newline diagnostics immediately visible to tail -f. */
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
    (void)write(STDERR_FILENO, marker, sizeof(marker) - 1U);
}

static void prepare_runtime(void)
{
    char exe_path[512];
    char *slash;
    ssize_t n;

    (void)setenv("SDL_VIDEODRIVER", "h3531", 1);
    (void)setenv("SDL_FBDEV", "/dev/fb0", 1);
    (void)setenv("H3531_FBZX_FAST16", "1", 1);

    n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1U);
    if (n <= 0 || (size_t)n >= sizeof(exe_path))
        return;
    exe_path[n] = '\0';
    slash = strrchr(exe_path, '/');
    if (!slash)
        return;
    if (slash == exe_path) {
        (void)chdir("/");
    } else {
        *slash = '\0';
        (void)chdir(exe_path);
    }
}

static void fatal_handler(int sig, siginfo_t *si, void *opaque)
{
    ucontext_t *uc = (ucontext_t *)opaque;
    uint32_t sp = (uint32_t)uc->uc_mcontext.arm_sp;
    char buf[768];
    size_t n = 0;
    unsigned i;

    n = append_text(buf, n, "H3531-SIGNAL ");
    n = append_reg(buf, n, "sig", (uint32_t)sig); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "code", (uint32_t)si->si_code); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "pc", (uint32_t)uc->uc_mcontext.arm_pc); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "lr", (uint32_t)uc->uc_mcontext.arm_lr); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "sp", sp); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "cpsr", (uint32_t)uc->uc_mcontext.arm_cpsr); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "fault", (uint32_t)uc->uc_mcontext.fault_address); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "si_addr", (uint32_t)(uintptr_t)si->si_addr);
    n = append_text(buf, n, "\nH3531-REGS ");

    n = append_reg(buf, n, "r0", (uint32_t)uc->uc_mcontext.arm_r0); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r1", (uint32_t)uc->uc_mcontext.arm_r1); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r2", (uint32_t)uc->uc_mcontext.arm_r2); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r3", (uint32_t)uc->uc_mcontext.arm_r3); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r4", (uint32_t)uc->uc_mcontext.arm_r4); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r5", (uint32_t)uc->uc_mcontext.arm_r5); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r6", (uint32_t)uc->uc_mcontext.arm_r6); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r7", (uint32_t)uc->uc_mcontext.arm_r7); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r8", (uint32_t)uc->uc_mcontext.arm_r8); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r9", (uint32_t)uc->uc_mcontext.arm_r9); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r10", (uint32_t)uc->uc_mcontext.arm_r10); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "fp", (uint32_t)uc->uc_mcontext.arm_fp); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "ip", (uint32_t)uc->uc_mcontext.arm_ip);
    n = append_text(buf, n, "\n");

    if (sp >= 0x00010000U && sp <= 0xbfffffc0U) {
        const volatile uint32_t *stack = (const volatile uint32_t *)(uintptr_t)sp;
        n = append_text(buf, n, "H3531-STACK");
        for (i = 0; i < 16; ++i) {
            n = append_text(buf, n, " s");
            n = append_hex32(buf, n, i);
            n = append_text(buf, n, "=0x");
            n = append_hex32(buf, n, stack[i]);
        }
        n = append_text(buf, n, "\n");
    }

    (void)write(STDERR_FILENO, buf, n);
    _exit(128 + sig);
}

static int install_one(int sig)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fatal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    return sigaction(sig, &sa, 0);
}

__attribute__((constructor))
static void install_runtime(void)
{
    static const char ok[] =
        "H3531 runtime prepared; buffered AO worker + per-frame clock + fast16 video + unconditional /var RAM log + fatal-signal diagnostic v7 enabled\n";
    static const char fail[] = "H3531 fatal-signal diagnostic install failed\n";
    int rc = 0;

    /* Capture from the first normal diagnostic line onward. */
    prepare_session_log();
    prepare_runtime();
    rc |= install_one(SIGILL);
    rc |= install_one(SIGSEGV);
    rc |= install_one(SIGBUS);
    rc |= install_one(SIGFPE);

    if (rc == 0)
        (void)write(STDERR_FILENO, ok, sizeof(ok) - 1U);
    else
        (void)write(STDERR_FILENO, fail, sizeof(fail) - 1U);
}
