#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winternl.h>
#include <iostream>
#include <vector>
#include <string>
#include <tchar.h>
#include <psapi.h>
#include <algorithm>
#include <shobjidl.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker, "/manifestinput:unlock_file.manifest")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// NTAPI definitions not in winternl.h
#define SystemExtendedHandleInformation 0x40
#define ObjectNameInformation 1
#define ObjectTypeInformation 2

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

// Control IDs
#define ID_EDIT_PATH          101
#define ID_BTN_BROWSE_FILE    102
#define ID_BTN_BROWSE_FOLDER  103
#define ID_CHK_RECURSIVE      104
#define ID_BTN_UNLOCK         105
#define ID_PROGRESS           106
#define ID_EDIT_LOG           107

#define WM_UNLOCK_COMPLETE    (WM_USER + 1)

struct UnlockThreadParams {
    HWND hwnd;
    std::wstring targetPath;
    bool recursive;
};

struct QueryNameParams {
    HANDLE handle;
    _NtQueryObject NtQueryObject;
    std::wstring result;
    bool success;
};

// Case-insensitive string comparison
bool EqualsIgnoreCase(const std::wstring& s1, const std::wstring& s2) {
    if (s1.length() != s2.length()) return false;
    return std::equal(s1.begin(), s1.end(), s2.begin(), [](wchar_t c1, wchar_t c2) {
        return towlower(c1) == towlower(c2);
    });
}

// Helper to get NT path from a Win32 path
std::wstring GetNtPath(const std::wstring& win32Path) {
    wchar_t deviceName[MAX_PATH] = {0};
    if (win32Path.length() < 2 || win32Path[1] != L':') return win32Path;

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

bool MatchPath(const std::wstring& found, const std::wstring& targetNt, const std::wstring& targetWin32, bool recursive) {
    std::wstring normalizedFound = found;
    if (normalizedFound.find(L"\\??\\") == 0) normalizedFound = normalizedFound.substr(4);

    auto checkPrefix = [](const std::wstring& path, const std::wstring& prefix) {
        if (path.length() == prefix.length()) {
            return _wcsicmp(path.c_str(), prefix.c_str()) == 0;
        }
        if (path.length() > prefix.length()) {
            if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.length()) == 0) {
                if (prefix.empty() || prefix.back() == L'\\' || path[prefix.length()] == L'\\') {
                    return true;
                }
            }
        }
        return false;
    };

    if (recursive) {
        if (checkPrefix(normalizedFound, targetNt)) return true;
        if (checkPrefix(normalizedFound, targetWin32)) return true;
    } else {
        if (_wcsicmp(normalizedFound.c_str(), targetNt.c_str()) == 0) return true;
        if (_wcsicmp(normalizedFound.c_str(), targetWin32.c_str()) == 0) return true;

        if (targetWin32.length() > 3) {
            std::wstring suffix = targetWin32.substr(2);
            if (normalizedFound.length() >= suffix.length()) {
                std::wstring foundSuffix = normalizedFound.substr(normalizedFound.length() - suffix.length());
                if (_wcsicmp(foundSuffix.c_str(), suffix.c_str()) == 0) {
                    size_t pos = normalizedFound.length() - suffix.length();
                    if (pos == 0 || normalizedFound[pos - 1] == L'\\' || normalizedFound[pos - 1] == L':') {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

DWORD WINAPI UnlockFilesThread(LPVOID lpParam) {
    UnlockThreadParams* params = (UnlockThreadParams*)lpParam;
    HWND hwnd = params->hwnd;
    std::wstring targetPath = params->targetPath;
    bool bRecursive = params->recursive;

    auto LogMsg = [hwnd](const std::wstring& text) {
        HWND hLog = GetDlgItem(hwnd, ID_EDIT_LOG);
        std::wstring line = text + L"\r\n";
        int len = GetWindowTextLengthW(hLog);
        SendMessageW(hLog, EM_SETSEL, len, len);
        SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    };

    wchar_t fullPath[MAX_PATH];
    if (GetFullPathNameW(targetPath.c_str(), MAX_PATH, fullPath, NULL)) {
        targetPath = fullPath;
    }

    std::wstring targetNtPath = GetNtPath(targetPath);
    LogMsg(L"Target Path: " + targetPath);
    LogMsg(L"Target NT Path: " + targetNtPath);
    if (bRecursive) {
        LogMsg(L"Mode: Recursive folder unlock enabled.");
    }

    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        SetPrivilege(hToken, SE_DEBUG_NAME, TRUE);
        CloseHandle(hToken);
    }

    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    auto NtQuerySystemInformation = (_NtQuerySystemInformation)GetProcAddress(hNtDll, "NtQuerySystemInformation");
    auto NtQueryObject = (_NtQueryObject)GetProcAddress(hNtDll, "NtQueryObject");

    USHORT fileTypeIndex = 0;
    HANDLE hCurrentFile = CreateFileW(targetPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hCurrentFile != INVALID_HANDLE_VALUE) {
        ULONG handleInfoSize = 0x10000;
        std::vector<BYTE> handleInfoBuffer(handleInfoSize);
        NTSTATUS handleStatus;
        while ((handleStatus = NtQuerySystemInformation(SystemExtendedHandleInformation, handleInfoBuffer.data(), handleInfoSize, &handleInfoSize)) == 0xC0000004) {
            handleInfoBuffer.resize(handleInfoSize);
        }
        if (handleStatus == 0) {
            PSYSTEM_HANDLE_INFORMATION_EX handleInfo = (PSYSTEM_HANDLE_INFORMATION_EX)handleInfoBuffer.data();
            DWORD currentPid = GetCurrentProcessId();
            for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; i++) {
                if (handleInfo->Handles[i].UniqueProcessId == currentPid && (HANDLE)handleInfo->Handles[i].HandleValue == hCurrentFile) {
                    fileTypeIndex = handleInfo->Handles[i].ObjectTypeIndex;
                    break;
                }
            }
        }
        CloseHandle(hCurrentFile);
    }

    if (fileTypeIndex == 0) {
        LogMsg(L"Warning: Could not determine file type index. Scanning all handles...");
    } else {
        wchar_t idxBuf[32];
        _swprintf(idxBuf, L"%u", fileTypeIndex);
        LogMsg(L"Detected File Type Index: " + std::wstring(idxBuf));
    }

    ULONG size = 0x20000;
    std::vector<BYTE> buffer(size);
    NTSTATUS status;

    while ((status = NtQuerySystemInformation(SystemExtendedHandleInformation, buffer.data(), size, &size)) == 0xC0000004) {
        buffer.resize(size);
    }

    if (status != 0) {
        LogMsg(L"Failed to query system handle information.");
        PostMessageW(hwnd, WM_UNLOCK_COMPLETE, 0, 0);
        delete params;
        return 1;
    }

    PSYSTEM_HANDLE_INFORMATION_EX handleInfo = (PSYSTEM_HANDLE_INFORMATION_EX)buffer.data();
    SendDlgItemMessageW(hwnd, ID_PROGRESS, PBM_SETRANGE32, 0, handleInfo->NumberOfHandles);

    int foundCount = 0;
    for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; i++) {
        if (i % 200 == 0 || i == handleInfo->NumberOfHandles - 1) {
            SendDlgItemMessageW(hwnd, ID_PROGRESS, PBM_SETPOS, i, 0);
        }

        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX entry = handleInfo->Handles[i];

        if (fileTypeIndex != 0 && entry.ObjectTypeIndex != fileTypeIndex) continue;

        HANDLE hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, (DWORD)entry.UniqueProcessId);
        if (!hProcess) continue;

        HANDLE hDup = NULL;
        if (DuplicateHandle(hProcess, (HANDLE)entry.HandleValue, GetCurrentProcess(), &hDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            QueryNameParams nameParams = { hDup, NtQueryObject, L"", false };
            HANDLE hThread = CreateThread(NULL, 0, QueryNameThread, &nameParams, 0, NULL);

            if (hThread) {
                if (WaitForSingleObject(hThread, 50) == WAIT_OBJECT_0) {
                    if (nameParams.success) {
                        std::wstring foundPath = nameParams.result;

                        if (MatchPath(foundPath, targetNtPath, targetPath, bRecursive)) {
                            wchar_t processName[MAX_PATH] = L"<unknown>";
                            HANDLE hProcessName = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)entry.UniqueProcessId);
                            if (hProcessName) {
                                GetModuleBaseNameW(hProcessName, NULL, processName, MAX_PATH);
                                CloseHandle(hProcessName);
                            }

                            wchar_t pidBuf[32];
                            _swprintf(pidBuf, L"%u", (DWORD)entry.UniqueProcessId);
                            LogMsg(L"Found lock in: " + std::wstring(processName) + L" (PID: " + pidBuf + L")");
                            LogMsg(L"  Object Path: " + nameParams.result);

                            if (DuplicateHandle(hProcess, (HANDLE)entry.HandleValue, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                                LogMsg(L"  [SUCCESS] Closed handle successfully.");
                                foundCount++;
                            } else {
                                wchar_t errBuf[32];
                                _swprintf(errBuf, L"%u", GetLastError());
                                LogMsg(L"  [FAILED] Failed to close handle. Error: " + std::wstring(errBuf));
                            }
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

    SendDlgItemMessageW(hwnd, ID_PROGRESS, PBM_SETPOS, handleInfo->NumberOfHandles, 0);

    wchar_t countBuf[32];
    _swprintf(countBuf, L"%d", foundCount);
    if (foundCount == 0) {
        LogMsg(L"Finished. No locks found for this path.");
    } else {
        LogMsg(L"Finished. Successfully unlocked " + std::wstring(countBuf) + L" handles.");
    }

    PostMessageW(hwnd, WM_UNLOCK_COMPLETE, 0, (LPARAM)foundCount);
    delete params;
    return 0;
}

HRESULT PickFileOrFolder(HWND hwnd, bool folderOnly, std::wstring& outPath) {
    IFileOpenDialog *pFileOpen = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        FILEOPENDIALOGOPTIONS options;
        hr = pFileOpen->GetOptions(&options);
        if (SUCCEEDED(hr)) {
            if (folderOnly) {
                pFileOpen->SetOptions(options | FOS_PICKFOLDERS | FOS_NODEREFERENCELINKS);
            } else {
                // FOS_NOVALIDATE skips testing if the file is locked or in use, allowing us to pick it to unlock it!
                pFileOpen->SetOptions(options | FOS_NOVALIDATE | FOS_NODEREFERENCELINKS);
            }
        }
        hr = pFileOpen->Show(hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem *pItem = NULL;
            hr = pFileOpen->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath = NULL;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    outPath = pszFilePath;
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
    return hr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH hBgBrush = NULL;
    static HBRUSH hEditBgBrush = NULL;
    static HFONT hFontNormal = NULL;
    static HFONT hFontTitle = NULL;
    static HWND hEditPath, hBtnBrowseFile, hBtnBrowseFolder, hChkRecursive, hBtnUnlock, hProgress, hEditLog;

    switch (msg) {
        case WM_CREATE: {
            hBgBrush = CreateSolidBrush(RGB(24, 24, 24));
            hEditBgBrush = CreateSolidBrush(RGB(36, 36, 36));

            hFontNormal = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            hFontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            // Title (Fixed & ampersand display character escaping issue)
            HWND hTitle = CreateWindowW(L"STATIC", L"File && Folder Unlocker", WS_CHILD | WS_VISIBLE, 20, 15, 560, 30, hwnd, NULL, NULL, NULL);
            SendMessageW(hTitle, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

            // Path Label
            HWND hLabel = CreateWindowW(L"STATIC", L"Target File or Folder Path:", WS_CHILD | WS_VISIBLE, 20, 55, 300, 20, hwnd, NULL, NULL, NULL);
            SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

            // Path Input
            hEditPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 75, 435, 24, hwnd, (HMENU)ID_EDIT_PATH, NULL, NULL);
            SendMessageW(hEditPath, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

            // Browse Buttons (Owner drawn to be dark themed)
            hBtnBrowseFile = CreateWindowW(L"BUTTON", L"File...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 465, 74, 65, 26, hwnd, (HMENU)ID_BTN_BROWSE_FILE, NULL, NULL);
            hBtnBrowseFolder = CreateWindowW(L"BUTTON", L"Folder...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 535, 74, 65, 26, hwnd, (HMENU)ID_BTN_BROWSE_FOLDER, NULL, NULL);

            // Recursive Checkbox (Split into check box square + a static label for flawless custom dark mode text coloring)
            hChkRecursive = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 112, 20, 20, hwnd, (HMENU)ID_CHK_RECURSIVE, NULL, NULL);
            SendMessageW(hChkRecursive, BM_SETCHECK, BST_CHECKED, 0); // Check it by default
            HWND hChkLabel = CreateWindowW(L"STATIC", L"Recursive (for folders)", WS_CHILD | WS_VISIBLE, 44, 112, 250, 20, hwnd, NULL, NULL, NULL);
            SendMessageW(hChkLabel, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

            // Unlock Button
            hBtnUnlock = CreateWindowW(L"BUTTON", L"Unlock", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 465, 108, 135, 30, hwnd, (HMENU)ID_BTN_UNLOCK, NULL, NULL);

            // Progress Bar
            hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 20, 150, 580, 16, hwnd, (HMENU)ID_PROGRESS, NULL, NULL);
            SendMessageW(hProgress, PBM_SETBARCOLOR, 0, RGB(0, 122, 204));
            SendMessageW(hProgress, PBM_SETBKCOLOR, 0, RGB(36, 36, 36));

            // Log output box
            hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Ready.\r\n", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 20, 180, 580, 245, hwnd, (HMENU)ID_EDIT_LOG, NULL, NULL);
            SendMessageW(hEditLog, WM_SETFONT, (WPARAM)hFontNormal, TRUE);

            // Turn scrollbars dark themed natively
            SetWindowTheme(hEditLog, L"DarkMode_Explorer", NULL);

            // Set Dark Title Bar for Windows 10/11
            BOOL darkTheme = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &darkTheme, sizeof(darkTheme));

            // Populate initial path if passed
            LPCWSTR initPath = (LPCWSTR)((CREATESTRUCTW*)lParam)->lpCreateParams;
            if (initPath && wcslen(initPath) > 0) {
                SetWindowTextW(hEditPath, initPath);
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(220, 220, 220));
            SetBkColor(hdc, RGB(24, 24, 24));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)hBgBrush;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, RGB(240, 240, 240));
            SetBkColor(hdc, RGB(36, 36, 36));
            return (LRESULT)hEditBgBrush;
        }

        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
            if (pDIS->CtlID == ID_BTN_BROWSE_FILE || pDIS->CtlID == ID_BTN_BROWSE_FOLDER || pDIS->CtlID == ID_BTN_UNLOCK) {
                bool isPressed = (pDIS->itemState & ODS_SELECTED);
                bool isDisabled = (pDIS->itemState & ODS_DISABLED);

                HBRUSH hBtnBrush;
                if (pDIS->CtlID == ID_BTN_UNLOCK && !isDisabled) {
                    hBtnBrush = isPressed ? CreateSolidBrush(RGB(0, 98, 163)) : CreateSolidBrush(RGB(0, 122, 204));
                } else if (isDisabled) {
                    hBtnBrush = CreateSolidBrush(RGB(45, 45, 48));
                } else {
                    hBtnBrush = isPressed ? CreateSolidBrush(RGB(65, 65, 65)) : CreateSolidBrush(RGB(50, 50, 50));
                }

                FillRect(pDIS->hDC, &pDIS->rcItem, hBtnBrush);
                DeleteObject(hBtnBrush);

                HPEN hPen = CreatePen(PS_SOLID, 1, isDisabled ? RGB(55, 55, 55) : (pDIS->CtlID == ID_BTN_UNLOCK ? RGB(0, 150, 255) : RGB(80, 80, 80)));
                HGDIOBJ hOldPen = SelectObject(pDIS->hDC, hPen);
                HGDIOBJ hOldBrush = SelectObject(pDIS->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(pDIS->hDC, pDIS->rcItem.left, pDIS->rcItem.top, pDIS->rcItem.right, pDIS->rcItem.bottom);
                SelectObject(pDIS->hDC, hOldBrush);
                SelectObject(pDIS->hDC, hOldPen);
                DeleteObject(hPen);

                wchar_t btnText[64] = {0};
                GetWindowTextW(pDIS->hwndItem, btnText, 64);
                SetTextColor(pDIS->hDC, isDisabled ? RGB(130, 130, 130) : RGB(245, 245, 245));
                SetBkMode(pDIS->hDC, TRANSPARENT);
                DrawTextW(pDIS->hDC, btnText, -1, &pDIS->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == ID_BTN_BROWSE_FILE || wmId == ID_BTN_BROWSE_FOLDER) {
                std::wstring selectedPath;
                if (SUCCEEDED(PickFileOrFolder(hwnd, wmId == ID_BTN_BROWSE_FOLDER, selectedPath))) {
                    if (!selectedPath.empty()) {
                        SetWindowTextW(hEditPath, selectedPath.c_str());
                    }
                }
            } else if (wmId == ID_BTN_UNLOCK) {
                wchar_t pathBuf[MAX_PATH];
                GetWindowTextW(hEditPath, pathBuf, MAX_PATH);
                std::wstring target = pathBuf;

                if (target.empty()) {
                    MessageBoxW(hwnd, L"Please select a target file or folder first.", L"Warning", MB_ICONWARNING | MB_OK);
                    break;
                }

                bool recursive = (SendMessageW(hChkRecursive, BM_GETCHECK, 0, 0) == BST_CHECKED);

                // Disable controls
                EnableWindow(hEditPath, FALSE);
                EnableWindow(hBtnBrowseFile, FALSE);
                EnableWindow(hBtnBrowseFolder, FALSE);
                EnableWindow(hChkRecursive, FALSE);
                EnableWindow(hBtnUnlock, FALSE);

                SetWindowTextW(hEditLog, L"Starting unlock process...\r\n");
                SendMessageW(hProgress, PBM_SETPOS, 0, 0);

                UnlockThreadParams* params = new UnlockThreadParams{ hwnd, target, recursive };
                CreateThread(NULL, 0, UnlockFilesThread, params, 0, NULL);
            }
            break;
        }

        case WM_UNLOCK_COMPLETE: {
            EnableWindow(hEditPath, TRUE);
            EnableWindow(hBtnBrowseFile, TRUE);
            EnableWindow(hBtnBrowseFolder, TRUE);
            EnableWindow(hChkRecursive, TRUE);
            EnableWindow(hBtnUnlock, TRUE);
            break;
        }

        case WM_DESTROY: {
            if (hBgBrush) DeleteObject(hBgBrush);
            if (hEditBgBrush) DeleteObject(hEditBgBrush);
            if (hFontNormal) DeleteObject(hFontNormal);
            if (hFontTitle) DeleteObject(hFontTitle);
            PostQuitMessage(0);
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring initialPath = L"";
    if (argv && argc > 1) {
        initialPath = argv[1];
        LocalFree(argv);
    }

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
    wc.lpszClassName = L"UnlockFileWindow";

    if (!RegisterClassExW(&wc)) {
        return 0;
    }

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int winWidth = 635;
    int winHeight = 485;
    int winX = (screenWidth - winWidth) / 2;
    int winY = (screenHeight - winHeight) / 2;

    HWND hwnd = CreateWindowExW(
        0, L"UnlockFileWindow", L"Unlock Files Utility",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        winX, winY, winWidth, winHeight,
        NULL, NULL, hInstance, (LPVOID)initialPath.c_str()
    );

    if (!hwnd) {
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return (int)msg.wParam;
}
