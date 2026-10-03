/* oscusb.dll - an empty 32-bit "carrier" module.
 *
 * It contains no code. After the build it is stamped with the
 * "Wine builtin DLL" signature (tools/stamp_builtin.py). When Wine loads a
 * module carrying that signature it looks for a Unix library with the same
 * base name in WINEDLLPATH (<dir>/oscusb.so) and binds the two together.
 * oscwus.dll then reaches oscusb.so through this module's unixlib handle.
 *
 * A separate, uniquely named carrier is needed because the pairing only
 * happens on Wine's builtin load path; oscwus.dll itself is an ordinary
 * native DLL.
 */
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
