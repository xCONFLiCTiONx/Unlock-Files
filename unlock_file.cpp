#include <windows.h>
#include <winternl.h>
#include <iostream>
#include <vector>
#include <string>
#include <tchar.h>
#include <psapi.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

// Embed manifest to require administrative privileges
#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

// NTAPI definitions not in winternl.h
#define SystemExtendedHandleInformation 0x40
#define ObjectNameInformation 1

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, *PSYSTEM_HANDLE_INFORMATION_EX;

typedef NTSTATUS (NTAPI *_NtQuerySystemInformation)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

typedef NTSTATUS (NTAPI *_NtQueryObject)(
    HANDLE Handle,
    ULONG ObjectInformationClass,
    PVOID ObjectInformation,
    ULONG ObjectInformationLength,
    PULONG ReturnLength
);

// Helper to get NT path from a Win32 path
std::wstring GetNtPath(const std::wstring& win32Path) {
    wchar_t deviceName[MAX_PATH] = {0};
    wchar_t drive[3] = { win32Path[0], win32Path[1], L'\0' };

    if (QueryDosDeviceW(drive, deviceName, MAX_PATH)) {
        std::wstring ntPath = deviceName;
        ntPath += &win32Path[2];
        return ntPath;
    }
    return win32Path;
}

BOOL SetPrivilege(HANDLE hToken, LPCTSTR lpszPrivilege, BOOL bEnablePrivilege) {
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!LookupPrivilegeValue(NULL, lpszPrivilege, &luid)) return FALSE;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = (bEnablePrivilege) ? SE_PRIVILEGE_ENABLED : 0;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) return FALSE;
    return GetLastError() == ERROR_SUCCESS;
}

struct QueryNameParams {
    HANDLE handle;
    _NtQueryObject NtQueryObject;
    std::wstring result;
    bool success;
};

DWORD WINAPI QueryNameThread(LPVOID lpParam) {
    QueryNameParams* params = (QueryNameParams*)lpParam;
    ULONG size = 0;
    params->NtQueryObject(params->handle, ObjectNameInformation, NULL, 0, &size);
    if (size == 0) size = 2048;

    std::vector<BYTE> buffer(size);
    NTSTATUS status = params->NtQueryObject(params->handle, ObjectNameInformation, buffer.data(), size, &size);

    if (status == 0) { // STATUS_SUCCESS
        PUNICODE_STRING pName = (PUNICODE_STRING)buffer.data();
        if (pName->Buffer && pName->Length > 0) {
            params->result = std::wstring(pName->Buffer, pName->Length / sizeof(wchar_t));
            params->success = true;
        }
    }
    return 0;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcout << L"Usage: unlock_file.exe <file_path>" << std::endl;
        return 1;
    }

    std::wstring targetPath = argv[1];
    // Convert to absolute path if necessary
    wchar_t fullPath[MAX_PATH];
    if (GetFullPathNameW(targetPath.c_str(), MAX_PATH, fullPath, NULL)) {
        targetPath = fullPath;
    }

    std::wstring targetNtPath = GetNtPath(targetPath);
    std::wcout << L"Target NT Path: " << targetNtPath << std::endl;

    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        SetPrivilege(hToken, SE_DEBUG_NAME, TRUE);
        CloseHandle(hToken);
    }

    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    auto NtQuerySystemInformation = (_NtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
    auto NtQueryObject = (_NtQueryObject)GetProcAddress(hNtDll, "NtQueryObject");

    ULONG size = 0x10000;
    std::vector<BYTE> buffer(size);
    NTSTATUS status;

    while ((status = NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), size, &size)) == 0xC0000004) { // STATUS_INFO_LENGTH_MISMATCH
        buffer.resize(size);
    }

    if (status != 0) {
        std::cerr << "Failed to query system handle information. Status: " << std::hex << status << std::endl;
        return 1;
    }

    PSYSTEM_HANDLE_INFORMATION_EX handleInfo = (PSYSTEM_HANDLE_INFORMATION_EX)buffer.data();
    std::wcout << L"Searching through " << handleInfo->NumberOfHandles << L" handles..." << std::endl;

    for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; i++) {
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX entry = handleInfo->Handles[i];

        HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)entry.UniqueProcessId);
        if (!hProcess) continue;

        HANDLE hDup = NULL;
        if (DuplicateHandle(hProcess, (HANDLE)entry.HandleValue, GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {

            // Use a thread to query name because NtQueryObject can hang on pipes/sockets
            QueryNameParams params = { hDup, NtQueryObject, L"", false };
            HANDLE hThread = CreateThread(NULL, 0, QueryNameThread, &params, 0, NULL);

            if (hThread) {
                if (WaitForSingleObject(hThread, 50) == WAIT_OBJECT_0) { // 50ms timeout
                    if (params.success && params.result == targetNtPath) {
                        wchar_t processName[MAX_PATH] = L"<unknown>";
                        HANDLE hProcessName = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)entry.UniqueProcessId);
                        if (hProcessName) {
                            GetModuleBaseNameW(hProcessName, NULL, processName, MAX_PATH);
                            CloseHandle(hProcessName);
                        }

                        std::wcout << L"Found lock in process: " << processName << L" (PID: " << entry.UniqueProcessId << L")" << std::endl;

                        // Close the handle in the source process
                        if (DuplicateHandle(hProcess, (HANDLE)entry.HandleValue, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                            std::wcout << L"Successfully closed handle." << std::endl;
                        } else {
                            std::wcout << L"Failed to close handle. Error: " << GetLastError() << std::endl;
                        }
                    }
                } else {
                    TerminateThread(hThread, 0);
                }
                CloseHandle(hThread);
            }
            CloseHandle(hDup);
        }
        CloseHandle(hProcess);
    }

    std::wcout << L"Finished search." << std::endl;
    return 0;
}
