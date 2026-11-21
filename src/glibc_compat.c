/* glibc 2.38 headers send strtol/strtoul to __isoc23_strtol/__isoc23_strtoul
 * under _GNU_SOURCE, symbols older glibc lacks. Defined here on top of the
 * original functions (the C23 versions only add "0b" prefixes, which nothing
 * here parses). See glibc_compat.h. */
#if defined(__linux__) && defined(__x86_64__)
extern long lambo_old_strtol(const char *s, char **end, int base);
extern unsigned long lambo_old_strtoul(const char *s, char **end, int base);
__asm__(".symver lambo_old_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver lambo_old_strtoul,strtoul@GLIBC_2.2.5");

long __isoc23_strtol(const char *s, char **end, int base)
{
    return lambo_old_strtol(s, end, base);
}

unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{
    return lambo_old_strtoul(s, end, base);
}
#endif
