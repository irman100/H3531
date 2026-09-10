#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ucontext.h>

/*
 * Temporary real-board diagnostic helper for FBZX on Hi3531.
 *
 * It also performs H3531 runtime preparation before main(): force the custom
 * SDL backend/device regardless of inherited environment and change cwd to the
 * executable directory so relative FBZX resources (spectrum-roms/, keymap.bmp)
 * resolve when the Monitor/File Manager execs an .APP from another directory.
 *
 * The factory Linux 3.0.8 image does not log useful user-space fatal-signal
 * register state to dmesg. Install minimal SA_SIGINFO handlers before main()
 * so SIGILL/SIGSEGV/SIGBUS/SIGFPE report ARM register state over UART.
 * For a sane user stack, also dump the first 16 words from the interrupted SP;
 * this is used to recover saved caller addresses from libc allocator frames.
 * Only async-signal-safe write(2) and _exit(2) are called by the handler.
 */

static size_t append_text(char *dst, size_t pos, const char *src)
{
    while (*src != '\0') {
        dst[pos++] = *src++;
    }
    return pos;
}

static size_t append_hex32(char *dst, size_t pos, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    int shift;

    for (shift = 28; shift >= 0; shift -= 4) {
        dst[pos++] = hex[(value >> shift) & 0x0fU];
    }
    return pos;
}

static size_t append_reg(char *dst, size_t pos, const char *name, uint32_t value)
{
    pos = append_text(dst, pos, name);
    pos = append_text(dst, pos, "=0x");
    pos = append_hex32(dst, pos, value);
    return pos;
}

static void h3531_prepare_runtime(void)
{
    char exe_path[512];
    char *slash;
    ssize_t n;

    /* This binary is an H3531-specific port, so inherited desktop/invalid SDL
       settings must never override the board backend. */
    (void)setenv("SDL_VIDEODRIVER", "h3531", 1);
    (void)setenv("SDL_FBDEV", "/dev/fb0", 1);

    /* File Manager may exec the application while its cwd is / or another
       directory. FBZX 3.1.0 loads ROMs and keymap through relative paths, so
       anchor cwd to the directory containing the executable itself. */
    n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1U);
    if (n <= 0 || (size_t)n >= sizeof(exe_path)) {
        return;
    }
    exe_path[n] = '\0';
    slash = strrchr(exe_path, '/');
    if (slash == NULL) {
        return;
    }
    if (slash == exe_path) {
        (void)chdir("/");
    } else {
        *slash = '\0';
        (void)chdir(exe_path);
    }
}

static void h3531_signal_handler(int sig, siginfo_t *si, void *opaque_context)
{
    ucontext_t *uc = (ucontext_t *)opaque_context;
    uint32_t sp = (uint32_t)uc->uc_mcontext.arm_sp;
    char buf[768];
    size_t n = 0;
    unsigned int i;

    n = append_text(buf, n, "H3531-SIGNAL ");
    n = append_reg(buf, n, "sig", (uint32_t)sig);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "code", (uint32_t)si->si_code);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "pc", (uint32_t)uc->uc_mcontext.arm_pc);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "lr", (uint32_t)uc->uc_mcontext.arm_lr);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "sp", sp);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "cpsr", (uint32_t)uc->uc_mcontext.arm_cpsr);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "fault", (uint32_t)uc->uc_mcontext.fault_address);
    n = append_text(buf, n, " ");
    n = append_reg(buf, n, "si_addr", (uint32_t)(uintptr_t)si->si_addr);
    n = append_text(buf, n, "\nH3531-REGS ");

    n = append_reg(buf, n, "r0",  (uint32_t)uc->uc_mcontext.arm_r0);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r1",  (uint32_t)uc->uc_mcontext.arm_r1);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r2",  (uint32_t)uc->uc_mcontext.arm_r2);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r3",  (uint32_t)uc->uc_mcontext.arm_r3);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r4",  (uint32_t)uc->uc_mcontext.arm_r4);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r5",  (uint32_t)uc->uc_mcontext.arm_r5);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r6",  (uint32_t)uc->uc_mcontext.arm_r6);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r7",  (uint32_t)uc->uc_mcontext.arm_r7);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r8",  (uint32_t)uc->uc_mcontext.arm_r8);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r9",  (uint32_t)uc->uc_mcontext.arm_r9);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "r10", (uint32_t)uc->uc_mcontext.arm_r10); n = append_text(buf, n, " ");
    n = append_reg(buf, n, "fp",  (uint32_t)uc->uc_mcontext.arm_fp);  n = append_text(buf, n, " ");
    n = append_reg(buf, n, "ip",  (uint32_t)uc->uc_mcontext.arm_ip);
    n = append_text(buf, n, "\n");

    /* Hi3531 user stacks live below 0xc0000000.  The bounds check avoids
       dereferencing obviously bogus SP values when the signal itself is a
       stack-corruption fault. */
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

    sa.sa_sigaction = h3531_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_restorer = 0;
    return sigaction(sig, &sa, 0);
}

__attribute__((constructor))
static void h3531_install_signal_handlers(void)
{
    static const char ok[] = "H3531 runtime prepared; fatal-signal diagnostic v2 enabled\n";
    static const char fail[] = "H3531 fatal-signal diagnostic install failed\n";
    int rc = 0;

    h3531_prepare_runtime();

    rc |= install_one(SIGILL);
    rc |= install_one(SIGSEGV);
    rc |= install_one(SIGBUS);
    rc |= install_one(SIGFPE);

    if (rc == 0) {
        (void)write(STDERR_FILENO, ok, sizeof(ok) - 1U);
    } else {
        (void)write(STDERR_FILENO, fail, sizeof(fail) - 1U);
    }
}
