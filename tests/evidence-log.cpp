// Exercise evidence log rotation and the directory budget without a game.
#include "../src/evidence-log.hpp"
#include <cassert>
#include <fstream>

static void writeSized(const fs::path& path,size_t size,char value='x') {
    std::ofstream f(path,std::ios::binary);
    std::string block(size,value);
    f.write(block.data(),static_cast<std::streamsize>(block.size()));
}

int main() {
    assert(evidenceBudget(L"")==(100ULL<<20));
    assert(evidenceBudget(L"1")==(16ULL<<20));
    assert(evidenceBudget(L"200")==(200ULL<<20));
    assert(evidenceBudget(L"9999")==(512ULL<<20));
    assert(evidenceBudget(L"nope")==(100ULL<<20));
    std::cout<<"PASS evidence budget: default 100 MiB, clamp 16-512 MiB, invalid fallback\n";

    const auto temp=fs::temp_directory_path()/(L"pbz-evidence-log-"+std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(temp);

    {
        const auto run=temp/L"evidence"/L"20200101-000000-1-dll";
        fs::create_directories(run);
        writeSized(run/L"loader.log",20*1024*1024);
        const auto current=temp/L"evidence"/L"20260101-000000-2-dll";
        EvidenceLog log;
        log.initialize(current,L"16");
        log.append("hello\r\n");
        assert(fs::exists(current/L"loader.log"));
        assert(!fs::exists(run));
        std::cout<<"PASS evidence quota: oldest run removed before allocation\n";
    }

    {
        const auto run=temp/L"evidence"/L"20260102-000000-3-dll";
        EvidenceLog log;
        log.initialize(run,L"100");
        const std::string line(1024*1024,'a');
        for(int i=0;i<9;++i) log.append(line+"\r\n");
        assert(fs::exists(run/L"loader.log"));
        assert(fs::exists(run/L"loader.1.log"));
        assert(fs::file_size(run/L"loader.log")<=8*1024*1024);
        const auto size=fs::file_size(run/L"loader.1.log");
        assert(size>0 && size<=8*1024*1024);
        log.shutdown();
        std::ifstream f(run/L"loader.1.log",std::ios::binary);
        f.seekg(-static_cast<std::streamoff>(sizeof("process-detach shutdown=true (not an exit-code assertion)\r\n")-1),std::ios::end);
        std::string tail(512,'\0');
        f.read(tail.data(),static_cast<std::streamsize>(tail.size()));
        assert(tail.find("process-detach shutdown=true")!=std::string::npos);
        std::cout<<"PASS evidence rotation: 8 MiB segments and detach marker\n";
    }

    assert(evidenceRetentionRuns(L"")==10);
    assert(evidenceRetentionRuns(L"0")==1);
    assert(evidenceRetentionRuns(L"25")==25);
    assert(evidenceRetentionRuns(L"999")==100);
    assert(evidencePinnedRuns(L"")==3);
    assert(evidencePinnedRuns(L"-1")==0);
    assert(evidencePinnedRuns(L"99")==10);
    assert(evidenceLogMode(L"off")==EvidenceLogMode::Off);
    assert(evidenceLogMode(L"ERRORS")==EvidenceLogMode::Errors);
    assert(evidenceLogMode(L"nope")==EvidenceLogMode::Full);
    assert(evidenceSegmentSize(L"1")==(1ULL<<20));
    assert(evidenceSegmentSize(L"999")==(64ULL<<20));
    std::cout<<"PASS evidence options: retention/pinned/mode/segment parsing\n";

    {
        const auto root=temp/L"evidence-retention";
        for(int i=0;i<4;++i) {
            wchar_t name[48];
            swprintf(name,48,L"2024010%d-000000-%d-dll",i+1,i+1);
            const auto old=root/name;
            fs::create_directories(old);
            writeSized(old/L"loader.log",4096,'r');
        }
        const auto current=root/L"20260103-000000-9-dll";
        EvidenceLog log;
        log.initialize(current,L"100",L"2");
        log.append("retention\r\n");
        log.shutdown();
        int kept=0;
        for(const auto& entry:fs::directory_iterator(root)) {
            if(entry.is_directory() && entry.path()!=current) ++kept;
        }
        assert(kept==1);
        std::cout<<"PASS evidence retention: ordinary runs capped at the soft target\n";
    }

    {
        const auto root=temp/L"evidence-pinned";
        for(int i=0;i<3;++i) {
            wchar_t name[48];
            swprintf(name,48,L"2024010%d-000000-%d-dll",i+1,i+1);
            const auto old=root/name;
            fs::create_directories(old);
            writeSized(old/L"loader.log",4096,'p');
            writeSized(old/L"pinned",8,'x');
        }
        const auto current=root/L"20260104-000000-9-dll";
        EvidenceLog log;
        log.initialize(current,L"100",L"10",L"full",L"1");
        log.append("pinned\r\n");
        log.shutdown();
        int kept=0;
        for(const auto& entry:fs::directory_iterator(root)) {
            if(entry.is_directory() && entry.path()!=current) ++kept;
        }
        assert(kept==1);
        std::cout<<"PASS evidence pinned: pinned runs are bounded too\n";
    }

    {
        const auto run=temp/L"evidence-errors"/L"20260105-000000-1-dll";
        EvidenceLog log;
        log.initialize(run,L"100",L"10",L"errors");
        log.append("ordinary info\r\n");
        log.append("ERROR important\r\n");
        log.shutdown();
        std::ifstream f(run/L"loader.log",std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        assert(text.find("ERROR important")!=std::string::npos);
        assert(text.find("ordinary info")==std::string::npos);
        std::cout<<"PASS evidence errors mode: only important lines are written\n";
    }

    {
        const auto run=temp/L"evidence-off"/L"20260106-000000-1-dll";
        EvidenceLog log;
        log.initialize(run,L"100",L"10",L"off");
        log.append("ignored\r\n");
        log.shutdown();
        assert(!fs::exists(run/L"loader.log"));
        assert(log.status().find("mode=off")!=std::string::npos);
        std::cout<<"PASS evidence off mode: no loader.log, status remains visible\n";
    }

    fs::remove_all(temp);
    std::cout<<"PASS evidence log\n";
    return 0;
}
