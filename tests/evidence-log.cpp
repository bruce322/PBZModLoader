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

    fs::remove_all(temp);
    std::cout<<"PASS evidence log\n";
    return 0;
}
