#pragma once
#include <QProcess>
#include <windows.h>

namespace kemu {
inline bool startDetachedProcess(const QString& program, const QStringList& arguments,
                                 const QString& directory, qint64* pid = nullptr) {
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(directory);
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        // A handoff process must not retain the emulator's sockets or instance lease.
        args->inheritHandles = false;
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
    });
    return process.startDetached(pid);
}
}
