/* oscwus.dll - WinUSB API replacement.
 *
 * The patched binaries import this DLL as "OSCWUS.DLL" instead of
 * "WINUSB.DLL" (see tools/patch_imports.py). It implements the WinUsb_*
 * calls the application makes and forwards the actual USB transfers through
 * Wine's unixlib mechanism (__wine_unix_call) to oscusb.so, a 64-bit library
 * running inside the same Wine process that uses libusb-1.0.
 *
 * Wine's own winusb.dll cannot be used: its WinUsb_Initialize is not
 * implemented. Only the functions the vendor software actually calls are
 * implemented; the rest are harmless stubs.
 */
#include <windows.h>
#include <winusb.h>
#include <math.h>
#include <stdlib.h>

#include "debuglog.h"
#include "oscusb_unixlib.h"

static void dbglog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("oscwus", fmt, ap);
    va_end(ap);
}

static void hexdump_log(const char *label, const unsigned char *buf, unsigned int len)
{
    log_hexdump("oscwus", label, buf, len);
}

/* ---------------- Per-pipe locking ----------------
 * In real WinUSB every pipe has its own URB queue; a long IN transfer on one
 * pipe does not block the others. With a single global lock, a 500 ms read on
 * 0x82 stalls the application's keep-alive writer and its 0x81 commands, and
 * streaming stops after a few blocks. So 0x82 (waveform data) gets its own
 * lock. */
#define CHAN_CTRL 0   /* 0x01 OUT writes + 0x81 IN reads (commands, keep-alive) */
#define CHAN_DATA 1   /* 0x82 IN - waveform data (long reads) */
#define CHAN_COUNT 2

static CRITICAL_SECTION g_lock[CHAN_COUNT];
static LONG g_lock_ready[CHAN_COUNT];

static int chan_for_ep(UCHAR ep)
{
    /* Only 0x82 gets its own channel. 0x81 is NOT split from 0x01: the
     * application reads both 1024-byte status replies and 64-byte identity
     * blocks from 0x81, and two concurrent IN transfers on the same endpoint
     * interleave the bytes and break usbwave.dll's state machine
     * ("open the device failed"). */
    return ep == 0x82 ? CHAN_DATA : CHAN_CTRL;
}

static void ensure_lock(int c)
{
    if (InterlockedCompareExchange(&g_lock_ready[c], 2, 2) != 2)
        Sleep(5); /* DllMain has not initialized the locks yet */
}

/* ---------------- Wine unixlib connection ----------------
 * oscusb.dll is a builtin-stamped carrier; when Wine loads it, it binds the
 * oscusb.so next to it (found through WINEDLLPATH). We get the handle with
 * NtQueryVirtualMemory(MemoryWineUnixFuncs) and call through ntdll's
 * __wine_unix_call_dispatcher - the same path as __wine_init_unix_call /
 * __wine_unix_call in Wine's winecrt0. Under WOW64, ntdll automatically
 * redirects the request to the __wine_unix_call_wow64_funcs table. */
#define MemoryWineUnixFuncs 1000

typedef LONG (WINAPI *unix_dispatcher_t)(unsigned long long handle, unsigned int code, void *args);
typedef LONG (WINAPI *nt_query_vm_t)(HANDLE, PVOID, int, PVOID, SIZE_T, SIZE_T *);

static unsigned long long g_unix_handle;
static unix_dispatcher_t volatile g_unix_dispatch;

/* Called under the CHAN_CTRL lock from the first WinUsb_Initialize. */
static int unix_init(void)
{
    HMODULE ntdll, lib;
    nt_query_vm_t qvm;
    void **pdisp;
    unsigned long long handle = 0;
    LONG status;

    if (g_unix_dispatch)
        return 1;

    ntdll = GetModuleHandleA("ntdll.dll");
    lib = LoadLibraryA("oscusb.dll");
    if (!ntdll || !lib)
    {
        dbglog("unix_init: could not load oscusb.dll (err=%lu)", (unsigned long)GetLastError());
        return 0;
    }
    qvm = (nt_query_vm_t)(void *)GetProcAddress(ntdll, "NtQueryVirtualMemory");
    pdisp = (void **)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");
    if (!qvm || !pdisp || !*pdisp)
    {
        dbglog("unix_init: ntdll has no unix call dispatcher (not running under Wine?)");
        return 0;
    }
    status = qvm(GetCurrentProcess(), (PVOID)lib, MemoryWineUnixFuncs, &handle, sizeof(handle), NULL);
    if (status || !handle)
    {
        dbglog("unix_init: oscusb.so not bound (status=0x%08lx) - WINEDLLPATH must point to "
               "the directory containing oscusb.dll and oscusb.so", (unsigned long)status);
        return 0;
    }
    g_unix_handle = handle;
    InterlockedExchangePointer((PVOID *)&g_unix_dispatch, *pdisp);
    dbglog("unix_init: oscusb.so bound, handle=0x%llx", handle);
    return 1;
}

static int unix_call(unsigned int code, void *args)
{
    unix_dispatcher_t disp = g_unix_dispatch;
    return disp && disp(g_unix_handle, code, args) == 0;
}

static int usb_open(void)
{
    struct usb_open_params p = {0};
    int ok;

    ensure_lock(CHAN_CTRL);
    EnterCriticalSection(&g_lock[CHAN_CTRL]);
    ok = unix_init() && unix_call(unix_usb_open, &p) && p.result == 0;
    LeaveCriticalSection(&g_lock[CHAN_CTRL]);
    return ok;
}

static void usb_close(void)
{
    struct usb_open_params p = {0};

    ensure_lock(CHAN_CTRL);
    EnterCriticalSection(&g_lock[CHAN_CTRL]);
    unix_call(unix_usb_close, &p);
    LeaveCriticalSection(&g_lock[CHAN_CTRL]);
}

/* Returns 1 if libusb completed the transfer without error. *out_transferred
 * is the byte count libusb reported even on error - WinUSB's URB semantics
 * never discard partial data, see bulk_in_until_data. The channel lock is held
 * for the whole transfer: requests on the same channel (0x01/0x81) must stay
 * serialized. */
static int usb_bulk(UCHAR ep, PUCHAR buf, ULONG len, unsigned int timeout,
                    unsigned int *out_transferred)
{
    struct usb_bulk_params p = {0};
    int c = chan_for_ep(ep);
    int ok;

    p.buffer = (uint64_t)(ULONG_PTR)buf;
    p.endpoint = ep;
    p.length = len;
    p.timeout = timeout;
    ensure_lock(c);
    EnterCriticalSection(&g_lock[c]);
    ok = unix_call(unix_usb_bulk, &p) && p.result == 0;
    LeaveCriticalSection(&g_lock[c]);
    if (out_transferred)
        *out_transferred = p.transferred;
    return ok;
}

static int usb_control(WINUSB_SETUP_PACKET setup, PUCHAR buf, ULONG len,
                       unsigned int timeout, unsigned int *out_transferred)
{
    struct usb_control_params p = {0};
    int ok;

    p.buffer = (uint64_t)(ULONG_PTR)buf;
    p.request_type = setup.RequestType;
    p.request = setup.Request;
    p.value = setup.Value;
    p.index = setup.Index;
    p.length = len;
    p.timeout = timeout;
    ensure_lock(CHAN_CTRL);
    EnterCriticalSection(&g_lock[CHAN_CTRL]);
    ok = unix_call(unix_usb_control, &p) && p.result >= 0;
    LeaveCriticalSection(&g_lock[CHAN_CTRL]);
    if (out_transferred)
        *out_transferred = ok ? (unsigned int)p.result : 0;
    return ok;
}

/* ---------------- Challenge/nonce echo injection ----------------
 *
 * The handshake as captured on real Windows:
 *   OUT 10 : ff ff ff ff ff 11 22 33 44 55     (sync)
 *   IN  10 : (echo)
 *   OUT  5 : 29 23 be 84 e1                    (challenge)
 *   IN   5 : <device nonce, different every session>
 *   OUT 10 : 29 23 be 84 e1 <nonce>            <-- usbwave.dll skips this under Wine
 *   IN  64+64+64+47 : identity / calibration blocks
 *
 * Under Wine, usbwave.dll reads the 5-byte nonce but never sends the
 * challenge+nonce packet; it writes a malformed 8-byte keep-alive
 * (00 00 fe 02 00 00 00 00 instead of Windows' 00 00 fe 00 00 00 02 00) and
 * loops on read timeouts forever, so the device never streams.
 *
 * The shim sends the missing packet on the application's behalf: after the
 * challenge is written, the first 5-byte IN reply is echoed back as
 * challenge+nonce. (Programs that do send it themselves, such as Logic.exe,
 * are unaffected: the injection only runs once, for the first 5-byte reply.)
 */
static const UCHAR kChallenge[5] = {0x29, 0x23, 0xbe, 0x84, 0xe1};
static UCHAR g_challenge[5];
static int g_have_challenge = 0;
static int g_nonce_echoed = 0;
static DWORD g_challenge_tick = 0;

#define CHALLENGE_WINDOW_MS 5000

static void note_challenge_written(const unsigned char *buf, unsigned int len)
{
    if (len == 5 && memcmp(buf, kChallenge, 5) == 0)
    {
        memcpy(g_challenge, buf, 5);
        g_have_challenge = 1;
        g_nonce_echoed = 0;
        g_challenge_tick = GetTickCount();
        dbglog("challenge seen, waiting for the 5-byte nonce");
    }
}

static void inject_nonce_echo(const unsigned char *nonce, unsigned int len)
{
    unsigned char combined[10];
    unsigned int outlen = 0;
    int ok;

    if (!g_have_challenge || g_nonce_echoed)
        return;
    if (len != 5)
        return;
    if (GetTickCount() - g_challenge_tick > CHALLENGE_WINDOW_MS)
    {
        dbglog("challenge window expired, nonce injection cancelled");
        g_have_challenge = 0;
        return;
    }

    memcpy(combined, g_challenge, 5);
    memcpy(combined + 5, nonce, 5);

    ok = usb_bulk(0x01, combined, 10, 5000, &outlen);
    g_nonce_echoed = 1;
    g_have_challenge = 0;
    dbglog("challenge+nonce injected: ok=%d data=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
           ok, combined[0], combined[1], combined[2], combined[3], combined[4],
           combined[5], combined[6], combined[7], combined[8], combined[9]);
}

/* Which code made this call? __builtin_return_address must be evaluated in the
 * exported function itself, so the address is passed in. With usbwave.dll's
 * preferred image base of 0x10000000, RVA + 0x10000000 is the
 * calling function's address. */
static void log_caller(const char *label, void *ret)
{
    void *base = GetModuleHandleA("usbwave.dll");
    dbglog("%s -> caller ret=%p usbwave_base=%p RVA=0x%06x", label, ret, base,
           base ? (unsigned int)((unsigned char *)ret - (unsigned char *)base) : 0u);
}

/* Per-channel statistics of a 0x82 waveform block. Samples are interleaved
 * (even index CH1, odd index CH2); useful to tell a real signal from a flat
 * line when the application shows nothing. */
static void log_wave_stats(const unsigned char *b, unsigned int n)
{
    double s[2] = {0.0, 0.0}, s2[2] = {0.0, 0.0};
    unsigned int mn[2] = {255, 255}, mx[2] = {0, 0}, cnt[2] = {0, 0};
    unsigned int i, c;

    for (i = 0; i < n; i++)
    {
        c = i & 1;
        s[c] += b[i];
        s2[c] += (double)b[i] * (double)b[i];
        cnt[c]++;
        if (b[i] < mn[c]) mn[c] = b[i];
        if (b[i] > mx[c]) mx[c] = b[i];
    }
    for (c = 0; c < 2; c++)
    {
        double mean = cnt[c] ? s[c] / cnt[c] : 0.0;
        double var = cnt[c] ? s2[c] / cnt[c] - mean * mean : 0.0;
        if (var < 0.0)
            var = 0.0;
        dbglog("wave CH%u: n=%u min=%u max=%u mean=%.1f std=%.1f", c + 1, cnt[c],
               mn[c], mx[c], mean, sqrt(var));
    }
}

/* ---------------- WinUSB API ---------------- */

#define FAKE_HANDLE ((WINUSB_INTERFACE_HANDLE)0x1a2b3c4d)

WINBOOL WINAPI My_WinUsb_Initialize(HANDLE DeviceHandle, PWINUSB_INTERFACE_HANDLE InterfaceHandle)
{
    dbglog("WinUsb_Initialize called, DeviceHandle=%p", DeviceHandle);
    if (!usb_open())
    {
        dbglog("WinUsb_Initialize: usb_open failed");
        return FALSE;
    }
    *InterfaceHandle = FAKE_HANDLE;
    dbglog("WinUsb_Initialize succeeded, InterfaceHandle=%p written to %p", FAKE_HANDLE,
           (void *)InterfaceHandle);
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_Free(WINUSB_INTERFACE_HANDLE InterfaceHandle)
{
    (void)InterfaceHandle;
    usb_close();
    return TRUE;
}

/* ---------------- ReadPipe timing ----------------
 * usbwave.dll issues the 0x82 (waveform) read only ONCE and does not retry it
 * if it fails. Streaming only starts after the application sends
 * its start-capture command once the channel is enabled - but the 0x82 read is
 * started before that command. A shim that gives up after a few seconds
 * returns before the command is ever sent.
 *
 * So 0x82 reads are retried for up to 30 s in 500 ms libusb attempts, and the
 * channel lock is released between attempts so that the writer thread can get
 * the command through; the device starts streaming and a later attempt gets
 * the data. 0x81 must not block: the application's handshake state machine
 * has its own short timeouts. Its patience while writing is ~200 ms (a 541 ms
 * delayed "aa" write triggered AbortPipe), so 0x81 uses 50 ms attempts; the
 * device answers a write within ~1 ms. 0x82's patience is ~14 s, so 500 ms
 * attempts are fine there. */
#define READ_BUDGET_DATA_MS  30000   /* 0x82: wait until streaming starts */
#define READ_BUDGET_CTRL_MS    600   /* 0x81: catch the reply in short windows */
#define READ_RETRY_GAP_MS       10   /* gap between attempts so a waiting writer gets the lock */

static volatile LONG g_abort_flag[256];

/* Runs one bulk IN request with retries; used by both the synchronous (0x81)
 * and the overlapped (0x82) paths.
 *
 * Important: libusb can return less data than requested together with a
 * TIMEOUT (asking for 2 MB, the device delivers ~1 MB and r=-7). WinUSB would
 * keep the URB pending and complete it with the remaining bytes. So partial
 * data is NEVER discarded: it is accumulated and the rest is requested until
 * the buffer is full or the budget runs out. */
static unsigned int bulk_in_until_data(UCHAR PipeID, char *Buffer, ULONG BufferLength,
                                       DWORD budget, unsigned int timeout)
{
    DWORD start = GetTickCount();
    unsigned int total = 0;
    int tries = 0;

    for (;;)
    {
        unsigned int got = 0;
        int ok;

        if (total >= BufferLength)
            return total;
        ok = usb_bulk(PipeID, (PUCHAR)Buffer + total, BufferLength - total, timeout, &got);
        if (got)
            total += got;
        if (total >= BufferLength)
            return total;
        if (ok)
            return total;   /* the device ended cleanly: a successful short read */
        tries++;
        if (InterlockedCompareExchange(&g_abort_flag[PipeID], 0, 0))
        {
            dbglog("ReadPipe PipeID=0x%02x aborted by AbortPipe (%u bytes)", PipeID, total);
            return total;
        }
        if (GetTickCount() - start > budget)
        {
            dbglog("ReadPipe PipeID=0x%02x budget exhausted (%lu ms, %d attempts, %u bytes)",
                   PipeID, (unsigned long)budget, tries, total);
            return total;
        }
        /* Without a gap this thread re-entered the lock immediately and
         * starved a waiting writer (e.g. the start-capture command or the sync command). */
        Sleep(READ_RETRY_GAP_MS);
    }
}

/* Completes an OVERLAPPED request on a separate thread. Like a real driver it
 * does not block the caller; signalling hEvent when done is all the
 * application waits for. */
typedef struct
{
    UCHAR  PipeID;
    PUCHAR Buffer;
    ULONG  Length;
    DWORD  budget;
    unsigned int timeout;
    HANDLE hEvent;
} OVL_REQ;

static DWORD WINAPI ovl_worker(LPVOID arg)
{
    OVL_REQ *r = (OVL_REQ *)arg;
    unsigned int outlen = bulk_in_until_data(r->PipeID, (char *)r->Buffer, r->Length,
                                             r->budget, r->timeout);
    int cancelled = InterlockedCompareExchange(&g_abort_flag[r->PipeID], 0, 0) != 0;

    if (r->PipeID == 0x82 && outlen)
    {
        static LONG wave_reads = 0;
        if (InterlockedIncrement(&wave_reads) <= 4)
        {
            hexdump_log("ReadPipe data", r->Buffer, outlen);
            log_wave_stats(r->Buffer, outlen);
        }
    }
    if (outlen)
        inject_nonce_echo((const unsigned char *)r->Buffer, outlen);

    dbglog("ReadPipe overlapped done: n=%u cancelled=%d hEvent=%p",
           outlen, cancelled, (void *)r->hEvent);
    if (r->hEvent)
        SetEvent(r->hEvent);
    free(r);
    return 0;
}

WINBOOL WINAPI My_WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID,
                                   PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred,
                                   LPOVERLAPPED Overlapped)
{
    DWORD budget = (PipeID == 0x82) ? READ_BUDGET_DATA_MS : READ_BUDGET_CTRL_MS;
    unsigned int timeout = (PipeID == 0x82) ? 500 : 50;
    unsigned int outlen;

    (void)InterfaceHandle;
    InterlockedExchange(&g_abort_flag[PipeID], 0);

    /* 0x82 requests are NOT split into 64-byte pieces: the device returns
     * nothing for IN requests smaller than 512 bytes (measured: 64 -> 0,
     * 1024 -> 1024). usbwave.dll reads 0x82 with its own large overlapped
     * buffers; 64-byte reads only happen on 0x81. */

    dbglog("ReadPipe called, PipeID=0x%02x len=%lu (retry budget=%lums overlapped=%p)",
           PipeID, (unsigned long)BufferLength, (unsigned long)budget, (void *)Overlapped);
    log_caller("ReadPipe", __builtin_return_address(0));

    /* OVERLAPPED I/O contract. usbwave.dll reads 0x82 asynchronously:
     *   WinUsb_ReadPipe(h, 0x82, buf, len, NULL, &ov);
     *   WaitForSingleObject(ov.hEvent, 2000);
     * It passes an OVERLAPPED instead of LengthTransferred and waits for
     * hEvent. Returning synchronously would block its reader thread for up to
     * 30 s and streaming would not progress; like a real driver we return
     * ERROR_IO_PENDING and finish the work on a worker thread. */
    if (Overlapped)
    {
        OVL_REQ *r = (OVL_REQ *)calloc(1, sizeof *r);
        HANDLE th;

        if (r)
        {
            r->PipeID = PipeID;
            r->Buffer = Buffer;
            r->Length = BufferLength;
            r->budget = budget;
            r->timeout = timeout;
            r->hEvent = Overlapped->hEvent;
            th = CreateThread(NULL, 0, ovl_worker, r, 0, NULL);
            if (th)
            {
                CloseHandle(th);
                SetLastError(ERROR_IO_PENDING);
                return FALSE;
            }
            free(r);
        }
    }

    outlen = bulk_in_until_data(PipeID, (char *)Buffer, BufferLength, budget, timeout);
    if (LengthTransferred)
        *LengthTransferred = outlen;
    dbglog("ReadPipe synchronous result: outlen=%u", outlen);
    if (outlen)
    {
        hexdump_log("ReadPipe data", Buffer, outlen);
        if (PipeID == 0x82)
            log_wave_stats(Buffer, outlen);
        inject_nonce_echo((const unsigned char *)Buffer, outlen);
    }
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID,
                                    PUCHAR Buffer, ULONG BufferLength, PULONG LengthTransferred,
                                    LPOVERLAPPED Overlapped)
{
    unsigned int transferred = 0;
    int ok;

    (void)InterfaceHandle;
    (void)Overlapped;
    dbglog("WritePipe called, PipeID=0x%02x len=%lu", PipeID, (unsigned long)BufferLength);
    log_caller("WritePipe", __builtin_return_address(0));
    if (BufferLength)
        hexdump_log("WritePipe data", Buffer, BufferLength);
    if (PipeID == 0x01 && BufferLength)
        note_challenge_written(Buffer, BufferLength);

    ok = usb_bulk(PipeID, Buffer, BufferLength, 5000, &transferred);
    if (LengthTransferred)
        *LengthTransferred = ok ? transferred : 0;
    dbglog("WritePipe result: ok=%d", ok);
    if (!ok)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE InterfaceHandle,
                                          WINUSB_SETUP_PACKET SetupPacket, PUCHAR Buffer,
                                          ULONG BufferLength, PULONG LengthTransferred,
                                          LPOVERLAPPED Overlapped)
{
    int is_in = (SetupPacket.RequestType & 0x80) != 0;
    unsigned int outlen = 0;
    int ok;

    (void)InterfaceHandle;
    (void)Overlapped;
    dbglog("ControlTransfer called, reqtype=0x%02x req=0x%02x value=%u index=%u len=%lu",
           SetupPacket.RequestType, SetupPacket.Request, SetupPacket.Value, SetupPacket.Index,
           (unsigned long)BufferLength);

    ok = usb_control(SetupPacket, Buffer, BufferLength, 5000, &outlen);
    if (LengthTransferred)
        *LengthTransferred = is_in ? outlen : BufferLength;
    if (!ok)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE InterfaceHandle,
                                                 UCHAR AlternateInterfaceNumber,
                                                 PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor)
{
    (void)InterfaceHandle;
    dbglog("QueryInterfaceSettings called, alt=%u, sizeof(struct)=%u",
           AlternateInterfaceNumber, (unsigned)sizeof(*UsbAltInterfaceDescriptor));
    memset(UsbAltInterfaceDescriptor, 0, sizeof(*UsbAltInterfaceDescriptor));
    UsbAltInterfaceDescriptor->bLength = sizeof(USB_INTERFACE_DESCRIPTOR);
    UsbAltInterfaceDescriptor->bDescriptorType = 0x04;
    UsbAltInterfaceDescriptor->bInterfaceNumber = 0;
    UsbAltInterfaceDescriptor->bAlternateSetting = 0;
    UsbAltInterfaceDescriptor->bNumEndpoints = 3;
    UsbAltInterfaceDescriptor->bInterfaceClass = 0xFF;
    UsbAltInterfaceDescriptor->bInterfaceSubClass = 0;
    UsbAltInterfaceDescriptor->bInterfaceProtocol = 0;
    UsbAltInterfaceDescriptor->iInterface = 0;
    return TRUE;
}

/* usbwave.dll (built with MSVC) reads WINUSB_PIPE_INFORMATION as exactly
 * 8 bytes (the caller's adjacent local starts 8 bytes after it), but
 * mingw's natural alignment makes the struct 12 bytes (sizeof == 12, verified
 * in a live log). Writing sizeof() bytes would overrun the caller's 8-byte
 * stack slot and clobber the adjacent local, so the packed 8-byte layout is
 * written by hand. */
WINBOOL WINAPI My_WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE InterfaceHandle,
                                    UCHAR AlternateInterfaceNumber, UCHAR PipeIndex,
                                    PWINUSB_PIPE_INFORMATION PipeInformation)
{
    /* The real device (lsusb -v -d d4a2:5661): EP1 OUT 0x01 = 64 bytes,
     * EP1 IN 0x81 = 64 bytes, EP2 IN 0x82 = 512 bytes. */
    static const UCHAR  pipe_ids[3] = {0x81, 0x82, 0x01};
    static const USHORT pipe_mps[3] = {64, 512, 64};
    unsigned char raw[8];

    (void)InterfaceHandle;
    (void)AlternateInterfaceNumber;
    dbglog("QueryPipe called, index=%u", PipeIndex);
    if (PipeIndex >= 3)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    memset(raw, 0, sizeof(raw));
    *(UINT32 *)&raw[0] = (UINT32)UsbdPipeTypeBulk;  /* offset 0: PipeType (4 bytes) */
    raw[4] = pipe_ids[PipeIndex];                    /* offset 4: PipeId (1 byte) */
    *(USHORT *)&raw[5] = pipe_mps[PipeIndex];        /* offset 5: MaximumPacketSize (2 bytes, unaligned) */
    raw[7] = 0;                                      /* offset 7: Interval (1 byte) */
    memcpy(PipeInformation, raw, 8);
    dbglog("QueryPipe returned (8-byte packed): PipeType=%d PipeId=0x%02x MaxPacketSize=%u",
           (int)UsbdPipeTypeBulk, pipe_ids[PipeIndex], pipe_mps[PipeIndex]);
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE InterfaceHandle,
                                                 ULONG InformationType, PULONG BufferLength,
                                                 PVOID Buffer)
{
    (void)InterfaceHandle;
    dbglog("QueryDeviceInformation called, type=%lu", (unsigned long)InformationType);
    if (InformationType == 1 /* DEVICE_SPEED */)
    {
        *(PUCHAR)Buffer = 3; /* UsbHighSpeed */
        *BufferLength = 1;
    }
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE h, UCHAR PipeID)
{
    (void)h;
    InterlockedExchange(&g_abort_flag[PipeID], 1);
    dbglog("AbortPipe PipeID=0x%02x", PipeID);
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE h, UCHAR PipeID)
{
    (void)h;
    dbglog("ResetPipe PipeID=0x%02x", PipeID);
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_FlushPipe(WINUSB_INTERFACE_HANDLE h, UCHAR PipeID)
{
    (void)h;
    dbglog("FlushPipe PipeID=0x%02x", PipeID);
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_ResetDevice(WINUSB_INTERFACE_HANDLE h)
{
    (void)h;
    dbglog("ResetDevice");
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE h, UCHAR PipeID, ULONG PolicyType,
                                        ULONG ValueLength, PVOID Value)
{
    (void)h; (void)PipeID; (void)PolicyType; (void)ValueLength; (void)Value;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE h, UCHAR PipeID, ULONG PolicyType,
                                        PULONG ValueLength, PVOID Value)
{
    (void)h; (void)PipeID; (void)PolicyType; (void)Value;
    if (ValueLength)
        *ValueLength = 0;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE h, UCHAR idx,
                                                 PWINUSB_INTERFACE_HANDLE out)
{
    (void)h; (void)idx;
    *out = FAKE_HANDLE;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE h, UCHAR DescriptorType, UCHAR Index,
                                       USHORT LanguageID, PUCHAR Buffer, ULONG BufferLength,
                                       PULONG LengthTransferred)
{
    (void)h; (void)DescriptorType; (void)Index; (void)LanguageID; (void)Buffer; (void)BufferLength;
    if (LengthTransferred)
        *LengthTransferred = 0;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE h, UCHAR SettingNumber)
{
    (void)h; (void)SettingNumber;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE h, PUCHAR SettingNumber)
{
    (void)h;
    if (SettingNumber)
        *SettingNumber = 0;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_GetPowerPolicy(WINUSB_INTERFACE_HANDLE h, ULONG PolicyType,
                                         PULONG ValueLength, PVOID Value)
{
    (void)h; (void)PolicyType; (void)Value;
    if (ValueLength)
        *ValueLength = 0;
    return TRUE;
}

WINBOOL WINAPI My_WinUsb_SetPowerPolicy(WINUSB_INTERFACE_HANDLE h, ULONG PolicyType,
                                         ULONG ValueLength, PVOID Value)
{
    (void)h; (void)PolicyType; (void)ValueLength; (void)Value;
    return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        /* The locks are set up here; unix_init happens later, in the first
         * WinUsb_Initialize, because calling LoadLibrary inside DllMain is not
         * safe. */
        int c;

        log_init();
        for (c = 0; c < CHAN_COUNT; c++)
        {
            InitializeCriticalSection(&g_lock[c]);
            g_lock_ready[c] = 2;
        }
    }
    return TRUE;
}
