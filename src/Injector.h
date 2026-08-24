#pragma once
#include <windows.h>
#include <string>

// CreateRemoteThread and LoadLibraryW injection
namespace injector
{
    // PID of the first running process whose image name matches exeName (case-insensitive), or 0 if none is running
    DWORD FindProcessId(const wchar_t* exeName);

    // Load dllPath into the target process via CreateRemoteThread(LoadLibraryW). Returns true only if LoadLibraryW returned a non-null module in the target (i.e., the DLL loaded)
    bool InjectDll(DWORD pid, const std::wstring& dllPath);
}
