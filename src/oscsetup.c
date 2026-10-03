/* oscsetup.dll - minimal SetupDi* replacement.
 *
 * Makes the application believe it found exactly one ISDS205 device,
 * regardless of Wine's PnP state (Wine has no driver for this device, so the
 * real setupapi finds nothing).
 *
 * It implements only the 7 SetupDi* functions that usbwave.dll (and the
 * programs with embedded USB code: Recorder.exe, Logic.exe, Sweep.exe)
 * import. The patched binaries import this DLL as "OSCSETUP.dll" instead of
 * "SETUPAPI.dll" (see tools/patch_imports.py).
 *
 * The device path we return must contain a real "vid_xxxx&pid_xxxx" pattern
 * (usbwave.dll parses it to identify the model), but the same string is later
 * passed to CreateFileA, which would reject it. Instead of replacing
 * kernel32.dll, DllMain patches ONLY the CreateFileA entry in the import table
 * of the module that imports us, so that this one path yields a fake handle.
 * kernel32.dll and every other module stay untouched.
 */
#include <windows.h>
#include <winnt.h>
#include <setupapi.h>
#include <string.h>

#include "debuglog.h"

static void dbglog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_write("oscsetup", fmt, ap);
    va_end(ap);
}

/* The device-interface GUIDs from the vendor driver's Dso.inf, one per
 * PID 0x5660..0x5666. */
static const GUID KNOWN_GUIDS[7] = {
    {0x6B156A6D, 0x9CEC, 0x4DCB, {0x81, 0x45, 0x96, 0x3E, 0x45, 0xF6, 0x79, 0x0F}},
    {0x7C49CBB1, 0x780E, 0x4603, {0xAE, 0x4C, 0x0B, 0x18, 0xB8, 0xE6, 0xD8, 0xB9}},
    {0xFF2A876C, 0x4E6F, 0x9EAD, {0x49, 0x62, 0x48, 0x39, 0xB8, 0xEF, 0x7B, 0x68}},
    {0x75EA33E6, 0xB6B1, 0x4B0F, {0x97, 0xFF, 0x71, 0xC6, 0xD6, 0x63, 0x16, 0x60}},
    {0x2D7E25D0, 0x9E1F, 0x4175, {0xA6, 0xE6, 0x8C, 0x5E, 0x13, 0xA3, 0x1A, 0xFE}},
    {0xD37CE9DD, 0xB52B, 0x4CF7, {0xAC, 0x56, 0x6D, 0xA5, 0x0B, 0xD8, 0x14, 0xE6}},
    {0x4BB6DFBE, 0x39C0, 0x4321, {0xB4, 0x28, 0xEA, 0xB0, 0xEB, 0x9F, 0xC9, 0xB6}},
};

#define FAKE_DEVINFO ((HDEVINFO)0x44332211)

/* usbwave.dll lowercases the whole path, takes path[4 .. 4+21], replaces '#'
 * with '\', uppercases it and compares it against "USB\VID_D4A2&PID_5661"
 * (observed behavior: the extraction starts 4 characters into the path).
 * So characters 4..24 here must spell exactly that. The WINEBRIDGE_FAKE_DEV
 * marker lets Hook_CreateFileA recognize this path. */
static const char FAKE_DEVICE_PATH[] = "AAAAUSB\\VID_D4A2&PID_5661#0#WINEBRIDGE_FAKE_DEV";
static const char FAKE_INSTANCE_ID[] = "USB\\VID_D4A2&PID_5661\\WINEBRIDGE001";

/* Only the GUID of the PID 0x5661 device is reported. Reporting all seven made
 * the application detect several device types at once and it never reached
 * WinUsb_Initialize. */
static int guid_is_known(const GUID *g)
{
    if (!g)
        return 0;
    return memcmp(g, &KNOWN_GUIDS[1], sizeof(GUID)) == 0;
}

HDEVINFO WINAPI My_SetupDiGetClassDevsA(const GUID *ClassGuid, PCSTR Enumerator,
                                         HWND hwndParent, DWORD Flags)
{
    (void)hwndParent;
    if (ClassGuid)
        dbglog("GetClassDevsA guid={%08lx-%04x-%04x-...} enum=%s flags=%lx known=%d",
               (unsigned long)ClassGuid->Data1, ClassGuid->Data2, ClassGuid->Data3,
               Enumerator ? Enumerator : "(null)", (unsigned long)Flags, guid_is_known(ClassGuid));
    else
        dbglog("GetClassDevsA guid=NULL enum=%s flags=%lx",
               Enumerator ? Enumerator : "(null)", (unsigned long)Flags);

    if (guid_is_known(ClassGuid))
        return FAKE_DEVINFO;
    SetLastError(ERROR_NO_MORE_ITEMS);
    return (HDEVINFO)INVALID_HANDLE_VALUE;
}

WINBOOL WINAPI My_SetupDiEnumDeviceInfo(HDEVINFO DeviceInfoSet, DWORD MemberIndex,
                                        PSP_DEVINFO_DATA DeviceInfoData)
{
    if (DeviceInfoSet != FAKE_DEVINFO || MemberIndex != 0)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    DeviceInfoData->ClassGuid = KNOWN_GUIDS[1];
    DeviceInfoData->DevInst = 1;
    DeviceInfoData->Reserved = 0;
    return TRUE;
}

WINBOOL WINAPI My_SetupDiEnumDeviceInterfaces(HDEVINFO DeviceInfoSet,
                                               PSP_DEVINFO_DATA DeviceInfoData,
                                               const GUID *InterfaceClassGuid,
                                               DWORD MemberIndex,
                                               PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData)
{
    (void)DeviceInfoData;
    dbglog("EnumDeviceInterfaces devinfo=%p member=%lu", DeviceInfoSet, (unsigned long)MemberIndex);
    if (DeviceInfoSet != FAKE_DEVINFO || MemberIndex != 0)
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    DeviceInterfaceData->InterfaceClassGuid = InterfaceClassGuid ? *InterfaceClassGuid : KNOWN_GUIDS[1];
    DeviceInterfaceData->Flags = 1; /* SPINT_ACTIVE */
    DeviceInterfaceData->Reserved = 0;
    return TRUE;
}

WINBOOL WINAPI My_SetupDiGetDeviceInterfaceDetailA(HDEVINFO DeviceInfoSet,
                                                    PSP_DEVICE_INTERFACE_DATA DeviceInterfaceData,
                                                    PSP_DEVICE_INTERFACE_DETAIL_DATA_A DeviceInterfaceDetailData,
                                                    DWORD DeviceInterfaceDetailDataSize,
                                                    PDWORD RequiredSize,
                                                    PSP_DEVINFO_DATA DeviceInfoData)
{
    DWORD needed = (DWORD)(sizeof(DWORD) + strlen(FAKE_DEVICE_PATH) + 1);

    (void)DeviceInterfaceData;
    dbglog("GetDeviceInterfaceDetailA devinfo=%p bufsize=%lu needed=%lu", DeviceInfoSet,
           (unsigned long)DeviceInterfaceDetailDataSize, (unsigned long)needed);

    if (DeviceInfoSet != FAKE_DEVINFO)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (DeviceInterfaceDetailData == NULL || DeviceInterfaceDetailDataSize < needed)
    {
        if (RequiredSize)
            *RequiredSize = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    strcpy(DeviceInterfaceDetailData->DevicePath, FAKE_DEVICE_PATH);
    if (RequiredSize)
        *RequiredSize = needed;
    if (DeviceInfoData)
    {
        DeviceInfoData->ClassGuid = KNOWN_GUIDS[1];
        DeviceInfoData->DevInst = 1;
        DeviceInfoData->Reserved = 0;
    }
    return TRUE;
}

WINBOOL WINAPI My_SetupDiGetDeviceInstanceIdA(HDEVINFO DeviceInfoSet,
                                               PSP_DEVINFO_DATA DeviceInfoData,
                                               PSTR DeviceInstanceId, DWORD DeviceInstanceIdSize,
                                               PDWORD RequiredSize)
{
    DWORD needed = (DWORD)strlen(FAKE_INSTANCE_ID) + 1;

    (void)DeviceInfoData;
    if (DeviceInfoSet != FAKE_DEVINFO)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (RequiredSize)
        *RequiredSize = needed;
    if (DeviceInstanceIdSize < needed)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    strcpy(DeviceInstanceId, FAKE_INSTANCE_ID);
    return TRUE;
}

/* Not needed on the device-open path; a safe stub. */
WINBOOL WINAPI My_SetupDiGetDeviceRegistryPropertyA(HDEVINFO DeviceInfoSet,
                                                     PSP_DEVINFO_DATA DeviceInfoData,
                                                     DWORD Property, PDWORD PropertyRegDataType,
                                                     PBYTE PropertyBuffer,
                                                     DWORD PropertyBufferSize,
                                                     PDWORD RequiredSize)
{
    (void)DeviceInfoSet;
    (void)DeviceInfoData;
    (void)Property;
    (void)PropertyRegDataType;
    (void)PropertyBuffer;
    (void)PropertyBufferSize;
    if (RequiredSize)
        *RequiredSize = 0;
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
}

WINBOOL WINAPI My_SetupDiDestroyDeviceInfoList(HDEVINFO DeviceInfoSet)
{
    (void)DeviceInfoSet;
    return TRUE;
}

/* ---------------- CreateFileA import hook ---------------- */

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileA_t g_real_CreateFileA = NULL;

#define FAKE_FILE_HANDLE ((HANDLE)0x0badf00d)

/* usbwave.dll passes the lowercased path to CreateFileA
 * ("aaaausb\vid_d4a2&pid_5661#0#winebridge_fake_dev", seen live in winedbg),
 * so the marker has to be matched case-insensitively. */
static int stristr_match(const char *hay, const char *needle)
{
    size_t hlen, nlen, i, j;

    if (!hay || !needle)
        return 0;
    hlen = strlen(hay);
    nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen)
        return 0;
    for (i = 0; i + nlen <= hlen; i++)
    {
        for (j = 0; j < nlen; j++)
        {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z')
                a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z')
                b += 'a' - 'A';
            if (a != b)
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                                       LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                                       DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
                                       HANDLE hTemplateFile)
{
    if (lpFileName)
        dbglog("Hook_CreateFileA saw: %s", lpFileName);
    if (stristr_match(lpFileName, "WINEBRIDGE_FAKE_DEV"))
    {
        dbglog("Hook_CreateFileA: %s -> fake handle", lpFileName);
        return FAKE_FILE_HANDLE;
    }
    if (g_real_CreateFileA)
        return g_real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                                   dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
    SetLastError(ERROR_PROC_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
}

static PIMAGE_IMPORT_DESCRIPTOR first_import(HMODULE hMod)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
    PIMAGE_NT_HEADERS32 nt = (PIMAGE_NT_HEADERS32)((BYTE *)hMod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY impdir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (impdir.VirtualAddress == 0)
        return NULL;
    return (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)hMod + impdir.VirtualAddress);
}

static int imports_dll(HMODULE hMod, const char *name)
{
    PIMAGE_IMPORT_DESCRIPTOR imp = first_import(hMod);

    for (; imp && imp->Name; imp++)
        if (_stricmp((char *)hMod + imp->Name, name) == 0)
            return 1;
    return 0;
}

static void patch_createfile_iat(HMODULE hMod, const char *label)
{
    PIMAGE_IMPORT_DESCRIPTOR imp = first_import(hMod);

    if (!imp)
    {
        dbglog("patch_createfile_iat: %s has no import directory", label);
        return;
    }

    for (; imp->Name; imp++)
    {
        DWORD thunk_rva;
        PIMAGE_THUNK_DATA32 thunk_orig, thunk_iat;

        if (_stricmp((char *)hMod + imp->Name, "KERNEL32.dll") != 0)
            continue;

        thunk_rva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk;
        thunk_orig = (PIMAGE_THUNK_DATA32)((BYTE *)hMod + thunk_rva);
        thunk_iat = (PIMAGE_THUNK_DATA32)((BYTE *)hMod + imp->FirstThunk);

        for (; thunk_orig->u1.AddressOfData; thunk_orig++, thunk_iat++)
        {
            PIMAGE_IMPORT_BY_NAME byname;
            DWORD old_prot;

            if (thunk_orig->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                continue;
            byname = (PIMAGE_IMPORT_BY_NAME)((BYTE *)hMod + thunk_orig->u1.AddressOfData);
            if (strcmp((char *)byname->Name, "CreateFileA") != 0)
                continue;

            g_real_CreateFileA = (CreateFileA_t)(ULONG_PTR)thunk_iat->u1.Function;
            VirtualProtect(&thunk_iat->u1.Function, sizeof(DWORD), PAGE_READWRITE, &old_prot);
            thunk_iat->u1.Function = (DWORD)(ULONG_PTR)Hook_CreateFileA;
            VirtualProtect(&thunk_iat->u1.Function, sizeof(DWORD), old_prot, &old_prot);
            dbglog("%s IAT CreateFileA hook installed, orig=%p", label, (void *)g_real_CreateFileA);
            return;
        }
    }
    dbglog("patch_createfile_iat: CreateFileA not found in the IAT of %s", label);
}

/* The code that opens the device path lives in the module that imports us:
 * usbwave.dll for ProfessionalVersion/devmng/VmMso, the main exe for programs
 * with embedded USB code (Recorder.exe, ...). Import tables are filled in
 * before any DllMain runs, so patching here is safe. */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)hinst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
    {
        HMODULE usbwave = GetModuleHandleA("usbwave.dll");
        HMODULE exe = GetModuleHandleA(NULL);

        log_init();
        if (usbwave && imports_dll(usbwave, "OSCSETUP.dll"))
            patch_createfile_iat(usbwave, "usbwave.dll");
        if (exe && exe != usbwave && imports_dll(exe, "OSCSETUP.dll"))
            patch_createfile_iat(exe, "main exe");
    }
    return TRUE;
}
