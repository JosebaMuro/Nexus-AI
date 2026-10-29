/*
 * Nexus-Office autonomo: que Android no mate a Nexus-Office por una llamada al sistema prohibida.
 *
 * Android aplica a las apps un filtro seccomp: si un programa usa una llamada al sistema que no esta en su
 * lista (p. ej. mbind, que usa OpenBLAS al reservar memoria al importar numpy), el kernel envia SIGSYS y el
 * proceso muere con el codigo 159 (128 + 31). Bionic nunca las usa, pero musl, Python y sus paquetes si.
 *
 * Este manejador de SIGSYS hace que la llamada devuelva -ENOSYS, como en un kernel que no la tuviera: los
 * programas ya saben seguir sin ella. Avisa una vez por llamada en el registro (nexus.log).
 *
 * Python lo carga con el cargador de musl (--preload). llama-server, que es estatico, lo lleva enlazado.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static volatile unsigned char seen[1024];

static void on_sigsys(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    (void)sig;
    (void)si;
#if defined(__aarch64__)
    long nr = (long)uc->uc_mcontext.regs[8];
    uc->uc_mcontext.regs[0] = (unsigned long)-ENOSYS;
#elif defined(__x86_64__)
    long nr = (long)uc->uc_mcontext.gregs[REG_RAX];   /* solo para probarlo en un PC */
    nr = si->si_syscall;
    uc->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#else
#error "arquitectura no soportada"
#endif
    if (nr >= 0 && nr < 1024 && !seen[nr]) {
        static const char pre[] = "nexus: Android no permite la llamada al sistema ";
        static const char post[] = "; se continua sin ella\n";
        char msg[sizeof pre + sizeof post + 12], num[12];
        int i = 0;
        long n = nr;
        size_t l = sizeof pre - 1;
        seen[nr] = 1;
        memcpy(msg, pre, l);
        do {
            num[i++] = (char)('0' + n % 10);
            n /= 10;
        } while (n && i < 11);
        while (i) msg[l++] = num[--i];
        memcpy(msg + l, post, sizeof post - 1);
        l += sizeof post - 1;
        if (write(2, msg, l) < 0) {
        }
    }
}

__attribute__((constructor)) static void nexus_sigsys_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sigsys;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, 0);
}
