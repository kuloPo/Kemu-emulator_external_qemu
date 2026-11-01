#pragma once
#include <QProcess>
#include <windows.h>
#include <tlhelp32.h>
#include "../../android/android-emu-base/android/base/system/Win32ChildProcess.h"

namespace kemu {
inline std::wstring quoteProcessArgument(const QString& argument) {
    const auto value = argument.toStdWString();
    if (!value.empty() && value.find_first_of(L" \t\v\n\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

inline bool startDetachedProcess(const QString& program, const QStringList& arguments,
                                 const QString& directory, qint64* pid = nullptr) {
    auto command = quoteProcessArgument(program);
    for (const auto& argument : arguments) command += L' ' + quoteProcessArgument(argument);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    // Handoff processes must not retain the engine's sockets or instance lease.
    if (!android::base::childprocess::create(program.toStdWString().c_str(), command.data(),
            nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            directory.toStdWString().c_str(), &startup, &process)) return false;
    if (pid) *pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

inline void configureChildProcess(QProcess& process) {
    const auto mask = android::base::childprocess::childMask();
    if (!mask) return;
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_SUSPENDED;
    });
    QObject::connect(&process, &QProcess::started, &process, [&process, mask] {
        const auto pid = DWORD(process.processId());
        HANDLE child = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION,
                                   FALSE, pid);
        bool ok = child && android::base::childprocess::restoreAffinity(child, mask);
        if (child) CloseHandle(child);
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        HANDLE thread = nullptr;
        THREADENTRY32 entry{sizeof(entry)};
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Thread32First(snapshot, &entry)) {
                do {
                    if (entry.th32OwnerProcessID == pid) {
                        thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                        break;
                    }
                } while (Thread32Next(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        ok = ok && thread && ResumeThread(thread) != DWORD(-1);
        if (thread) CloseHandle(thread);
        if (!ok) process.kill();
    });
}
} // namespace kemu
