# ISDS205 Wine bridge

Run the Windows-only software of the **InstruStar ISDS205** USB oscilloscope
("Multi VirAnalyzer", vendor directory `Vimu Electronic`) on Linux under Wine,
talking to the **real device** plugged into the Linux machine.

Wine cannot do this on its own: its `winusb.dll` does not implement
`WinUsb_Initialize` and friends, and Wine has no driver that would make the
device visible to Windows programs. This project supplies the missing piece:

- two small replacement DLLs that emulate the parts of SetupAPI and WinUSB the
  vendor software uses,
- a 64-bit Linux library that performs the USB transfers with **libusb**,
  loaded **inside the Wine process** through Wine's own "unixlib" mechanism
  (no daemon, no sockets, no extra processes),
- a tiny patch to the vendor's `usbwave.dll` (two DLL names in its import
  table) so that everything lives in the program's own directory.

No data is faked: every byte on screen comes from the device.

> This project is not affiliated with InstruStar. The vendor software is not
> included; you need your own copy.

---

## Quick start

You need your own copy of the vendor installer, Wine in wow64 mode and the
build tools listed under [Requirements](#requirements).

```sh
# 1. Install the vendor software into a dedicated Wine prefix
export WINEPREFIX=~/.wine-osc
wineboot --init
wine "English Version(3.13.2.0).exe" /quiet /norestart   # the vendor installer
winetricks -q vcrun2019

# 2. Build the bridge and install it into the program directory
make
make install WINEPREFIX=~/.wine-osc

# 3. Plug in the device and start the oscilloscope program
make run WINEPREFIX=~/.wine-osc
```

Then enable CH1 in the program to start the display. If something goes wrong,
see [Troubleshooting](#troubleshooting). `make uninstall WINEPREFIX=~/.wine-osc`
restores the original files.

If opening the device fails with a permission error, add a udev rule first
(see [USB permissions](#usb-permissions)).

---

## Contents

- [Quick start](#quick-start)
- [Status](#status)
- [Which programs work](#which-programs-work)
- [Requirements](#requirements)
- [Installation](#installation)
- [Usage](#usage)
- [Uninstalling](#uninstalling)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [Repository layout](#repository-layout)
- [Limitations](#limitations)
- [License](#license)

---

## Status

Working: the oscilloscope program performs the device handshake and streams
waveform data continuously.

Tested with:

| | |
|---|---|
| Device | ISDS205A (reports itself as `ISDS205A(1.1)`), USB ID `d4a2:5661` |
| Vendor software | Multi VirAnalyzer, English version 3.13.2.0 (`usbwave.dll` dated 2022-05-17) |
| Wine | 11.0 (Staging), "wine-wow64" build (single 64-bit `wine` running 32-bit programs via WOW64) |
| OS | Fedora 43, x86_64, libusb 1.0.30 |

Other ISDS205 variants (B/C/X) and the other models covered by the same
software use the same USB driver and will probably work, but only the
ISDS205A has been tried.

---

## Which programs work

The vendor package contains several programs. They reach the device in one of
two ways:

1. **Through `usbwave.dll`** (the vendor's USB library). Patching
   `usbwave.dll` — the default — is enough for all of them.
2. **With their own embedded copy of the USB code.** These import
   `WINUSB.DLL` / `SETUPAPI.dll` themselves and have to be patched
   individually (see [Patching other programs](#patching-other-programs)).

| Program | What it is | USB path | Status |
|---|---|---|---|
| `ProfessionalVersion.exe` | Oscilloscope (professional UI); also contains the spectrum analyzer (FFT "Spectrum View") | `usbwave.dll` | **Works** (tested) |
| `devmng.exe` | Device manager | `usbwave.dll` | **Works** (tested) |
| `VmMso.exe` | – | `usbwave.dll` | Not tested; uses the same path, so it should work |
| `Recorder.exe` | Data recorder (long recordings of both channels to disk) | embedded | Connects and receives data (tested); a complete recording session was not verified |
| `Logic.exe` | Logic analyzer | embedded | Connects and completes the handshake, then ignores an ISDS205A — expected, the logic analyzer exists only on the ISDS205C/205X. Not tested with those models |
| `Sweep.exe` | Frequency sweep (needs the DDS output of the ISDS205B/205X) | embedded | Not tested |

In short: **patch `usbwave.dll` and the oscilloscope, spectrum analyzer and
device manager work. The same patch can be applied to the other programs;
Recorder.exe has been seen talking to the device, the rest have not been
tested.**

Only `usbwave.dll` was patched and tested, on an ISDS205A. The other programs
in the table and the other ISDS205 models were not tested. If you want to try
them, you can patch them as described in
[Patching other programs](#patching-other-programs); `make uninstall` restores
the originals. Use at your own risk.

According to the vendor's ISDS205 manual, features by model are:

| Model | Oscilloscope | Spectrum analyzer | Data recorder | Logic analyzer | DDS |
|---|---|---|---|---|---|
| ISDS205A | ✓ | ✓ | ✓ | – | – |
| ISDS205B | ✓ | ✓ | ✓ | – | ✓ |
| ISDS205C | ✓ | ✓ | ✓ | ✓ | – |
| ISDS205X | ✓ | ✓ | ✓ | ✓ | ✓ |

---

## Requirements

Build tools:

- `i686-w64-mingw32-gcc` (32-bit mingw-w64 cross compiler)
- `gcc`, `make`, `pkg-config`
- libusb-1.0 development files (64-bit)
- `python3` (for the patch and signing helpers)

Fedora:

```sh
sudo dnf install mingw32-gcc gcc make pkgconf-pkg-config libusb1-devel python3
```

Debian/Ubuntu (package names, not tested):

```sh
sudo apt install gcc-mingw-w64-i686 gcc make pkg-config libusb-1.0-0-dev python3
```

Runtime:

- Wine in **wow64 mode** (see [Limitations](#limitations)); `winetricks` to
  install the Visual C++ runtime.
- Access to the USB device for your user (see
  [USB permissions](#usb-permissions)).

`winebuild`/`wine-devel` is **not** needed: the "Wine builtin DLL" signature
is written by `tools/stamp_builtin.py`.

---

## Installation

### 1. Install the vendor software into a Wine prefix

Use a dedicated prefix (the examples use `~/.wine-osc`):

```sh
export WINEPREFIX=~/.wine-osc
wineboot --init
wine "English Version(3.13.2.0).exe" /quiet /norestart   # the vendor installer
winetricks -q vcrun2019                                  # MSVC 2015-2022 runtime
```

The silent install can skip the Visual C++ runtime; without it the programs do
not start (`c0000135`). The program ends up in
`$WINEPREFIX/drive_c/Program Files/Vimu Electronic/Multi VirAnalyzer/`.

The vendor's Windows USB driver is **not** needed and is not used.

### 2. Build and install the bridge

```sh
make
make install WINEPREFIX=~/.wine-osc
```

`make install`:

- copies `oscwus.dll`, `oscsetup.dll`, `oscusb.dll` and `oscusb.so` into the
  program directory,
- patches `usbwave.dll` (the original is kept as `usbwave.dll.orig`).

It changes nothing outside the program directory: no `system32`/`syswow64`
files, no registry, no DLL overrides.

If the software is installed somewhere else inside the prefix, pass
`APPDIR=/path/to/Multi\ VirAnalyzer` as well.

### USB permissions

libusb needs write access to the device node. If opening the device fails with
an access error (`-3`), add a udev rule, e.g.
`/etc/udev/rules.d/70-isds205.rules`:

```
SUBSYSTEM=="usb", ATTR{idVendor}=="d4a2", MODE="0660", TAG+="uaccess"
```

then `sudo udevadm control --reload && sudo udevadm trigger` and replug the
device.

---

## Usage

```sh
make run WINEPREFIX=~/.wine-osc                           # ProfessionalVersion.exe
make run WINEPREFIX=~/.wine-osc APP=devmng.exe
# or directly:
WINEPREFIX=~/.wine-osc scripts/run.sh ProfessionalVersion.exe
```

Always start the programs through `make run` / `scripts/run.sh`: it sets
`WINEDLLPATH` to the program directory, which Wine needs to find
`oscusb.so`. Started any other way, the program opens but does not see the
device.

In ProfessionalVersion.exe, enable CH1 to start the display. The spectrum
analyzer is the "Spectrum View" of the same program (FFT1/FFT2 settings in
the "waveform process" tab).

**Close the programs from their own window.** Killing a program while it is
streaming can leave the device in a state where it no longer answers; it then
has to be unplugged and plugged back in.

### Patching other programs

`make install` patches `usbwave.dll` only. To patch the programs with
embedded USB code as well:

```sh
make install WINEPREFIX=~/.wine-osc PATCH="usbwave.dll Recorder.exe Logic.exe Sweep.exe"
```

or use the tool directly on any file:

```sh
tools/patch_imports.py --check  "$APPDIR"/usbwave.dll "$APPDIR"/Recorder.exe
tools/patch_imports.py          "$APPDIR"/Recorder.exe
tools/patch_imports.py --restore "$APPDIR"/Recorder.exe
```

`make check` shows the state of every known file. The tool only rewrites the
two DLL names in the import table, keeps `<file>.orig` on first use and does
nothing if the file is already patched. It works on any 32-bit program that
imports `SETUPAPI.dll` and/or `WINUSB.DLL` in the same way; `oscsetup.dll`
automatically hooks the device open in whichever module imports it.

### Environment variables

| Variable | Effect |
|---|---|
| `WINEPREFIX` | Wine prefix (used by `make` and `scripts/run.sh`) |
| `APPDIR` | Program directory, if not in the standard location |
| `OSCBRIDGE_LOG` | Debug log of the two DLLs. Default: `C:\oscbridge.log` (i.e. `$WINEPREFIX/drive_c/oscbridge.log`). `0` disables it; any other value is used as a Windows path. |
| `WINEDEBUG` | Passed to Wine; `scripts/run.sh` defaults it to `-all` |

`oscusb.so` prints `[oscusb] …` lines (device open, claim errors) to the
program's standard error.

---

## Uninstalling

```sh
make uninstall WINEPREFIX=~/.wine-osc
```

Restores every patched file from its `.orig` backup (the backups are kept)
and removes the bridge files. Without the bridge the programs start but do
not see the device (Wine's own `winusb.dll` reports `WinUsb_Initialize` as not
implemented).

---

## Troubleshooting

Check the debug log first (`$WINEPREFIX/drive_c/oscbridge.log`):

| Log shows | Meaning / fix |
|---|---|
| No log at all | The patched file did not load our DLLs. Run `make check`; make sure you started the patched program. |
| `unix_init: could not load oscusb.dll` | `oscusb.dll` is missing from the program directory — `make install`. |
| `unix_init: oscusb.so not bound` | `WINEDLLPATH` does not contain the program directory, or `oscusb.so` is missing. Start the program with `make run` / `scripts/run.sh`. |
| `WinUsb_Initialize: usb_open failed` and `[oscusb] claim_interface failed … r=-6` | Another process holds the device (another Wine instance, sigrok/PulseView, …). |
| `[oscusb] … r=-3` | No permission for the USB device — see [USB permissions](#usb-permissions). |
| `[oscusb] ERROR: could not open/claim the device` | Device not found: check `lsusb` for `d4a2:5661`. |
| Handshake writes time out (`WritePipe result: ok=0` right after the sync packet) | The device is stuck after an interrupted session. Unplug it, wait a few seconds, plug it back in. |

---

## How it works

### The problem

The vendor software (32-bit, MSVC) talks to the device through the WinUSB
API:

```
ProfessionalVersion.exe → usbwave.dll → SETUPAPI.dll  (find the device)
                                      → CreateFileA    (open its path)
                                      → WINUSB.DLL     (WinUsb_ReadPipe/WritePipe/...)
```

Under Wine:

- SetupAPI finds nothing — there is no Windows driver for the device, so no
  device interface is registered.
- Wine's user-mode `winusb.dll` does not implement `WinUsb_Initialize`
  ("unimplemented function"), so even with a handle nothing could be read.
  (Wine's `wineusb.sys` exposes USB devices to *kernel drivers*, not to the
  user-mode WinUSB API.)

### Architecture

```
ProfessionalVersion.exe  (32-bit, WOW64)
   │
usbwave.dll              imports patched: OSCSETUP.dll, OSCWUS.DLL
   │
   ├── oscsetup.dll      fake SetupDi* enumeration + CreateFileA hook
   └── oscwus.dll        WinUSB emulation (32-bit PE)
          │  __wine_unix_call   (Wine's PE → Unix transition, same process)
          ▼
       oscusb.so         64-bit Linux library, libusb-1.0
          │
          ▼
     ISDS205 (USB d4a2:5661)
```

Files in the program directory:

| File | Built from | Role |
|---|---|---|
| `usbwave.dll` | vendor, patched | import names changed to `OSCSETUP.dll` / `OSCWUS.DLL` |
| `oscsetup.dll` | `src/oscsetup.c` | SetupAPI replacement |
| `oscwus.dll` | `src/oscwus.c` | WinUSB replacement |
| `oscusb.dll` | `src/oscusb_pe.c` | empty "builtin" carrier that Wine pairs with `oscusb.so` |
| `oscusb.so` | `src/oscusb_unix.c` | the USB transfers (libusb) |

### 1. Import-name patch (`tools/patch_imports.py`)

`usbwave.dll` imports `WINUSB.DLL` and `SETUPAPI.dll` **statically**. Simply
dropping replacement DLLs with those names into the program directory does
not work:

- `setupapi` is a **KnownDLL** (listed under
  `HKLM\System\CurrentControlSet\Control\Session Manager\KnownDLLs` in the
  prefix). Known DLLs are never loaded from the application directory, only
  from the system directory — replacing it would mean replacing
  `syswow64/setupapi.dll` for the whole prefix.
- For `winusb`, Wine prefers its own builtin copy unless forced with
  `WINEDLLOVERRIDES`.

The patch renames the two imports to names of the same length that Wine does
not know and that are not KnownDLLs:

| Original | Patched | Loaded file |
|---|---|---|
| `SETUPAPI.dll` | `OSCSETUP.dll` | `oscsetup.dll` |
| `WINUSB.DLL` | `OSCWUS.DLL` | `oscwus.dll` |

Wine then loads them from the program directory like any other DLL. The tool
parses the PE headers, walks the import descriptors, converts each name's RVA
to a file offset through the section table and overwrites the string in
place: 14 bytes change in `usbwave.dll`. The imports are not bound, and DLL
checksums are not verified by the loader, so nothing else needs updating.

### 2. `oscsetup.dll` — device enumeration

Implements exactly the 7 `SetupDi*` functions the software imports and
reports one device:

- `SetupDiGetClassDevsA` answers only for the device-interface GUID of PID
  `0x5661` (`{7c49cbb1-780e-4603-ae4c-0b18b8e6d8b9}`, from the vendor's
  `Dso.inf`). Reporting all seven GUIDs of the driver made the application
  detect several device types at once and it never opened any.
- The device path is `AAAAUSB\VID_D4A2&PID_5661#0#WINEBRIDGE_FAKE_DEV`.
  usbwave.dll lowercases it, takes characters 4–24, replaces `#` by `\`,
  uppercases the result and compares it with `USB\VID_D4A2&PID_5661` to
  identify the model — so those characters must be exactly that.
- usbwave.dll then calls `CreateFileA` on that path, which would fail. In its
  `DllMain`, oscsetup.dll patches the **`CreateFileA` entry of the import
  table of the module that imports it** (usbwave.dll, or the main exe for
  programs with embedded USB code). The hook returns a dummy handle for paths
  containing `WINEBRIDGE_FAKE_DEV` (case-insensitive — usbwave passes the
  lowercased path) and forwards everything else to the real `CreateFileA`.
  kernel32 itself is not modified. Import tables are filled before any
  `DllMain` runs, so the patch is in place before the first call.

### 3. `oscwus.dll` — WinUSB emulation

Implements the 19 `WinUsb_*` exports; the ones that matter are
`Initialize`, `ReadPipe`, `WritePipe`, `QueryPipe`, `AbortPipe`. It reproduces
the behavior the software relies on (details and measurements in
[docs/PROTOCOL.md](docs/PROTOCOL.md)):

- **Overlapped reads.** usbwave.dll reads the data pipe `0x82` with an
  `OVERLAPPED` and waits on its event. The emulation returns
  `ERROR_IO_PENDING`, does the read on a worker thread and signals the event.
- **Independent pipes.** One lock for the data pipe `0x82`, one shared by
  `0x01` (commands) and `0x81` (replies) — two concurrent reads on the same
  endpoint interleave bytes.
- **Partial data is never dropped.** libusb may return part of a large read
  together with a timeout; the emulation accumulates and asks for the rest,
  like a pending WinUSB request would.
- **Retry budgets** tuned to the application's own timeouts: `0x82` waits up
  to 30 s in 500 ms attempts (the data read is issued before the command that
  starts streaming), `0x81` up to 600 ms in 50 ms attempts; the lock is
  released for 10 ms between attempts so a waiting writer gets through.
- **Handshake fix.** Under Wine, usbwave.dll reads the device's nonce but does
  not send the challenge+nonce packet in time and loops forever. After seeing
  the challenge written, the emulation sends that packet itself.
- **`WinUsb_QueryPipe`** writes MSVC's packed 8-byte `WINUSB_PIPE_INFORMATION`
  by hand; mingw's struct is 12 bytes and would overwrite the caller's stack.

### 4. `oscusb.dll` + `oscusb.so` — the Wine unixlib

A 32-bit Windows DLL cannot call a Linux library directly. In older Wine the
answer was a "Winelib" DLL — an ELF `.so` that Wine loads as a DLL. Modern
Wine builds in **wow64 mode** always run a 64-bit process and execute 32-bit
programs through WOW64; they refuse to load a 32-bit ELF module
(`load_so_dll` reads the ELF header and rejects it — verified).

The supported way — the one Wine's own `wpcap.dll`, `winebus.sys` and
`wineusb.sys` use — is the **PE/Unix split**:

- A PE module stamped with the "Wine builtin DLL" signature (32 bytes after
  the DOS header; `server/mapping.c`). When Wine loads such a module, the
  Unix-side loader (`find_builtin_dll` in `dlls/ntdll/unix/loader.c`) searches
  every directory in `WINEDLLPATH` for the module and registers the
  **Unix library of the same name** with it. For a directory `D` it checks
  `D/i386-windows/X.dll` + `D/x86_64-unix/X.so`, and also the flat layout
  `D/X.dll` + `D/X.so` — which is what we use, with `D` = the program
  directory.
- The Unix library exports two function tables:
  `__wine_unix_call_funcs` (for 64-bit callers) and
  `__wine_unix_call_wow64_funcs` (for 32-bit WOW64 callers).
- The PE side gets a handle with
  `NtQueryVirtualMemory(module, MemoryWineUnixFuncs)` — under WOW64, ntdll
  turns this into `MemoryWineUnixWow64Funcs` automatically — and calls
  `__wine_unix_call_dispatcher(handle, function_number, &args)`, exported by
  ntdll. The call switches to 64-bit Unix mode **on the same thread**, runs
  the function and returns.

Why a separate `oscusb.dll` instead of stamping `oscwus.dll` itself: the
pairing only happens on the builtin load path, and the PE loader only takes
that path when it finds a builtin-stamped file on the normal search path.
`oscwus.dll` stays an ordinary native DLL; `oscusb.dll` is an empty, uniquely
named carrier whose only job is to get `oscusb.so` bound. `oscwus.dll` loads
it on the first `WinUsb_Initialize` (not in `DllMain`, where `LoadLibrary` is
unsafe) and uses its handle.

The argument structs (`src/oscusb_unixlib.h`) have the same layout for the
32-bit and the 64-bit compiler — pointers are carried as `uint64_t`, all
fields have fixed widths, sizes are multiples of 8 — so both dispatch tables
can point at the same functions. (Wine's `wpcap` instead writes a separate
WOW64 thunk per function that widens 32-bit pointers.)

Inside `oscusb.so`, transfers hold a read lock and open/close a write lock on
the device handle, so the data and control pipes run in parallel but the
handle can never be closed under a running transfer. libusb writes directly
into the application's buffer: the 64-bit code runs in the same address space,
and the 32-bit buffer addresses are valid there.

### Start-up sequence

1. `scripts/run.sh` sets `WINEDLLPATH=<program dir>` and starts the program.
2. The program loads `usbwave.dll`, which imports `OSCSETUP.dll` and
   `OSCWUS.DLL` from the program directory. `oscsetup.dll`'s `DllMain`
   hooks usbwave's `CreateFileA`.
3. usbwave.dll enumerates devices (fake answer), opens the fake path (dummy
   handle) and calls `WinUsb_Initialize`.
4. `oscwus.dll` loads `oscusb.dll`; Wine binds `oscusb.so`; `oscwus.dll` gets
   the unixlib handle and asks `oscusb.so` to open the device (libusb open,
   claim interface 0, drain stale data).
5. From here every `WinUsb_ReadPipe`/`WritePipe` becomes one
   `__wine_unix_call` → `libusb_bulk_transfer`.

---

## Repository layout

```
Makefile                build / install / run / uninstall
src/
  oscwus.c, oscwus.def      WinUSB replacement (oscwus.dll)
  oscsetup.c, oscsetup.def  SetupAPI replacement + CreateFileA hook (oscsetup.dll)
  oscusb_pe.c               empty builtin carrier (oscusb.dll)
  oscusb_unix.c             Unix side: libusb transfers (oscusb.so)
  oscusb_unixlib.h          PE <-> Unix argument structs and function numbers
  debuglog.h                shared debug log (OSCBRIDGE_LOG)
tools/
  patch_imports.py          import-name patch for usbwave.dll and other programs
  stamp_builtin.py          writes the "Wine builtin DLL" signature
scripts/
  run.sh                    start a program with WINEDLLPATH set
docs/
  PROTOCOL.md               USB protocol and WinUSB behavior notes
```

---

## Limitations

- **Wine in wow64 mode only (tested).** `oscusb.so` is built as a 64-bit
  library. In a classic setup where 32-bit programs run in a 32-bit Wine
  process, the Unix side would have to be a 32-bit `.so` built against a
  32-bit libusb; this has not been tried.
- **One device, PID `0x5661`.** The fake enumeration reports exactly one
  device with the ISDS205A's interface GUID; `oscusb.so` opens the first
  matching device with PID `0x5660`–`0x5666`.
- Only the WinUSB and SetupAPI functions the vendor software actually uses
  are implemented; the rest are stubs.
- `devmng.exe` and `ProfessionalVersion.exe` are the only programs tested end
  to end with the ISDS205A.

---

## License

[MIT](LICENSE). The vendor software is not part of this repository and is
covered by its own license.
