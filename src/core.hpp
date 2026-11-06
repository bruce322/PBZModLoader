#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <array>
#include <atomic>
#include <set>
#include <map>
#include <regex>
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using U64 = unsigned long long;
constexpr size_t SLAB_SIZE=8192+256*2048;
#ifdef PBZ_DLL
void log(const std::string& s);
#else
std::ofstream output;
void log(const std::string& s) { std::cout<<s<<std::endl; if(output) { output<<s<<'\n'; output.flush(); } }
#endif
void check(bool ok,const char* op) { if(!ok) throw std::runtime_error(std::string(op)+" error="+std::to_string(GetLastError())); }
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE v=nullptr):h(v){}
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
std::string utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr); return r;
}
std::string sha256(const fs::path& p) {
    BCRYPT_ALG_HANDLE a=nullptr; BCRYPT_HASH_HANDLE h=nullptr;
    if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("BCrypt provider");
    if(BCryptCreateHash(a,&h,nullptr,0,nullptr,0,0)<0) { BCryptCloseAlgorithmProvider(a,0); throw std::runtime_error("BCrypt hash"); }
    std::ifstream f(p,std::ios::binary); std::array<char,65536> b{}; unsigned char digest[32]; bool ok=bool(f);
    while(f) { f.read(b.data(),b.size()); if(BCryptHashData(h,(PUCHAR)b.data(),(ULONG)f.gcount(),0)<0) ok=false; }
    ok=ok && f.eof() && BCryptFinishHash(h,digest,32,0)>=0;
    BCryptDestroyHash(h); BCryptCloseAlgorithmProvider(a,0);
    if(!ok) throw std::runtime_error("Hash read failed");
    std::ostringstream s; for(auto v:digest) s<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)v; return s.str();
}
std::wstring ini(const fs::path& p,const std::wstring& section,const wchar_t* key) {
    wchar_t buf[4096]; DWORD n=GetPrivateProfileStringW(section.c_str(),key,L"",buf,4096,p.c_str());
    if(n>=4095) throw std::runtime_error("Config value too long"); return std::wstring(buf,n);
}
bool utf8Bom(const fs::path& p) {
    std::ifstream f(p,std::ios::binary); unsigned char b[3]{};
    return bool(f.read(reinterpret_cast<char*>(b),sizeof(b))) && b[0]==0xEF && b[1]==0xBB && b[2]==0xBF;
}
int number(const std::wstring& s,int max) {
    if(s.empty() || s.find_first_not_of(L"0123456789")!=std::wstring::npos) throw std::runtime_error("Invalid numeric config");
    auto n=std::stoul(s); if(n>(unsigned)max) throw std::runtime_error("Config number out of range"); return (int)n;
}
struct ModInfo { std::string id,entry; bool enabled; };
bool isReparsePoint(const fs::path& p) {
    auto attributes=GetFileAttributesW(p.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT);
}
std::string config(const fs::path& root,const std::string& loadPrefix,std::vector<ModInfo>* out=nullptr) {
    std::string prefix=loadPrefix;
    if(!prefix.empty() && prefix.back()!='/') prefix+='/';
    if(isReparsePoint(root)) throw std::runtime_error("PBZModLoader directory must not be a reparse point");
    fs::path p=root/L"mods.ini";
    if(isReparsePoint(p)) throw std::runtime_error("mods.ini must not be a reparse point");
    if(!fs::is_regular_file(p)) throw std::runtime_error("Missing mods.ini");
    if(utf8Bom(p)) throw std::runtime_error("mods.ini has a UTF-8 BOM; save as UTF-8 without BOM or ANSI");
    if(ini(p,L"Loader",L"Version")!=L"1") throw std::runtime_error("Unsupported config version");
    std::map<std::string,std::string> discovered;
    const fs::path mods=root/L"mods";
    if(isReparsePoint(mods)) throw std::runtime_error("mods directory must not be a reparse point");
    if(fs::is_directory(mods)) for(const auto& item:fs::directory_iterator(mods)) {
        auto id=utf8(item.path().filename().wstring());
        if(isReparsePoint(item.path()) || isReparsePoint(item.path()/L"main.js"))
            throw std::runtime_error("MOD must not contain reparse points: "+id);
        if(!item.is_directory() || !std::regex_match(id,std::regex("[A-Za-z0-9_-]{1,64}")) ||
           !fs::is_regular_file(item.path()/L"main.js")) continue;
        for(const auto& [seen,_]:discovered) if(_stricmp(seen.c_str(),id.c_str())==0)
            throw std::runtime_error("Duplicate MOD id: "+id);
        discovered.emplace(id,"mods/"+id+"/main.js");
    }
    if(discovered.size()>128) throw std::runtime_error("Too many MODs (maximum 128)");
    std::string result="'use strict';\nmodule.exports = {\"version\":1,\"mods\":[";
    size_t index=0;
    for(const auto& [id,entry]:discovered) {
        auto enabledValue=ini(p,L"Mods",fs::u8path(id).wstring().c_str());
        bool enabled=enabledValue.empty() || number(enabledValue,1)!=0;
        auto published=prefix+entry.substr(5);
        if(published.size()>512) throw std::runtime_error("MOD entry path too long: "+id);
        if(enabled) {
            auto file=fs::weakly_canonical(root/fs::u8path(entry));
            auto relative=file.lexically_relative(fs::canonical(root/L"mods"));
            if(relative.empty() || *relative.begin()==L".." || !fs::is_regular_file(file)) throw std::runtime_error("Missing or escaped entry: "+entry);
        }
        if(index++) result+=",";
        result+="{\"id\":\""+id+"\",\"enabled\":"+(enabled?"true":"false")+",\"entry\":\""+published+"\"}";
        if(out) out->push_back({id,entry,enabled});
    }
    return result+"]};\n";
}
// PuerTS still reads from its JavaScript search root. This disposable cache is
// only used for hot-reload snapshots; startup config loads MODs in place.
void syncMods(const fs::path& root,const fs::path& scripts) {
    const auto source=root/L"mods", cache=scripts/L"generated-mods";
    auto rejectLink=[](const fs::path& path) {
        auto attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("MOD source/cache must not contain reparse points");
    };
    rejectLink(source); rejectLink(cache);
    if(fs::exists(source)) for(const auto& item:fs::recursive_directory_iterator(source)) rejectLink(item.path());
    if(fs::exists(cache)) for(const auto& item:fs::recursive_directory_iterator(cache)) rejectLink(item.path());
    fs::remove_all(cache); fs::create_directories(cache);
    if(!fs::exists(source)) return;
    for(const auto& item:fs::recursive_directory_iterator(source)) {
        rejectLink(item.path());
        auto relative=item.path().lexically_relative(source);
        auto destination=cache/relative;
        if(item.is_directory()) { fs::create_directories(destination); continue; }
        if(!item.is_regular_file()) continue;
        fs::create_directories(destination.parent_path());
        std::error_code error;
        fs::copy_file(item.path(),destination,fs::copy_options::overwrite_existing,error);
        if(error) throw std::runtime_error("MOD cache materialization failed: "+error.message());
    }
}
void resetGenerated(const fs::path& scripts) {
    auto cache=scripts/L"generated-mods";
    auto reject=[](const fs::path& path) {
        auto attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Generated cache must not contain reparse points");
    };
    reject(cache);
    if(fs::exists(cache)) for(const auto& item:fs::recursive_directory_iterator(cache)) reject(item.path());
    fs::remove_all(cache);
}


void resetReloads(const fs::path& scripts) {
    auto cache=scripts/L"generated-reloads";
    auto reject=[](const fs::path& p) {
        auto a=GetFileAttributesW(p.c_str());
        if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Hot reload cache contains reparse point");
    };
    reject(cache);
    if(fs::exists(cache)) for(const auto& item:fs::recursive_directory_iterator(cache)) reject(item.path());
    fs::remove_all(cache); fs::create_directories(cache);
}

void publishReload(const fs::path& root,const fs::path& scripts,unsigned revision) {
    auto cache=scripts/L"generated-reloads";
    auto stage=cache/L"pending";
    fs::create_directories(stage);
    // Owned staging directory; reuse syncMods' reparse checks before cleanup.
    syncMods(root,stage);
    auto generated=config(root,"./generated-reloads/"+std::to_string(revision)+"/generated-mods/");
    auto enabled=ini(root/L"mods.ini",L"Loader",L"Enabled");
    if(!enabled.empty() && number(enabled,1)==0) generated="module.exports={version:1,mods:[]};\n";
    { std::ofstream f(stage/L"config.js",std::ios::binary); f<<generated; f.close(); check(bool(f),"write reload config"); }
    fs::rename(stage,cache/std::to_wstring(revision));
}

void publishReloadFailure(const fs::path& scripts,unsigned revision) {
    auto cache=scripts/L"generated-reloads", stage=cache/L"pending";
    fs::create_directories(stage);
    { std::ofstream f(stage/L"config.js",std::ios::binary);
      f<<"module.exports={error:'Reload preparation failed; see loader.log'};\n";
      f.close(); check(bool(f),"write reload failure"); }
    fs::rename(stage,cache/std::to_wstring(revision));
}

#ifdef PBZ_DLL
void read(HANDLE,U64 address,void* dest,size_t n) {
    size_t done=0;
    while(done<n) {
        MEMORY_BASIC_INFORMATION region{};
        check(VirtualQuery((void*)(address+done),&region,sizeof(region))!=0,"query image memory");
        if(region.State!=MEM_COMMIT || (region.Protect&(PAGE_NOACCESS|PAGE_GUARD))) throw std::runtime_error("Unreadable image memory");
        size_t count=std::min(n-done,(size_t)((U64)region.BaseAddress+region.RegionSize-address-done));
        if(!count) throw std::runtime_error("Invalid image region");
        std::memcpy((unsigned char*)dest+done,(void*)(address+done),count); done+=count;
    }
}
void write(HANDLE,U64 address,const void* data,size_t n) { std::memcpy((void*)address,data,n); }
BOOL protect(HANDLE,void* address,size_t n,DWORD flags,DWORD* old) { return VirtualProtect(address,n,flags,old); }
void* allocate(HANDLE,void* address,size_t n) { return VirtualAlloc(address,n,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); }
BOOL release(HANDLE,void* address) { return VirtualFree(address,0,MEM_RELEASE); }
#else
void read(HANDLE p,U64 address,void* dest,size_t n) { SIZE_T done=0; check(ReadProcessMemory(p,(void*)address,dest,n,&done)&&done==n,"read memory"); }
void write(HANDLE p,U64 address,const void* data,size_t n) { SIZE_T done=0; check(WriteProcessMemory(p,(void*)address,data,n,&done)&&done==n,"write memory"); }
BOOL protect(HANDLE p,void* address,size_t n,DWORD flags,DWORD* old) { return VirtualProtectEx(p,address,n,flags,old); }
void* allocate(HANDLE p,void* address,size_t n) { return VirtualAllocEx(p,address,n,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); }
BOOL release(HANDLE p,void* address) { return VirtualFreeEx(p,address,0,MEM_RELEASE); }
#endif
void patch(HANDLE p,U64 address,const Bytes& bytes) {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    struct Saved { void* address; size_t size; DWORD protection; };
    std::vector<Saved> saved; U64 cursor=address, end=address+bytes.size(); DWORD unused=0;
    try {
        while(cursor<end) {
            U64 pageEnd=(cursor/si.dwPageSize+1)*si.dwPageSize;
            size_t size=(size_t)((end<pageEnd?end:pageEnd)-cursor); DWORD old=0;
            check(protect(p,(void*)cursor,size,PAGE_EXECUTE_READWRITE,&old),"patch protect");
            saved.push_back({(void*)cursor,size,old}); cursor+=size;
        }
        write(p,address,bytes.data(),bytes.size());
    }
    catch(...) {
        for(auto& s:saved) protect(p,s.address,s.size,s.protection,&unused);
        throw;
    }
    for(auto& s:saved) check(protect(p,s.address,s.size,s.protection,&unused),"restore protect");
    check(FlushInstructionCache(p,(void*)address,bytes.size()),"flush instructions");
}
void imm64(Bytes& b,U64 n) { for(int i=0;i<8;++i) b.push_back((unsigned char)(n>>(i*8))); }
void imm32(Bytes& b,int n) { for(int i=0;i<4;++i) b.push_back((unsigned char)((unsigned)n>>(i*8))); }
void rel32(Bytes& b,U64 from,U64 to) {
    auto d=(long long)to-(long long)from; if(d<INT32_MIN||d>INT32_MAX) throw std::runtime_error("rel32 out of range"); imm32(b,(int)d);
}
void absoluteJump(Bytes& b,U64 target) { b.insert(b.end(),{0xff,0x25,0,0,0,0}); imm64(b,target); }
#include "signatures.hpp"
bool mappedReadable(U64 at,size_t size) {
    constexpr DWORD readable=PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY;
    U64 end=at+size; if(end<at) return false;
    while(at<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery((LPCVOID)(UINT_PTR)at,&info,sizeof(info))) return false;
        if(info.State!=MEM_COMMIT || (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !(info.Protect&readable)) return false;
        U64 next=(U64)(UINT_PTR)info.BaseAddress+info.RegionSize;
        if(next<=at) return false;
        at=next;
    }
    return true;
}
// Called from the machine-code logger hook, never from DllMain. VirtualQuery
// guards the FString object and payload so a changed PuerTS ABI skips one log
// record instead of dereferencing a stale pointer on the game thread.
struct LoggerString { const wchar_t* data; int count; int capacity; };
extern "C" inline void pbzCaptureLog(const void* object,U64 ring,int captureAll) noexcept {
    if(!object || !ring) return;
    if(!mappedReadable((U64)object,sizeof(LoggerString))) return;
    const auto text=static_cast<const LoggerString*>(object);
    if(!text->data || text->count<(captureAll?1:5)) return;
    int length=text->count-1;
    if(length>1000) length=1000;
    if(length<0) return;
    const size_t bytes=(size_t)length*sizeof(wchar_t);
    if(bytes && !mappedReadable((U64)text->data,bytes)) return;
    if(!captureAll && (length<4 || text->data[0]!=L'[' || text->data[1]!=L'P' || text->data[2]!=L'B' || text->data[3]!=L'Z')) return;
    auto counter=reinterpret_cast<std::atomic<U64>*>(ring);
    const U64 old=counter->fetch_add(1,std::memory_order_relaxed);
    auto record=reinterpret_cast<unsigned char*>(ring+4096+(old&255)*2048);
    *reinterpret_cast<U64*>(record)=0;
    *reinterpret_cast<unsigned int*>(record+8)=(unsigned)length;
    if(length) std::memcpy(record+16,text->data,bytes);
    reinterpret_cast<std::atomic<U64>*>(record)->store(old+1,std::memory_order_release);
}
// Return a non-owning view of an image already mapped into this process. The
// previous implementation allocated and copied SizeOfImage bytes at startup;
// this version only records readable section ranges and reads headers.
ImageView snapshotImage(HANDLE p,U64 base) {
    if(p!=GetCurrentProcess()) throw std::runtime_error("Mapped image view requires current process");
    IMAGE_DOS_HEADER dos{}; read(p,base,&dos,sizeof(dos));
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x100000) throw std::runtime_error("Invalid PE DOS header");
    IMAGE_NT_HEADERS64 nt{}; read(p,base+dos.e_lfanew,&nt,sizeof(nt));
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.SizeOfOptionalHeader!=sizeof(IMAGE_OPTIONAL_HEADER64) ||
       !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96 ||
       !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage>1024*1024*1024)
        throw std::runtime_error("Unsupported PE image");
    const U64 imageSize=nt.OptionalHeader.SizeOfImage;
    ImageView image{(const unsigned char*)base,base,{}};
    U64 table=dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader;
    if(table+nt.FileHeader.NumberOfSections*sizeof(IMAGE_SECTION_HEADER)>nt.OptionalHeader.SizeOfHeaders) throw std::runtime_error("Invalid PE section table");
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER s{}; read(p,base+table+i*sizeof(s),&s,sizeof(s));
        size_t size=s.Misc.VirtualSize;
        if(s.VirtualAddress>imageSize || size>imageSize-s.VirtualAddress) throw std::runtime_error("PE section exceeds image");
        if(!size || !(s.Characteristics&IMAGE_SCN_MEM_READ) || (s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE)) continue;
        for(auto other:image.sections) if(s.VirtualAddress<other.begin+other.size && other.begin<s.VirtualAddress+size) throw std::runtime_error("Overlapping PE sections");
        if(!mappedReadable(base+s.VirtualAddress,size)) throw std::runtime_error("PE section is not readable");
        image.sections.push_back({s.VirtualAddress,size,s.Characteristics});
    }
    return image;
}
U64 allocateNear(HANDLE p,U64 callAddress) {
    // 64 KiB aligned allocations, all hooks and relocated operands must fit rel32.
    U64 center=callAddress&~U64(65535);
    for(U64 d=0x10000;d<0x70000000;d+=0x10000) {
        for(U64 addr:{center+d,center-d}) {
            auto v=allocate(p,(void*)addr,SLAB_SIZE);
            if(v) return (U64)v;
        }
    }
    throw std::runtime_error("No nearby allocation");
}
// Leaf trampoline: no foreign DLL, CRT or UE allocation ownership. Original argv untouched.
Bytes startStub(U64 slab,U64 target) {
    Bytes b={0x9c,0x48,0xff,0x05}; rel32(b,slab+8,slab+4096+8); // pushfq; inc qword [counter]
    b.push_back(0x9d); b.insert(b.end(),{0x48,0xba}); imm64(b,slab+4128); // popfq; mov rdx,FString*
    absoluteJump(b,target); return b;
}
Bytes loggerStub(U64 at,U64 ring,U64 original,const Bytes& stolen,bool captureAll,U64 capture) {
    // Preserve flags and every register touched. The helper validates the FString
    // before reading it; the stub only replays the stolen comparison and jumps.
    Bytes b={0x9c,0x41,0x50,0x50,0x51,0x52,0x56,0x57,0x41,0x51,0x41,0x52,0x41,0x53};
    // The helper may use XMM0-5 for its memcpy; save them because the stolen
    // instruction lives in arbitrary code that may keep live values there.
    b.insert(b.end(),{0x48,0x81,0xec,0x88,0,0,0}); // sub rsp,0x88
    const unsigned char xmmStore[]={0x44,0x4c,0x54,0x5c,0x64,0x6c};
    for(int i=0;i<6;++i) b.insert(b.end(),{0xf3,0x0f,0x7f,xmmStore[i],0x24,(unsigned char)(0x28+i*16)});
    b.insert(b.end(),{0x48,0x89,0xd1}); // mov rcx,rdx (FString*)
    b.insert(b.end(),{0x48,0xba}); imm64(b,ring); // mov rdx,ring
    b.insert(b.end(),{0x41,0xb8}); imm32(b,captureAll?1:0); // mov r8d,captureAll
    b.insert(b.end(),{0x48,0xb8}); imm64(b,capture); // mov rax,helper
    b.insert(b.end(),{0xff,0xd0}); // call rax; caller-owned shadow at rsp+0
    for(int i=0;i<6;++i) b.insert(b.end(),{0xf3,0x0f,0x6f,xmmStore[i],0x24,(unsigned char)(0x28+i*16)});
    b.insert(b.end(),{0x48,0x81,0xc4,0x88,0,0,0}); // add rsp,0x88
    b.insert(b.end(),{0x41,0x5b,0x41,0x5a,0x41,0x59,0x5f,0x5e,0x5a,0x59,0x58,0x41,0x58,0x9d});
    // Relocate the exact seven-byte cmp byte [rip+disp32],imm8; following jcc stays original.
    int oldDisp; std::memcpy(&oldDisp,stolen.data()+2,4);
    U64 operand=original+7+oldDisp;
    b.insert(b.end(),{0x80,0x3d}); rel32(b,at+b.size()+5,operand); b.push_back(stolen[6]);
    Bytes j={0xe9}; rel32(j,at+b.size()+5,original+7); b.insert(b.end(),j.begin(),j.end()); return b;
}
struct Patch { U64 address; Bytes old; };
struct Installed { U64 slab=0; std::vector<Patch> patches; };
Installed install(HANDLE p,U64 base,const ImageView& image,const HookSites& sites,U64 validateBytesHook=0,U64 validateTextHook=0) {
    Installed state;
    const auto& loggers=sites.loggers;
    std::array<Bytes,3> signatures;
    const unsigned char* imageBytes=image.raw();
    if(sites.loggerHooks) for(int i=0;i<3;++i) signatures[i]=Bytes(imageBytes+loggers[i],imageBytes+loggers[i]+7);
    const Bytes originalCall(imageBytes+sites.call,imageBytes+sites.call+5);
    auto verify=[&](U64 addr,const Bytes& expected) { Bytes got(expected.size()); read(p,addr,got.data(),got.size()); if(got!=expected) throw std::runtime_error("Instruction signature mismatch"); };
    verify(base+sites.call,originalCall);
    verify(base+sites.start,{0x48,0x8b,0x09,0x48,0x8b,0x01,0x48,0xff,0x20});
    if(sites.validateBytes && (!validateBytesHook || !validateTextHook)) throw std::runtime_error("Missing script validation adapters");
    for(auto at:sites.validationCalls) if(at) verify(base+at,Bytes(imageBytes+at,imageBytes+at+5));
    if(sites.loggerHooks) for(int i=0;i<3;++i) verify(base+loggers[i],signatures[i]);
    state.slab=allocateNear(p,base+sites.call);
    try {
        Bytes blob(3072); auto stub=startStub(state.slab,base+sites.start); std::copy(stub.begin(),stub.end(),blob.begin());
        if(sites.validateBytes) {
            Bytes gate; absoluteJump(gate,validateBytesHook); std::copy(gate.begin(),gate.end(),blob.begin()+2048);
            gate.clear(); absoluteJump(gate,validateTextHook); std::copy(gate.begin(),gate.end(),blob.begin()+2080);
        }
        if(sites.loggerHooks) for(int i=0;i<3;++i) {
            // PuerTS startup errors occur before bootstrap can add our prefix.
            auto logger=loggerStub(state.slab+256+i*512,state.slab+4096,base+loggers[i],signatures[i],i!=0,(U64)&pbzCaptureLog);
            if(logger.size()>512) throw std::runtime_error("Logger stub overflow");
            std::copy(logger.begin(),logger.end(),blob.begin()+256+i*512);
        }
        write(p,state.slab,blob.data(),blob.size());
        const wchar_t module[]=L"PBZModLoader/bootstrap";
        struct String { U64 data; int count; int capacity; } name={state.slab+4160,(int)std::size(module),(int)std::size(module)};
        write(p,state.slab+4128,&name,sizeof(name)); write(p,state.slab+4160,module,sizeof(module));
        // Executable code is RX; counters, FString and bounded ring are on RW pages.
        DWORD ignored; check(protect(p,(void*)state.slab,4096,PAGE_EXECUTE_READ,&ignored),"stub RX");
        check(FlushInstructionCache(p,(void*)state.slab,4096),"flush stubs");
        // Prove every rel32 branch fits before changing any game instruction.
        Bytes call={0xe8}; rel32(call,base+sites.call+5,state.slab);
        std::array<Bytes,3> hooks;
        if(sites.loggerHooks) for(int i=0;i<3;++i) { hooks[i]={0xe9}; rel32(hooks[i],base+loggers[i]+5,state.slab+256+i*512); hooks[i].insert(hooks[i].end(),{0x90,0x90}); }
        std::array<Bytes,5> validators;
        for(size_t i=0;i<validators.size();++i) if(sites.validationCalls[i]) {
            validators[i]={0xe8}; rel32(validators[i],base+sites.validationCalls[i]+5,state.slab+(i==4?2080:2048));
        }
        for(size_t i=0;i<validators.size();++i) if(sites.validationCalls[i]) {
            auto at=sites.validationCalls[i];
            state.patches.push_back({base+at,Bytes(imageBytes+at,imageBytes+at+5)});
            patch(p,base+at,validators[i]);
        }
        if(sites.loggerHooks) for(int i=0;i<3;++i) {
            state.patches.push_back({base+loggers[i],signatures[i]});
            patch(p,base+loggers[i],hooks[i]);
        }
        state.patches.push_back({base+sites.call,originalCall});
        patch(p,base+sites.call,call);
        return state;
    } catch(...) {
        bool restored=true; for(auto it=state.patches.rbegin();it!=state.patches.rend();++it) try{patch(p,it->address,it->old);}catch(...){restored=false;}
        if(restored) release(p,(void*)state.slab);
        throw;
    }
}
void drain(HANDLE p,U64 slab,U64& next) {
    U64 count=0; read(p,slab+4096,&count,8);
    if(count>next+256) { log("log-overflow dropped="+std::to_string(count-next-256)); next=count-256; }
    while(next<count) {
        std::array<unsigned char,2048> b{}; U64 addr=slab+8192+(next%256)*2048;
        read(p,addr,b.data(),b.size()); U64 seq=0,again=0; unsigned len=0;
        std::memcpy(&seq,b.data(),8); std::memcpy(&len,b.data()+8,4); read(p,addr,&again,8);
        if(seq!=next+1 || again!=seq) break;
        if(len<=1000) log(utf8(std::wstring((wchar_t*)(b.data()+16),len)));
        ++next;
    }
}
