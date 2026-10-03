/* Interface between oscwus.dll (32-bit PE, built with mingw) and oscusb.so
 * (64-bit Unix library, built with the host gcc), called through Wine's
 * unixlib mechanism (__wine_unix_call).
 *
 * Every struct must have the SAME memory layout in both compilers: pointers
 * travel as uint64_t, all fields have fixed widths and every size is a
 * multiple of 8 (mingw i386 aligns uint64_t to 8, Linux i386 to 4 - keeping
 * sizes a multiple of 8 removes the difference). Because of this, one
 * dispatch table serves both native 64-bit callers and WOW64 (32-bit) callers.
 */
#ifndef OSCUSB_UNIXLIB_H
#define OSCUSB_UNIXLIB_H

#include <stdint.h>

enum oscusb_funcs
{
    unix_usb_open,
    unix_usb_close,
    unix_usb_bulk,
    unix_usb_control,
    unix_funcs_count
};

struct usb_open_params
{
    int32_t result;         /* 0 = device open */
    uint32_t pad;
};

struct usb_bulk_params
{
    uint64_t buffer;
    uint32_t endpoint;      /* bit 7 is the direction: 0x8x = IN */
    uint32_t length;
    uint32_t timeout;       /* ms */
    uint32_t transferred;   /* bytes reported by libusb, even on error */
    int32_t result;         /* libusb return code */
    uint32_t pad;
};

struct usb_control_params
{
    uint64_t buffer;
    uint32_t request_type;
    uint32_t request;
    uint32_t value;
    uint32_t index;
    uint32_t length;
    uint32_t timeout;       /* ms */
    int32_t result;         /* >= 0 bytes transferred, < 0 libusb error */
    uint32_t pad;
};

_Static_assert(sizeof(struct usb_open_params) == 8, "layout");
_Static_assert(sizeof(struct usb_bulk_params) == 32, "layout");
_Static_assert(sizeof(struct usb_control_params) == 40, "layout");

#endif
