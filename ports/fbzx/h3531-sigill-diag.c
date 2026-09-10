#define _GNU_SOURCE
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <ucontext.h>

/*
 * Temporary real-board diagnostic helper for FBZX on Hi3531.
 *
 * The factory Linux 3.0.8 image does not log useful user-space fatal-signal
 * register state to dmesg. Install minimal SA_SIGINFO handlers before main()
 * so SIGILL/SIGSEGV/SIGBUS/SIGFPE report ARM PC/LR/SP/CPSR and fault address
 * over the existing UART stderr. Only async-signal-safe write(2) and _exit(2)
 * are used from the handler.
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

static void h3531_signal_handler(int sig, siginfo_t *si, void *opaque_context)
{
    ucontext_t *uc = (ucontext_t *)opaque_context;
    char buf[224];
    size_t n = 0;

    n = append_text(buf, n, "H3531-SIGNAL sig=0x");
    n = append_hex32(buf, n, (uint32_t)sig);
    n = append_text(buf, n, " code=0x");
    n = append_hex32(buf, n, (uint32_t)si->si_code);
    n = append_text(buf, n, " pc=0x");
    n = append_hex32(buf, n, (uint32_t)uc->uc_mcontext.arm_pc);
    n = append_text(buf, n, " lr=0x");
    n = append_hex32(buf, n, (uint32_t)uc->uc_mcontext.arm_lr);
    n = append_text(buf, n, " sp=0x");
    n = append_hex32(buf, n, (uint32_t)uc->uc_mcontext.arm_sp);
    n = append_text(buf, n, " cpsr=0x");
    n = append_hex32(buf, n, (uint32_t)uc->uc_mcontext.arm_cpsr);
    n = append_text(buf, n, " fault=0x");
    n = append_hex32(buf, n, (uint32_t)uc->uc_mcontext.fault_address);
    n = append_text(buf, n, " si_addr=0x");
    n = append_hex32(buf, n, (uint32_t)(uintptr_t)si->si_addr);
    n = append_text(buf, n, "\n");

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
    static const char ok[] = "H3531 fatal-signal diagnostic enabled\n";
    static const char fail[] = "H3531 fatal-signal diagnostic install failed\n";
    int rc = 0;

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
