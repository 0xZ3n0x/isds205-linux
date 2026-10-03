# ISDS205 USB notes

Device and WinUSB behavior that the bridge has to take into account. The notes
come from measurements with the device and the vendor software.

You do not need this document to *use* the bridge: the vendor software speaks
the device protocol itself and the bridge only forwards bytes. It explains why
the WinUSB emulation in `src/oscwus.c` and the libusb layer in
`src/oscusb_unix.c` behave the way they do.

## Contents

1. [Device and endpoints](#1-device-and-endpoints)
2. [Handshake](#2-handshake)
3. [Streaming](#3-streaming)
4. [Sample format](#4-sample-format)
5. [libusb behavior](#5-libusb-behavior)
6. [WinUSB contract](#6-winusb-contract)
7. [Device quirks](#7-device-quirks)

---

## 1. Device and endpoints

| | |
|---|---|
| VID / PID | `0xD4A2` / `0x5661` (the vendor driver lists PIDs `0x5660`–`0x5666`) |
| Interface | 0 |

| Endpoint | Direction | Max packet | Used for |
|---|---|---|---|
| `0x01` | OUT | 64 | commands |
| `0x81` | IN | 64 | command and handshake replies, keep-alive replies |
| `0x82` | IN | 512 | waveform data (the actual stream) |

**Small reads on `0x82` do not work.** For an IN request smaller than 512
bytes the device returns nothing (measured: 64 bytes → 0 bytes, 1024 bytes →
1024 bytes). The bridge therefore never splits `0x82` requests; it passes the
size the application asked for through unchanged.

---

## 2. Handshake

The application sends the 5-byte challenge `29 23 be 84 e1` on `0x01`. The
device answers with a 5-byte nonce on `0x81`, and the application then has to
send the challenge followed by that nonce.

`0x82` produces no data until the handshake is complete.

Under Wine, `usbwave.dll` reads the nonce but does not send the
challenge + nonce packet by itself in time. The bridge sends it once it has
seen the nonce (`inject_nonce_echo` in `src/oscwus.c`).

---

## 3. Streaming

Waveform data comes straight from `0x82`; starting a capture produces no
reply on `0x81`.

While streaming, the application sends a keep-alive on `0x01` now and then.
Apart from the keep-alive reply, `0x81` returns nothing during streaming. This
is normal: many `0x81` reads simply time out.

---

## 4. Sample format

Samples of both channels are interleaved, 8 bits per sample:

| Index | Channel |
|---|---|
| even | CH1 |
| odd | CH2 |

The bridge logs per-channel statistics for the first few blocks (`wave CH1/CH2`
lines in the debug log).

---

## 5. libusb behavior

`libusb_bulk_transfer` does not behave exactly like WinUSB.

**a) Partial transfers.** When N bytes are requested, the device may deliver
only part of them and the transfer ends with `LIBUSB_ERROR_TIMEOUT` and
`transferred > 0`. WinUSB would keep the request pending until it completes.

Fix: accumulate partial data instead of discarding it, and request the rest
until the buffer is full or a time budget runs out. Without this, large reads
(such as the 2 MB reads after a timebase change) never complete and the
application hangs.

**b) One request in flight per endpoint.** Two threads issuing IN transfers on
the same endpoint get their bytes interleaved. All `0x81` reads are therefore
serialized under one lock, which is shared with the `0x01` writes.

---

## 6. WinUSB contract

`usbwave.dll` relies on real WinUSB behaving in the ways below. Without 6.1 to
6.3 it received no data at all.

### 6.1 Overlapped I/O on `0x82`

`usbwave.dll` does not read `0x82` synchronously:

```c
WinUsb_ReadPipe(h, 0x82, buf, len, NULL, &ov);   /* LengthTransferred = NULL */
WaitForSingleObject(ov.hEvent, 2000);             /* waits for the event */
```

When `Overlapped` is not NULL, the emulation:

- returns immediately with `ERROR_IO_PENDING`,
- performs the read on a worker thread,
- signals `ov.hEvent` when done.

Without the event the application times out after 2 s and silently drops the
data; the screen stays empty.

### 6.2 Independent pipes

In real WinUSB each pipe has its own request queue: a long read on `0x82` does
not block `0x81` replies or `0x01` writes such as the keep-alive. With a single
global lock, a long data read blocked the keep-alive writer and streaming
stalled after a few blocks.

The emulation therefore uses **one lock per channel**: `0x82` has its own, and
`0x01` + `0x81` share one (see 5b). The same rule applies on the Unix side: the
device-handle lock is never held for the duration of a transfer.

### 6.3 Never write more than `BufferLength`

Writing past the application's buffer corrupts its stack. Related:
`WinUsb_QueryPipe` must write the 8-byte packed `WINUSB_PIPE_INFORMATION`
layout that MSVC uses, not mingw's 12-byte `sizeof`.

### 6.4 Retry budgets

| Pipe | Attempt timeout | Total budget |
|---|---|---|
| `0x82` | 500 ms | 30 s (`READ_BUDGET_DATA_MS`) |
| `0x81` | 50 ms | 600 ms (`READ_BUDGET_CTRL_MS`) |

- `0x82` waits long because the application issues the data read before the
  command that starts streaming.
- `0x81` is short because holding the control lock longer delays the
  application's handshake writes beyond its tolerance, and it drops the device.
- Between attempts the reader sleeps `READ_RETRY_GAP_MS` (10 ms) and releases
  the lock, so a waiting writer is not starved.

---

## 7. Device quirks

- **Interrupted sessions.** If the program is killed while streaming, the
  device can stop answering the first handshake packet (writes to `0x01` time
  out). Unplug the device and plug it back in. The bridge also drains stale
  `0x81` data when it opens the device.
- **Do not reset the port.** `libusb_reset_device` can lock up the device's
  firmware so that it disappears from the bus until it is replugged. The
  bridge never resets the port.
