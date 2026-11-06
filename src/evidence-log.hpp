#pragma once
#include "core.hpp"

#include <algorithm>
#include <cwchar>
#include <regex>

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

// Allocates loader.log in 8 MiB segments and keeps the evidence directory
// within a configurable budget. Allocation is serialized across game
// instances; the hot append path only takes the SRW lock.
class EvidenceLog {
    static constexpr DWORD segmentLimit=8*1024*1024;
    static constexpr char shutdownLine[]="process-detach shutdown=true (not an exit-code assertion)\r\n";
    fs::path run;
    HANDLE file=INVALID_HANDLE_VALUE;
    DWORD bytes=0;
    unsigned segment=0;
    ULONGLONG retryAt=0;
    SRWLOCK lock=SRWLOCK_INIT;
    U64 directoryLimit=100ULL<<20;

    struct Candidate {
        fs::path path;
        fs::file_time_type time;
        U64 size;
    };

    bool makeRoom() {
        const auto root=run.parent_path();
        const auto rootAttributes=GetFileAttributesW(root.c_str());
        if(rootAttributes==INVALID_FILE_ATTRIBUTES ||
           (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        static const std::wregex runName(L"[0-9]{8}-[0-9]{6}-[0-9]+-dll");
        static const std::wregex logName(L"loader(\\.[0-9]+)?\\.log");
        U64 total=segmentLimit;
        std::vector<Candidate> old;
        for(auto it=fs::recursive_directory_iterator(root); it!=fs::recursive_directory_iterator(); ++it) {
            const auto& path=it->path();
            const auto attributes=GetFileAttributesW(path.c_str());
            if(attributes==INVALID_FILE_ATTRIBUTES) return false;
            if(attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                it.disable_recursion_pending();
                continue;
            }
            if(!it->is_regular_file()) continue;
            const U64 size=it->file_size();
            total+=size;
            if(path.parent_path().parent_path()!=root ||
               !std::regex_match(path.parent_path().filename().wstring(),runName) ||
               !std::regex_match(path.filename().wstring(),logName)) continue;
            // Writers deny write sharing. Account for the rest of an active
            // segment and leave locked files alone.
            Handle probe(CreateFileW(path.c_str(),GENERIC_WRITE,
                                     FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                     nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
            if(probe.h==INVALID_HANDLE_VALUE) {
                if(size<segmentLimit) total+=segmentLimit-size;
                continue;
            }
            old.push_back({path,it->last_write_time(),size});
        }
        std::sort(old.begin(),old.end(),[](const Candidate& a,const Candidate& b) {
            return a.time!=b.time ? a.time<b.time : a.path<b.path;
        });
        for(const auto& item:old) {
            if(total<=directoryLimit) break;
            if(DeleteFileW(item.path.c_str())) {
                total-=item.size;
                if(item.path.parent_path()!=run) RemoveDirectoryW(item.path.parent_path().c_str());
            }
        }
        return total<=directoryLimit;
    }

    bool openSegment() {
        Handle gate(CreateMutexW(nullptr,FALSE,L"Local\\PBZModLoaderEvidenceQuota"));
        if(!gate.h) return false;
        const DWORD result=WaitForSingleObject(gate.h,1000);
        if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED) return false;
        struct Unlock { HANDLE mutex; ~Unlock(){ ReleaseMutex(mutex); } } unlock{gate.h};
        fs::create_directories(run.parent_path());
        if(!makeRoom()) return false;
        fs::create_directories(run);
        const auto attributes=GetFileAttributesW(run.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES ||
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto path=run/(segment ? L"loader."+std::to_wstring(segment)+L".log" : L"loader.log");
        file=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,
                         CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) return false;
        ++segment;
        bytes=0;
        return true;
    }

public:
    EvidenceLog()=default;
    ~EvidenceLog() { if(file!=INVALID_HANDLE_VALUE) CloseHandle(file); }
    EvidenceLog(const EvidenceLog&)=delete;
    EvidenceLog& operator=(const EvidenceLog&)=delete;

    void initialize(const fs::path& directory,const std::wstring& budget={}) {
        run=directory;
        directoryLimit=evidenceBudget(budget);
    }

    void append(const std::string& line) noexcept {
        AcquireSRWLockExclusive(&lock);
        struct Unlock { SRWLOCK* lock; ~Unlock(){ ReleaseSRWLockExclusive(lock); } } unlock{&lock};
        if(run.empty() || line.size()>segmentLimit-sizeof(shutdownLine)) return;
        if(file!=INVALID_HANDLE_VALUE && bytes+line.size()+sizeof(shutdownLine)>segmentLimit) {
            CloseHandle(file);
            file=INVALID_HANDLE_VALUE;
        }
        if(file==INVALID_HANDLE_VALUE) {
            const auto now=GetTickCount64();
            if(now<retryAt) return;
            retryAt=now+5000;
            try { if(!openSegment()) return; }
            catch(...) { return; }
            retryAt=0;
        }
        DWORD written=0;
        WriteFile(file,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);
        bytes+=written;
    }

    // Runs under loader lock during process detach; must not allocate or wait.
    void shutdown() noexcept {
        if(!TryAcquireSRWLockExclusive(&lock)) return;
        if(file!=INVALID_HANDLE_VALUE) {
            DWORD written=0;
            WriteFile(file,shutdownLine,sizeof(shutdownLine)-1,&written,nullptr);
        }
        ReleaseSRWLockExclusive(&lock);
    }
};
