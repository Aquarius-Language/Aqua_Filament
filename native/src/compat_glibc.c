/* Aqua_Filament - glibc compatibility shims for Linux.
 * Filament's official Linux release is compiled against glibc 2.38 headers, which redirect
 * strtol/sscanf to the C23 entry points (__isoc23_*) and bind fmod/fmodf to GLIBC_2.38.
 * Providing local definitions keeps libaqua_filament.so loadable on glibc 2.31+ distributions
 * (Ubuntu 20.04/22.04, Debian 11/12). C23 only adds "0b" binary prefixes, unused by Filament. */
#if defined(__linux__) && !defined(__ANDROID__)
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

#define HIDDEN __attribute__((visibility("hidden")))

/* Declared here instead of defining _GNU_SOURCE, which would redirect strtol to __isoc23_strtol. */
extern long long strtoll_l(const char*, char**, int, locale_t);
extern unsigned long long strtoull_l(const char*, char**, int, locale_t);

HIDDEN long __isoc23_strtol(const char* s, char** e, int b) { return strtol(s, e, b); }
HIDDEN long long __isoc23_strtoll(const char* s, char** e, int b) { return strtoll(s, e, b); }
HIDDEN unsigned long __isoc23_strtoul(const char* s, char** e, int b) { return strtoul(s, e, b); }
HIDDEN unsigned long long __isoc23_strtoull(const char* s, char** e, int b) { return strtoull(s, e, b); }
HIDDEN long long __isoc23_strtoll_l(const char* s, char** e, int b, locale_t l) { return strtoll_l(s, e, b, l); }
HIDDEN unsigned long long __isoc23_strtoull_l(const char* s, char** e, int b, locale_t l) { return strtoull_l(s, e, b, l); }
HIDDEN int __isoc23_vsscanf(const char* s, const char* f, va_list a) { return vsscanf(s, f, a); }
HIDDEN int __isoc23_sscanf(const char* s, const char* f, ...) {
    va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r;
}

/* fmod/fmodf: exact x87 remainder (same results as glibc). */
HIDDEN double fmod(double x, double y) {
    long double r;
    __asm__("1: fprem\n\t fnstsw %%ax\n\t testb $4, %%ah\n\t jnz 1b" : "=t"(r) : "0"((long double)x), "u"((long double)y) : "ax", "cc");
    return (double)r;
}
HIDDEN float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); }
#endif
