/* Debug log shared by oscwus.dll and oscsetup.dll.
 *
 * Enabled by default (this is the configuration everything was tested with).
 * Controlled by the OSCBRIDGE_LOG environment variable, read once at load:
 *   unset / empty   -> log to C:\oscbridge.log
 *   OSCBRIDGE_LOG=0 -> logging disabled
 *   OSCBRIDGE_LOG=X -> log to Windows path X
 */
#ifndef OSCBRIDGE_DEBUGLOG_H
#define OSCBRIDGE_DEBUGLOG_H

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_log_path[MAX_PATH] = "C:\\oscbridge.log";
static int g_log_enabled = 1;

/* Call from DLL_PROCESS_ATTACH. */
static inline void log_init(void)
{
    char value[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("OSCBRIDGE_LOG", value, sizeof(value));

    if (n == 0 || n >= sizeof(value))
        return;
    if (strcmp(value, "0") == 0)
        g_log_enabled = 0;
    else
        memcpy(g_log_path, value, n + 1);
}

static inline void log_write(const char *tag, const char *fmt, va_list ap)
{
    FILE *f;

    if (!g_log_enabled || !(f = fopen(g_log_path, "a")))
        return;
    fprintf(f, "[%s tid=%lu t=%lu] ", tag, (unsigned long)GetCurrentThreadId(),
            (unsigned long)GetTickCount());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fclose(f);
}

static inline void log_hexdump(const char *tag, const char *label, const unsigned char *buf,
                               unsigned int len)
{
    FILE *f;
    unsigned int i, n = len < 32 ? len : 32;

    if (!g_log_enabled || !(f = fopen(g_log_path, "a")))
        return;
    fprintf(f, "[%s tid=%lu t=%lu] %s (%u bytes): ", tag, (unsigned long)GetCurrentThreadId(),
            (unsigned long)GetTickCount(), label, len);
    for (i = 0; i < n; i++)
        fprintf(f, "%02x ", buf[i]);
    fputc('\n', f);
    fclose(f);
}

#endif
