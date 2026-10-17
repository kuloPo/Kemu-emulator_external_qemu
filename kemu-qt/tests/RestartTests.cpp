#include <winsock2.h>
#include <windows.h>
#include "../src/DetachedProcess.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void write(const fs::path& file, const std::string& text) {
    std::ofstream out(file); out << text; out.close(); require(bool(out), "Cannot write restart fixture");
}
static std::string read(const fs::path& file) {
    std::ifstream in(file); return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
struct Socket {
    SOCKET handle = INVALID_SOCKET;
    ~Socket() { if (handle!=INVALID_SOCKET) closesocket(handle); }
    void listenOn(int port) {
        handle=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        require(handle!=INVALID_SOCKET,"Cannot create test socket");
        BOOL exclusive=TRUE;
        require(setsockopt(handle,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive))==0,"Cannot make test socket exclusive");
        sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK); address.sin_port=htons(u_short(port));
        require(bind(handle,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"Test emulator port is occupied");
        require(listen(handle,1)==0,"Cannot listen on test port");
        require(SetHandleInformation(reinterpret_cast<HANDLE>(handle),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"Cannot mark test socket inheritable");
    }
};
struct Handle {
    HANDLE value=nullptr;
    ~Handle() { if(value) CloseHandle(value); }
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    DWORD restartPid=0;
    try {
        WSADATA network{}; require(WSAStartup(MAKEWORD(2,2),&network)==0,"Cannot initialize test networking");
        const auto args=app.arguments();
        const fs::path self(app.applicationFilePath().toStdWString());
        if (self.filename()==L"qemu-system-x86_64.exe") {
            const auto root=self.parent_path().parent_path().parent_path();
            Socket console,adb; console.listenOn(5554); adb.listenOn(5555);
            const auto mutex=QString::fromStdString(read(root/"lease-name")).toStdWString();
            Handle inherited{OpenMutexW(SYNCHRONIZE,FALSE,mutex.c_str())};
            require(!inherited.value,"Restart retained the old emulator instance lease");
            require(args.value(args.indexOf("-cores")+1)=="1" && args.value(args.indexOf("-memory")+1)=="4096","Restart did not apply saved CPU/RAM settings");
            const fs::path avd(qEnvironmentVariable("ANDROID_AVD_HOME").toStdWString());
            const auto config=read(avd/"kemu.avd/config.ini");
            require(config.find("hw.lcd.vsync=120")!=std::string::npos && config.find("hw.lcd.width=1280")!=std::string::npos,"Restart did not apply display settings");
            write(root/"restarted","yes"); return 0;
        }
        if (args.value(1)=="--old-emulator") {
            const fs::path root(args.at(2).toStdWString());
            Socket console,adb; console.listenOn(5554); adb.listenOn(5555);
            const auto mutex=L"Local\\KemuRestartTest-"+std::to_wstring(GetCurrentProcessId());
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            Handle lease{CreateMutexW(&security,FALSE,mutex.c_str())}; require(lease.value,"Cannot create test instance lease");
            write(root/"lease-name",QString::fromStdWString(mutex).toStdString());
            qint64 pid=0;
            require(kemu::startDetachedProcess(QString::fromStdWString((root/"Kemu.exe").wstring()),{"--restart-pid",QString::number(GetCurrentProcessId())},args.at(2),&pid),"Cannot launch actual restart helper");
            write(root/"launcher-pid",std::to_string(pid));
            for(int i=0;i<200 && !fs::exists(root/"exit-old");++i) Sleep(50);
            require(fs::exists(root/"exit-old"),"Test parent was not released");
            return 0;
        }
        require(args.size()==2,"Pass the compiled Kemu.exe path");
        QTemporaryDir temporary; require(temporary.isValid(),"Cannot create restart fixture");
        const fs::path root(temporary.path().toStdWString());
        fs::create_directories(root/"system-img"); fs::create_directories(root/"qemu/windows-x86_64");
        fs::copy_file(fs::path(args.at(1).toStdWString()),root/"Kemu.exe");
        fs::copy_file(self,root/"qemu/windows-x86_64/qemu-system-x86_64.exe");
        for(const auto* name:{"system.img","ramdisk.img","kernel-ranchu","userdata.img","build.prop","kernel_cmdline.txt","VerifiedBootParams.textproto"}) write(root/"system-img"/name,"fixture");
        write(root/"system-img/advancedFeatures.ini","EncryptUserData=off\n");
        fs::create_directories(root/"local/Kemu/Kemu/avd");
        write(root/"local/Kemu/Kemu/avd/performance.ini","[performance]\ncores=1\nramGb=4\nresolution=0\nfpsMode=4\n");
        qputenv("LOCALAPPDATA",QDir(temporary.path()).filePath("local").toUtf8());
        QProcess old; old.start(app.applicationFilePath(),{"--old-emulator",temporary.path()});
        require(old.waitForStarted(5000),"Cannot start old-emulator fixture");
        for(int i=0;i<100 && !fs::exists(root/"launcher-pid");++i) Sleep(50);
        if (!fs::exists(root/"launcher-pid")) { old.waitForFinished(1000); throw std::runtime_error("No restart launcher: "+old.readAllStandardError().toStdString()); }
        restartPid=std::stoul(read(root/"launcher-pid"));
        Handle launcher{OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,restartPid)};
        require(launcher.value,"Cannot observe restart launcher");
        Sleep(300); require(!fs::exists(root/"restarted"),"Restart launched before the old emulator exited");
        write(root/"exit-old","yes");
        require(old.waitForFinished(10000) && old.exitCode()==0,"Old emulator did not exit cleanly");
        require(WaitForSingleObject(launcher.value,15000)==WAIT_OBJECT_0,"Restart launcher remained blocked after parent exit");
        DWORD code=1; require(GetExitCodeProcess(launcher.value,&code) && code==0,"Restart launcher failed");
        require(fs::exists(root/"restarted"),"New emulator did not acquire both ports");
        restartPid=0; WSACleanup();
        std::cout<<"PASS: actual launcher waits for parent exit, releases inherited sockets/lease, and applies saved performance settings\n";
        return 0;
    } catch(const std::exception& error) {
        if(restartPid) { Handle failed{OpenProcess(PROCESS_TERMINATE,FALSE,restartPid)}; if(failed.value) TerminateProcess(failed.value,1); }
        std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1;
    }
}
