#define PBZ_DLL
#include "core.hpp"
#include "mod-validation.hpp"
#include "proxy_config.hpp"
#include <atomic>

extern "C" void pbz_entry_gate();
extern "C" { void* pbz_original_entry=nullptr; }
static unsigned char entryBytes[14];
static HMODULE selfModule=nullptr;
static HANDLE logFile=INVALID_HANDLE_VALUE;
static INIT_ONCE versionOnce=INIT_ONCE_STATIC_INIT;
static HMODULE realVersion=nullptr;
static std::atomic<FARPROC> versionFunctions[proxyCount]{};
static fs::path reloadRoot,reloadScripts;
static bool (*originalValidateBytes)(const GameString*,const GameBytes*)=nullptr;
static bool (*originalValidateText)(const GameString*,const GameString*)=nullptr;
static bool validateModBytes(const GameString* path,const GameBytes* source) noexcept {
    if(!originalValidateBytes) return false;
    if(originalValidateBytes(path,source)) return true;
    try { if(trustedModBytes(reloadScripts,path,source)) return true; }
    catch(const std::exception& e) { log(std::string("MOD byte validation failed: ")+e.what()); }
    std::wstring name; if(gameString(path,name)) log("script bytes rejected: "+utf8(name));
    return false;
}
static bool validateModText(const GameString* path,const GameString* source) noexcept {
    if(!originalValidateText) return false;
    if(originalValidateText(path,source)) return true;
    try { if(trustedModText(reloadScripts,path,source)) return true; }
    catch(const std::exception& e) { log(std::string("MOD text validation failed: ")+e.what()); }
    std::wstring name; if(gameString(path,name)) log("script text rejected: "+utf8(name));
    return false;
}
static BOOL CALLBACK loadVersion(PINIT_ONCE,void*,void**) {
    wchar_t system[MAX_PATH+32]; UINT n=GetSystemDirectoryW(system,MAX_PATH);
    if(!n || n>=MAX_PATH) return FALSE;
    lstrcatW(system,PBZ_PROXY_DLL);
    realVersion=LoadLibraryExW(system,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!realVersion || realVersion==selfModule) return FALSE;
    return TRUE;
}
extern "C" FARPROC pbz_resolve_version(unsigned index) noexcept {
    const DWORD error=GetLastError();
    if(index>=proxyCount || !InitOnceExecuteOnce(&versionOnce,loadVersion,nullptr,nullptr)) {
        // A missing Windows API cannot be safely replaced with a fabricated return value.
        RaiseFailFastException(nullptr,nullptr,0);
        return nullptr;
    }
    auto function=versionFunctions[index].load(std::memory_order_relaxed);
    if(!function) {
        function=GetProcAddress(realVersion,versionNames[index]);
        // Optional exports vary across Windows versions. Resolve only the API used.
        if(!function) { RaiseFailFastException(nullptr,nullptr,0); return nullptr; }
        versionFunctions[index].store(function,std::memory_order_relaxed);
    }
    SetLastError(error); return function;
}
void log(const std::string& s) {
    if(logFile==INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t); char time[40];
    snprintf(time,sizeof(time),"[%02u:%02u:%02u.%03u] ",t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
    std::string line=std::string(time)+s+"\r\n"; DWORD written;
    // FILE_APPEND_DATA and a single WriteFile per record allow the shutdown marker
    // to use this handle without a C++ lock or allocation in DllMain.
    WriteFile(logFile,line.data(),(DWORD)line.size(),&written,nullptr);
}
static DWORD WINAPI monitor(void* context) noexcept {
    U64 slab=(U64)context,next=0,starts=0;
    bool hotkeyRegistered=false,hotkeyWarning=false;
    constexpr int hotkeyId=0x5042;
    unsigned revision=1;
    try {
        for(;;) {
            drain(GetCurrentProcess(),slab,next);
            U64 current=0; read(GetCurrentProcess(),slab+4104,&current,8);
            if(current!=starts) { starts=current; log("main-start count="+std::to_string(starts)); }
            DWORD foregroundProcess=0;
            GetWindowThreadProcessId(GetForegroundWindow(),&foregroundProcess);
            bool focused=foregroundProcess==GetCurrentProcessId();
            if(focused && !hotkeyRegistered) {
                hotkeyRegistered=RegisterHotKey(nullptr,hotkeyId,MOD_ALT|MOD_NOREPEAT,VK_F5)!=0;
                if(!hotkeyRegistered && !hotkeyWarning) {
                    log("Alt+F5 registration failed; shortcut may be in use"); hotkeyWarning=true;
                }
            } else if(!focused && hotkeyRegistered) {
                UnregisterHotKey(nullptr,hotkeyId); hotkeyRegistered=false;
            }
            if(!focused) hotkeyWarning=false;
            MSG message{};
            bool requested=false;
            while(PeekMessageW(&message,nullptr,WM_HOTKEY,WM_HOTKEY,PM_REMOVE))
                if(message.wParam==hotkeyId && focused) requested=true;
            if(requested) {
                // Compilation may take time; do not reserve another app's
                // shortcut or queue repeated requests while preparing a batch.
                UnregisterHotKey(nullptr,hotkeyId); hotkeyRegistered=false;
                try {
                    publishReload(reloadRoot,reloadScripts,revision);
                    log("hot reload published revision="+std::to_string(revision++));
                }
                catch(const std::exception& e) {
                    log(std::string("hot reload preparation failed: ")+e.what());
                    try { publishReloadFailure(reloadScripts,revision); ++revision; }
                    catch(const std::exception& failure) { log(std::string("reload failure notification: ")+failure.what()); }
                }
            }
            Sleep(50);
        }
    } catch(const std::exception& e) { log(std::string("ERROR monitor: ")+e.what()); }
    catch(...) { log("ERROR monitor: unknown exception"); }
    if(hotkeyRegistered) UnregisterHotKey(nullptr,hotkeyId);
    return 0;
}
extern "C" void pbz_initialize() noexcept {
    const DWORD savedError=GetLastError();
    // Restore the complete original entry before initialization. No instruction
    // relocation is necessary: the gate tail-jumps back to this exact address.
    DWORD old=0,unused=0;
    if(!VirtualProtect(pbz_original_entry,sizeof(entryBytes),PAGE_EXECUTE_READWRITE,&old)) {
        RaiseFailFastException(nullptr,nullptr,0); return;
    }
    std::memcpy(pbz_original_entry,entryBytes,sizeof(entryBytes));
    if(!VirtualProtect(pbz_original_entry,sizeof(entryBytes),old,&unused) ||
       !FlushInstructionCache(GetCurrentProcess(),pbz_original_entry,sizeof(entryBytes))) {
        RaiseFailFastException(nullptr,nullptr,0); return;
    }
    try {
        wchar_t exe[32768]; check(GetModuleFileNameW(nullptr,exe,32768)>0,"game path");
        const fs::path game=exe;
        const fs::path project=findProject(game);
        const fs::path root=project/L"PBZModLoader", scripts=project/L"Content/JavaScript/PBZModLoader";
        SYSTEMTIME st; GetLocalTime(&st); wchar_t name[80];
        swprintf(name,80,L"%04u%02u%02u-%02u%02u%02u-%lu-dll",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,GetCurrentProcessId());
        fs::path evidence=root/L"evidence"/name; fs::create_directories(evidence);
        logFile=CreateFileW((evidence/L"loader.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(logFile!=INVALID_HANDLE_VALUE,"create DLL log");
        log("backend=dll; startup=exe-entry; debugger-api=none");
        log("entry-restored; pid="+std::to_string(GetCurrentProcessId()));
        HMODULE pinned=nullptr;
        check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&pbz_initialize),&pinned),"pin proxy");
        check(InitOnceExecuteOnce(&versionOnce,loadVersion,nullptr,nullptr)!=FALSE,"load system proxy");
        wchar_t system[32768]; check(GetModuleFileNameW(realVersion,system,32768)>0,"system version path");
        log("proxy-system="+utf8(system));
        log("project="+utf8(project.wstring()));
        auto enabled=ini(root/L"mods.ini",L"Loader",L"Enabled");
        if(!enabled.empty() && number(enabled,1)==0) { log("MODs disabled by Loader.Enabled=0; original game continues"); SetLastError(savedError); return; }
        auto image=snapshotImage(GetCurrentProcess(),(U64)GetModuleHandleW(nullptr));
        const auto sites=resolveSites(image);
        { std::ostringstream message; message<<"signatures validated; call-rva=0x"<<std::hex<<sites.call<<" start-rva=0x"<<sites.start;
          for(auto rva:sites.loggers) message<<" logger-rva=0x"<<rva; log(message.str()); }
        for(auto file:{L"bootstrap.js",L"manager.js"}) if(!fs::is_regular_file(scripts/file)) throw std::runtime_error("Missing loader JS");
        resetReloads(scripts);
        reloadRoot=root; reloadScripts=scripts;
        syncMods(root,scripts);
        auto generated=config(root);
        auto temporary=scripts/(L"generated-config."+std::to_wstring(GetCurrentProcessId())+L".tmp");
        { std::ofstream f(temporary,std::ios::binary); f<<generated; f.close(); check(bool(f),"write config"); }
        check(MoveFileExW(temporary.c_str(),(scripts/L"generated-config.js").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"publish config");
        const auto base=(U64)GetModuleHandleW(nullptr);
        if(sites.validateBytes) {
            originalValidateBytes=reinterpret_cast<decltype(originalValidateBytes)>(base+sites.validateBytes);
            originalValidateText=reinterpret_cast<decltype(originalValidateText)>(base+sites.validateText);
        }
        auto installed=install(GetCurrentProcess(),base,image,sites,(U64)&validateModBytes,(U64)&validateModText);
        if(sites.validateBytes) log("script validation adapters installed; scope=PBZModLoader; content=local-file-exact");
        log("native hooks installed; configuration published");
        HANDLE worker=CreateThread(nullptr,0,monitor,(void*)installed.slab,0,nullptr);
        if(worker) CloseHandle(worker); else log("ERROR monitor: CreateThread failed; MOD loading remains enabled");
    } catch(const std::exception& e) { log(std::string("MODs skipped: ")+e.what()+"; original game continues"); }
    catch(...) { log("MODs skipped: unknown initialization error; original game continues"); }
    SetLastError(savedError);
}

// Called with the Windows loader lock held. No file I/O, hash, STL allocation,
// LoadLibrary, thread creation/wait, configuration or PuerTS calls occur here.
__attribute__((noinline)) static void armEntry(HMODULE module) noexcept {
    // PROCESS_ATTACH is serialized. Keep long-path buffers out of thread stacks:
    // the game creates small-stack threads that also receive DLL notifications.
    static wchar_t exe[32768],dll[32768];
    DWORD a=GetModuleFileNameW(nullptr,exe,32768),b=GetModuleFileNameW(module,dll,32768);
    if(!a || a>=32768 || !b || b>=32768) return;
    wchar_t* exeName=wcsrchr(exe,L'\\'); wchar_t* dllName=wcsrchr(dll,L'\\');
    if(!exeName || !dllName) return;
    if(exeName-exe!=dllName-dll || _wcsnicmp(exe,dll,exeName-exe)) return;
    wchar_t guardName[80];
    swprintf(guardName,80,L"Local\\PBZModLoader-entry-%lu",GetCurrentProcessId());
    HANDLE guard=CreateMutexW(nullptr,FALSE,guardName);
    DWORD guardError=GetLastError();
    if(!guard) return;
    if(guardError==ERROR_ALREADY_EXISTS) { CloseHandle(guard); return; }
    // Keep the first handle until process exit; never wait under loader lock.
    // No platform path assumptions. Unsupported wrappers fail signature validation.
    auto base=(unsigned char*)GetModuleHandleW(nullptr);
    auto dos=(IMAGE_DOS_HEADER*)base;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return;
    auto nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return;
    DWORD rva=nt->OptionalHeader.AddressOfEntryPoint;
    if(!rva || nt->OptionalHeader.SizeOfImage<sizeof(entryBytes) || rva>nt->OptionalHeader.SizeOfImage-sizeof(entryBytes)) return;
    void* entry=base+rva; DWORD old,unused;
    if(!VirtualProtect(entry,sizeof(entryBytes),PAGE_EXECUTE_READWRITE,&old)) return;
    std::memcpy(entryBytes,entry,sizeof(entryBytes)); pbz_original_entry=entry;
    unsigned char jump[14]={0xff,0x25,0,0,0,0}; auto gate=&pbz_entry_gate;
    std::memcpy(jump+6,&gate,8); std::memcpy(entry,jump,sizeof(jump));
    if(!FlushInstructionCache(GetCurrentProcess(),entry,sizeof(jump))) {
        std::memcpy(entry,entryBytes,sizeof(entryBytes)); pbz_original_entry=nullptr;
        FlushInstructionCache(GetCurrentProcess(),entry,sizeof(jump));
    }
    VirtualProtect(entry,sizeof(entryBytes),old,&unused);
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void* reserved) {
    if(reason==DLL_PROCESS_ATTACH) {
        selfModule=module;
        // Only static imports are early enough to intercept the EXE entry point.
        // Accidentally installing multiple entries must not chain entry gates.
        if(reserved) armEntry(module);
    }
    else if(reason==DLL_PROCESS_DETACH && reserved && logFile!=INVALID_HANDLE_VALUE) {
        static const char marker[]="process-detach shutdown=true (not an exit-code assertion)\r\n";
        DWORD written; WriteFile(logFile,marker,sizeof(marker)-1,&written,nullptr);
        // No joins, loader calls or C++ stream destruction here. OS closes the handle.
    }
    return TRUE;
}
