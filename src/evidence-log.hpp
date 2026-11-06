#pragma once
#include "core.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// Evidence directory budget, in MiB. Keep the default deliberately small:
// a runaway MOD can otherwise fill the disk during a long session.
inline U64 evidenceBudget(const std::wstring& value) {
    constexpr U64 defaultBudget=100ULL<<20;
    constexpr U64 minimumBudget=16ULL<<20;
    constexpr U64 maximumBudget=512ULL<<20;
    wchar_t* end=nullptr;
    const long long mib=std::wcstoll(value.c_str(),&end,10);
    if(end==value.c_str() || *end) return defaultBudget;
    if(mib<16) return minimumBudget;
    if(mib>512) return maximumBudget;
    return static_cast<U64>(mib)<<20;
}

inline unsigned evidenceRetentionRuns(const std::wstring& value) {
    if(value.empty()) return 10;
    wchar_t* end=nullptr;
    const long long runs=std::wcstoll(value.c_str(),&end,10);
    if(end==value.c_str() || *end) return 10;
    if(runs<1) return 1;
    if(runs>100) return 100;
    return static_cast<unsigned>(runs);
}

inline unsigned evidencePinnedRuns(const std::wstring& value) {
    if(value.empty()) return 3;
    wchar_t* end=nullptr;
    const long long runs=std::wcstoll(value.c_str(),&end,10);
    if(end==value.c_str() || *end) return 3;
    if(runs<0) return 0;
    if(runs>10) return 10;
    return static_cast<unsigned>(runs);
}

enum class EvidenceLogMode { Full, Errors, Off };

inline EvidenceLogMode evidenceLogMode(const std::wstring& value) {
    std::wstring lower;
    lower.reserve(value.size());
    for(const auto ch:value) lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
    if(lower==L"off") return EvidenceLogMode::Off;
    if(lower==L"errors") return EvidenceLogMode::Errors;
    return EvidenceLogMode::Full;
}

inline U64 evidenceSegmentSize(const std::wstring& value) {
    constexpr U64 defaultSize=8ULL<<20;
    wchar_t* end=nullptr;
    const long long mib=std::wcstoll(value.c_str(),&end,10);
    if(end==value.c_str() || *end) return defaultSize;
    if(mib<1) return 1ULL<<20;
    if(mib>64) return 64ULL<<20;
    return static_cast<U64>(mib)<<20;
}

inline const char* evidenceModeName(EvidenceLogMode mode) {
    switch(mode) {
        case EvidenceLogMode::Off: return "off";
        case EvidenceLogMode::Errors: return "errors";
        default: return "full";
    }
}

// Allocates loader.log in fixed-size segments. The evidence directory is a
// hard budget: cleanup runs at startup and at each segment rotation, never on
// the per-line path. A run with extra files (verification/failure artifacts)
// is pinned, but pinning is bounded so it cannot defeat the budget forever.
class EvidenceLog {
    static constexpr DWORD shutdownLineSize=sizeof("process-detach shutdown=true (not an exit-code assertion)\r\n")-1;
    static constexpr char shutdownLine[]="process-detach shutdown=true (not an exit-code assertion)\r\n";
    static constexpr size_t bufferLimit=8*1024;
    fs::path run;
    HANDLE file=INVALID_HANDLE_VALUE;
    U64 directoryLimit=100ULL<<20;
    U64 segmentLimit=8ULL<<20;
    unsigned segment=0;
    unsigned runLimit=10;
    unsigned pinnedLimit=3;
    EvidenceLogMode mode=EvidenceLogMode::Full;
    DWORD bytes=0;
    ULONGLONG retryAt=0;
    SRWLOCK lock=SRWLOCK_INIT;
    std::string pending;
    U64 lastTotal=0;
    unsigned lastRuns=0;
    unsigned lastPinned=0;
    bool degraded=false;

    struct RunInfo {
        fs::path path;
        std::wstring name;
        U64 size=0;
        bool pinned=false;
        bool active=false;
        bool removed=false;
    };

    static bool isLogName(const std::wstring& name) {
        static const std::wregex re(L"loader(\\.[0-9]+)?\\.log");
        return std::regex_match(name,re);
    }

    static bool isRunName(const std::wstring& name) {
        static const std::wregex re(L"[0-9]{8}-[0-9]{6}-[0-9]+-dll");
        return std::regex_match(name,re);
    }

    static bool importantLine(const std::string& line) {
        return line.find("ERROR")!=std::string::npos ||
               line.find("failed")!=std::string::npos ||
               line.find("MODs skipped")!=std::string::npos ||
               line.find("WARN")!=std::string::npos ||
               line.find("reload blocked")!=std::string::npos;
    }

    bool scanRun(RunInfo& info) const noexcept {
        try {
            for(auto it=fs::recursive_directory_iterator(info.path); it!=fs::recursive_directory_iterator(); ++it) {
                const auto& path=it->path();
                const auto attributes=GetFileAttributesW(path.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES) return false;
                if(attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    it.disable_recursion_pending();
                    continue;
                }
                if(!it->is_regular_file()) continue;
                info.size+=it->file_size();
                const auto name=path.filename().wstring();
                if(name==L"pinned") info.pinned=true;
                else if(name!=L"run.json" && !isLogName(name)) info.pinned=true;
            }
            return true;
        } catch(...) {
            return false;
        }
    }

    static bool activeRun(const fs::path& path) noexcept {
        try {
            for(const auto& entry:fs::directory_iterator(path)) {
                if(!entry.is_regular_file()) continue;
                const auto name=entry.path().filename().wstring();
                if(!isLogName(name)) continue;
                Handle probe(CreateFileW(entry.path().c_str(),GENERIC_WRITE,
                    FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                    nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
                if(probe.h==INVALID_HANDLE_VALUE) {
                    const auto error=GetLastError();
                    if(error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION) return true;
                }
            }
            return false;
        } catch(...) {
            return true;
        }
    }

    bool makeRoom() noexcept {
        try {
            const auto root=run.parent_path();
            if(root.empty()) return true;
            const auto rootAttributes=GetFileAttributesW(root.c_str());
            if(rootAttributes==INVALID_FILE_ATTRIBUTES ||
               (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                degraded=true;
                return false;
            }
            std::vector<RunInfo> runs;
            for(const auto& entry:fs::directory_iterator(root)) {
                if(!entry.is_directory()) continue;
                const auto name=entry.path().filename().wstring();
                if(!isRunName(name)) continue;
                RunInfo info;
                info.path=entry.path();
                info.name=name;
                if(!scanRun(info)) {
                    info.active=true;
                    runs.push_back(std::move(info));
                    continue;
                }
                info.active=info.path!=run && activeRun(info.path);
                runs.push_back(std::move(info));
            }
            std::sort(runs.begin(),runs.end(),[](const RunInfo& a,const RunInfo& b) {
                return a.name<b.name;
            });

            auto normalCount=[&]() {
                unsigned count=0;
                for(const auto& info:runs) if(!info.removed && !info.pinned) ++count;
                return count;
            };
            auto pinnedCount=[&]() {
                unsigned count=0;
                for(const auto& info:runs) if(!info.removed && info.pinned) ++count;
                return count;
            };
            auto dropOldest=[&](bool pinned) {
                for(auto& info:runs) {
                    if(info.removed || info.pinned!=pinned || info.path==run || info.active) continue;
                    std::error_code error;
                    fs::remove_all(info.path,error);
                    info.removed=true;
                    return !error;
                }
                return false;
            };

            while(normalCount()>runLimit && dropOldest(false)) {}
            while(pinnedCount()>pinnedLimit && dropOldest(true)) {}

            U64 total=0;
            for(const auto& info:runs) if(!info.removed) total+=info.size;
            while(total>directoryLimit) {
                RunInfo* pick=nullptr;
                for(auto& info:runs) {
                    if(info.removed || info.pinned || info.path==run || info.active) continue;
                    pick=&info;
                    break;
                }
                if(!pick) {
                    for(auto& info:runs) {
                        if(info.removed || !info.pinned || info.path==run || info.active) continue;
                        pick=&info;
                        break;
                    }
                }
                if(!pick) {
                    degraded=true;
                    break;
                }
                const auto removedSize=pick->size;
                std::error_code error;
                fs::remove_all(pick->path,error);
                pick->removed=true;
                if(!error) total-=removedSize;
            }

            lastTotal=total;
            lastRuns=normalCount();
            lastPinned=pinnedCount();
            if(total>directoryLimit) degraded=true;
            return total<=directoryLimit;
        } catch(...) {
            degraded=true;
            return false;
        }
    }

    void writeRunJson() noexcept {
        try {
            std::ofstream f(run/L"run.json",std::ios::binary|std::ios::trunc);
            if(!f) return;
            f<<"{\"version\":1,\"pid\":"<<GetCurrentProcessId()
             <<",\"budget\":"<<directoryLimit
             <<",\"retention\":"<<runLimit
             <<",\"pinned\":"<<pinnedLimit
             <<",\"mode\":\""<<evidenceModeName(mode)<<"\"}\n";
        } catch(...) {}
    }

    bool openSegment() noexcept {
        if(file!=INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file=INVALID_HANDLE_VALUE;
        }
        try { fs::create_directories(run); } catch(...) { return false; }
        const auto attributes=GetFileAttributesW(run.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            degraded=true;
            return false;
        }
        if(!makeRoom()) return false;
        try {
            if(!fs::exists(run/L"run.json")) writeRunJson();
        } catch(...) {}
        while(segment<1000) {
            const auto path=run/(segment ? L"loader."+std::to_wstring(segment)+L".log" : L"loader.log");
            file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,
                             CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file!=INVALID_HANDLE_VALUE) {
                ++segment;
                bytes=0;
                pending.clear();
                return true;
            }
            if(GetLastError()!=ERROR_FILE_EXISTS) return false;
            ++segment;
        }
        return false;
    }

    bool ensureOpen() noexcept {
        if(file!=INVALID_HANDLE_VALUE) return true;
        const auto now=GetTickCount64();
        if(now<retryAt) return false;
        retryAt=now+5000;
        if(!openSegment()) return false;
        retryAt=0;
        return true;
    }

    void flushLocked() noexcept {
        if(pending.empty() || file==INVALID_HANDLE_VALUE) return;
        DWORD written=0;
        if(WriteFile(file,pending.data(),static_cast<DWORD>(pending.size()),&written,nullptr)) {
            bytes+=written;
            if(written<pending.size()) pending.erase(0,written);
            else pending.clear();
        } else {
            pending.clear();
            CloseHandle(file);
            file=INVALID_HANDLE_VALUE;
            retryAt=GetTickCount64()+5000;
        }
    }

public:
    EvidenceLog()=default;
    ~EvidenceLog() { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); }
    EvidenceLog(const EvidenceLog&)=delete;
    EvidenceLog& operator=(const EvidenceLog&)=delete;

    void initialize(const fs::path& directory,const std::wstring& budget={},
                    const std::wstring& retention={},const std::wstring& modeValue={},
                    const std::wstring& pinned={},const std::wstring& segmentMiB={}) {
        run=directory;
        directoryLimit=evidenceBudget(budget);
        runLimit=evidenceRetentionRuns(retention);
        pinnedLimit=evidencePinnedRuns(pinned);
        mode=evidenceLogMode(modeValue);
        segmentLimit=evidenceSegmentSize(segmentMiB);
        degraded=false;
        lastTotal=0;
        lastRuns=0;
        lastPinned=0;
        try { fs::create_directories(run); } catch(...) { degraded=true; return; }
        const auto attributes=GetFileAttributesW(run.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            degraded=true;
            return;
        }
        writeRunJson();
        makeRoom();
    }

    bool enabled() const { return mode!=EvidenceLogMode::Off && !degraded; }

    void append(const std::string& line) noexcept {
        if(mode==EvidenceLogMode::Off || degraded || run.empty()) return;
        if(mode==EvidenceLogMode::Errors && !importantLine(line)) return;
        if(line.size()>segmentLimit-shutdownLineSize) return;
        AcquireSRWLockExclusive(&lock);
        struct Unlock { SRWLOCK* lock; ~Unlock(){ ReleaseSRWLockExclusive(lock); } } unlock{&lock};
        if(!ensureOpen()) return;
        if(bytes+pending.size()+line.size()+shutdownLineSize>segmentLimit) {
            flushLocked();
            if(file!=INVALID_HANDLE_VALUE) {
                CloseHandle(file);
                file=INVALID_HANDLE_VALUE;
            }
            if(!openSegment()) return;
        }
        pending+=line;
        if(pending.size()>=bufferLimit || importantLine(line)) flushLocked();
    }

    void pin(const char* reason=nullptr) noexcept {
        if(run.empty()) return;
        try {
            std::ofstream f(run/L"pinned",std::ios::binary|std::ios::trunc);
            if(f) f<<(reason ? reason : "pinned")<<"\n";
        } catch(...) {}
    }

    // Runs under loader lock during process detach; must not allocate or wait.
    void shutdown() noexcept {
        if(TryAcquireSRWLockExclusive(&lock)) {
            flushLocked();
            if(file!=INVALID_HANDLE_VALUE && mode!=EvidenceLogMode::Off) {
                DWORD written=0;
                WriteFile(file,shutdownLine,shutdownLineSize,&written,nullptr);
            }
            ReleaseSRWLockExclusive(&lock);
        }
    }

    std::string status() const {
        std::ostringstream text;
        text<<"mode="<<evidenceModeName(mode)
            <<" limit="<<directoryLimit
            <<" bytes="<<lastTotal
            <<" runs="<<lastRuns
            <<" pinned="<<lastPinned
            <<" degraded="<<(degraded ? 1 : 0);
        return text.str();
    }
};
