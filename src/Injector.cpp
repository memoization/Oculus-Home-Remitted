#define WIN32_LEAN_AND_MEAN
#include "Injector.h"
#include <tlhelp32.h>
#include "HomeLogger.h"

namespace injector
{

    DWORD FindProcessId(const wchar_t* exeName)
    {
        DWORD pid = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;

        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do
            {
                if (_wcsicmp(entry.szExeFile, exeName) == 0)
                {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return pid;
    }

    bool InjectDll(DWORD pid, const std::wstring& dllPath)
    {
        HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!process)
        {
            // Common early in the target's startup (handle not yet grantable) - caller retries.
            return false;
        }

        bool ok = false;
        SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        LPVOID remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (remote)
        {
            if (WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr))
            {
                HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
                auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));

                HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
                if (thread)
                {
                    WaitForSingleObject(thread, INFINITE);
                    DWORD exitCode = 0;
                    GetExitCodeThread(thread, &exitCode);

                    // exitCode is the low 32 bits of the returned HMODULE, and 0 means load failed.
                    ok = (exitCode != 0);
                    CloseHandle(thread);
                }
            }
            VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        }

        CloseHandle(process);
        return ok;
    }

}
