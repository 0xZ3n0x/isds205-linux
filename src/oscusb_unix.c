/* oscusb.so - the Unix side of oscwus.dll (a Wine unixlib).
 *
 * Runs as native 64-bit code INSIDE the Wine process and talks to the device
 * directly through libusb-1.0. oscwus.dll (32-bit PE) calls the functions in
 * the tables at the bottom of this file through __wine_unix_call; there is no
 * separate process, socket or serialization in between.
 *
 * Wine finds this file next to the builtin-stamped carrier oscusb.dll, in a
 * directory listed in WINEDLLPATH.
 */
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oscusb_unixlib.h"

typedef int NTSTATUS;
typedef NTSTATUS (*unixlib_entry_t)(void *args);

#define VID 0xD4A2
static const uint16_t PIDS[] = {0x5660, 0x5661, 0x5662, 0x5663, 0x5664, 0x5665, 0x5666};
#define NUM_PIDS (sizeof(PIDS) / sizeof(PIDS[0]))

static libusb_context *g_ctx;
static libusb_device_handle *g_handle;

/* Transfers take the read lock, so the 0x82 data pipe and the 0x81/0x01
 * control pipes can run in parallel (libusb allows concurrent transfers on
 * different endpoints of one handle). Open/close take the write lock, so the
 * handle can never be closed under a running transfer. Writer preference keeps
 * a close from starving behind continuously streaming reads. */
static pthread_rwlock_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static void init_lock(void)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&g_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
}

/* usbwave.dll's handshake state machine loses sync when the first bytes it
 * reads are unexpected. Drain anything a previous session left in the 0x81
 * FIFO before the application starts its handshake. */
static void drain_stale_data(libusb_device_handle *h)
{
    unsigned char buf[1024];
    int transferred, drained = 0, i;

    for (i = 0; i < 10; i++)
    {
        int r = libusb_bulk_transfer(h, 0x81, buf, sizeof(buf), &transferred, 50);
        if (r != 0 || transferred == 0)
            break;
        drained += transferred;
    }
    if (drained)
        fprintf(stderr, "[oscusb] drained %d stale bytes\n", drained);
}

static libusb_device_handle *open_device(void)
{
    int attempt;

    for (attempt = 0; attempt < 20; attempt++)
    {
        size_t i;
        for (i = 0; i < NUM_PIDS; i++)
        {
            libusb_device_handle *h = libusb_open_device_with_vid_pid(g_ctx, VID, PIDS[i]);
            int r;

            if (!h)
                continue;
            libusb_set_auto_detach_kernel_driver(h, 1);
            r = libusb_claim_interface(h, 0);
            if (r == 0)
            {
                fprintf(stderr, "[oscusb] device opened: VID=%04x PID=%04x\n", VID, PIDS[i]);
                drain_stale_data(h);
                return h;
            }
            fprintf(stderr, "[oscusb] claim_interface failed: PID=%04x r=%d "
                    "(-6 = BUSY: another process holds the device)\n", PIDS[i], r);
            libusb_close(h);
        }
        usleep(500 * 1000);
    }
    fprintf(stderr, "[oscusb] ERROR: could not open/claim the device within 10 seconds\n");
    return NULL;
}

static NTSTATUS usb_open(void *args)
{
    struct usb_open_params *p = args;

    pthread_once(&g_lock_once, init_lock);
    pthread_rwlock_wrlock(&g_lock);
    if (!g_ctx && libusb_init(&g_ctx) != 0)
    {
        fprintf(stderr, "[oscusb] libusb_init failed\n");
        g_ctx = NULL;
    }
    if (g_ctx && !g_handle)
        g_handle = open_device();
    p->result = g_handle ? 0 : -1;
    pthread_rwlock_unlock(&g_lock);
    return 0;
}

static NTSTATUS usb_close(void *args)
{
    (void)args;
    pthread_once(&g_lock_once, init_lock);
    pthread_rwlock_wrlock(&g_lock);
    if (g_handle)
    {
        libusb_close(g_handle);
        g_handle = NULL;
    }
    pthread_rwlock_unlock(&g_lock);
    return 0;
}

static NTSTATUS usb_bulk(void *args)
{
    struct usb_bulk_params *p = args;
    int transferred = 0;

    pthread_once(&g_lock_once, init_lock);
    pthread_rwlock_rdlock(&g_lock);
    p->result = g_handle
        ? libusb_bulk_transfer(g_handle, (unsigned char)p->endpoint,
                               (unsigned char *)(uintptr_t)p->buffer, (int)p->length,
                               &transferred, p->timeout)
        : LIBUSB_ERROR_NO_DEVICE;
    pthread_rwlock_unlock(&g_lock);
    p->transferred = (uint32_t)transferred;
    return 0;
}

static NTSTATUS usb_control(void *args)
{
    struct usb_control_params *p = args;

    pthread_once(&g_lock_once, init_lock);
    pthread_rwlock_rdlock(&g_lock);
    p->result = g_handle
        ? libusb_control_transfer(g_handle, (uint8_t)p->request_type, (uint8_t)p->request,
                                  (uint16_t)p->value, (uint16_t)p->index,
                                  (unsigned char *)(uintptr_t)p->buffer, (uint16_t)p->length,
                                  p->timeout)
        : LIBUSB_ERROR_NO_DEVICE;
    pthread_rwlock_unlock(&g_lock);
    return 0;
}

/* The argument structs have the same layout for 32- and 64-bit callers (see
 * oscusb_unixlib.h), so WOW64 callers (the 32-bit application) need no
 * separate pointer-widening thunks: both tables are identical. */
__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_funcs[] =
{
    usb_open,
    usb_close,
    usb_bulk,
    usb_control,
};

__attribute__((visibility("default")))
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    usb_open,
    usb_close,
    usb_bulk,
    usb_control,
};

_Static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == unix_funcs_count,
               "dispatch table");
