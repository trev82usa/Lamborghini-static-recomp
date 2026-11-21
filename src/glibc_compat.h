/* Portable Linux binaries: bind the few libm/libc functions whose newest
 * symbol versions (glibc 2.38 / 2.43) would otherwise make the binary refuse
 * to start on older distributions to their original versions, which behave
 * the same here. With this the binary needs glibc 2.34 (Ubuntu 22.04,
 * Debian 12, Fedora 35). Force-included into every C file (CMakeLists.txt,
 * LAMBO_PORTABLE_GLIBC); strtol/strtoul are in glibc_compat.c. */
#ifndef LAMBO_GLIBC_COMPAT_H
#define LAMBO_GLIBC_COMPAT_H
/* No #include here: it would fix the feature macros (_GNU_SOURCE) before
 * the source file sets them. glibc is assumed on Linux; on musl configure
 * with -DLAMBO_PORTABLE_GLIBC=OFF. */
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
#endif
#endif
