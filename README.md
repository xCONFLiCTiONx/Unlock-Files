# Unlock Files Utility

A lightweight, standalone C++ utility designed to identify and force-close handles that are locking a file. This tool is specifically built to be integrated into context menu extensions like [xToolsMenu](https://github.com/xCONFLiCTiONx/xToolsMenu), allowing you to right-click and "Unlock" a file that the system or another process is holding onto.

## Key Features

- **Deep Handle Scanning**: Uses low-level Windows NTAPI (`SystemExtendedHandleInformation`) to find locks across the entire system, including those held by the **System (PID 4)** process.
- **Kernel-Safe Design**: Implements a threaded query mechanism with a 50ms timeout. This prevents the utility from hanging or causing a **BSOD** when encountering blocked kernel objects (like synchronous pipes or dead network shares).
- **Process Identification**: Shows you exactly which process (e.g., `SearchIndexer.exe`, `System`) is holding the lock.
- **UAC Elevation**: Built-in manifest ensures the tool always runs with the Administrative privileges required to close handles in other processes.
- **Custom Icon**: Compiled with the classic lock icon from `imageres.dll`.

## How to Compile

To build the executable with the embedded icon and manifest, use the **Visual Studio Developer Command Prompt**:

```cmd
rc unlock_file.rc
cl /EHsc unlock_file.cpp unlock_file.res
```

This will generate `unlock_file.exe`.

## Usage

### Command Line
You can run it manually from an elevated terminal:
```cmd
unlock_file.exe "C:\path\to\your\locked_file.txt"
```

### xToolsMenu Integration
To add this to your right-click menu:
1. Copy the compiled `unlock_file.exe` to a permanent folder.
2. In your `xToolsMenu` configuration, add an entry for the file type (or `*` for all files).
3. Set the command to: `"C:\path\to\unlock_file.exe" "%1"`

## Technical Details

- **Path Mapping**: Automatically maps Win32 paths (e.g., `C:\...`) to NT Device paths (e.g., `\Device\HarddiskVolume...\...`) to accurately match kernel object names.
- **Duplication Logic**: Uses `DuplicateHandle` with the `DUPLICATE_CLOSE_SOURCE` flag to safely detach the lock from the source process.

## Safety Warning

> [!CAUTION]
> Force-closing handles is a powerful action. While this tool uses a safe querying method, closing a handle in a process that actively depends on it can lead to instability in that specific process. Only use this when you know the file is no longer needed by the holder (e.g., a zombie lock).
