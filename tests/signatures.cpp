#include "../src/core.hpp"
#include <cassert>
ImageView loadFile(const fs::path& file) {
    std::ifstream stream(file,std::ios::binary|std::ios::ate);
    if(!stream) throw std::runtime_error("Cannot read fixture EXE");
    Bytes raw((size_t)stream.tellg()); stream.seekg(0); stream.read((char*)raw.data(),raw.size());
    auto get=[&](size_t at,void* out,size_t size) { if(at>raw.size() || size>raw.size()-at) throw std::runtime_error("Truncated PE file"); std::memcpy(out,raw.data()+at,size); };
    IMAGE_DOS_HEADER dos{}; get(0,&dos,sizeof(dos));
    IMAGE_NT_HEADERS64 nt{}; get(dos.e_lfanew,&nt,sizeof(nt));
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || nt.Signature!=IMAGE_NT_SIGNATURE || nt.OptionalHeader.SizeOfImage>1024*1024*1024) throw std::runtime_error("Invalid fixture PE");
    ImageView image{Bytes(nt.OptionalHeader.SizeOfImage),nt.OptionalHeader.ImageBase,{}};
    for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        IMAGE_SECTION_HEADER s{}; get(dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader+i*sizeof(s),&s,sizeof(s));
        size_t size=s.Misc.VirtualSize;
        if(s.VirtualAddress>image.bytes.size() || size>image.bytes.size()-s.VirtualAddress) throw std::runtime_error("Invalid fixture section");
        if(!(s.Characteristics&IMAGE_SCN_MEM_READ) || (s.Characteristics&IMAGE_SCN_MEM_DISCARDABLE) || !size) continue;
        get(s.PointerToRawData,image.bytes.data()+s.VirtualAddress,std::min(size,(size_t)s.SizeOfRawData));
        image.sections.push_back({s.VirtualAddress,size,s.Characteristics});
    }
    return image;
}
int main(int argc,char** argv) {
    if(argc!=2) { std::cerr<<"Usage: signature-test.exe game.exe\n"; return 2; }
    auto image=loadFile(fs::u8path(argv[1]));
    auto original=resolveSites(image);
    // Regression expectations belong only in tests, never in the resolver.
    const bool november=original.call==0x66c3e1b;
    assert(original.call==(november?0x66c3e1b:0x6686768) && original.start==(november?0x62cc850:0x6284a20));
    assert((original.loggers==(november?std::array<size_t,3>{0x62eee00,0x62f6830,0x62e9c40}:std::array<size_t,3>{0x62ab700,0x62b2e00,0x62a56b0})));
    assert(original.loggerHooks);
    const size_t callOffset=november?29:27;
    if(november) {
        assert(original.validateBytes==0x62f65a0 && original.validateText==0x62f65e0);
        assert((original.validationCalls==std::array<size_t,5>{0x61ec4da,0x61eee5f,0x61ef162,0x61fe5bb,0x61ebce0}));
        auto rejectValidation=[&] { bool rejected=false; try {resolveSites(image);} catch(const std::exception&) {rejected=true;} assert(rejected); };
        auto at=original.validateBytes; auto saved=image.bytes[at]; image.bytes[at]=0xcc; rejectValidation(); image.bytes[at]=saved;
        at=original.validationCalls[3]+1; saved=image.bytes[at]; image.bytes[at]^=1; rejectValidation(); image.bytes[at]=saved;
        image.sections.push_back({original.validateText,80,IMAGE_SCN_MEM_EXECUTE|IMAGE_SCN_MEM_READ});
        rejectValidation(); image.sections.pop_back();
        std::cout<<"PASS script validation pair, all five call sites, damaged/missing/ambiguous rejection\n";
    }
    std::cout<<"PASS real PE: unique MainStart and three PuerTS loggers\n";
    // Exercise the other observed stack layout against the same validated PE.
    // Two valid layouts together must be rejected, not silently pick one.
    {
        const Bytes rbp={0x48,0x8d,0x15,0,0,0,0,0x48,0x8d,0x4d,0x97,0xe8,0,0,0,0,0x4c,0x8d,0x45,0xd7,0x48,0x8b,0xcb,0x48,0x8d,0x55,0x97,0xe8,0,0,0,0,0x48,0x8b,0x4d,0x97,0x48,0x85,0xc9};
        const Bytes rsp={0x48,0x8d,0x15,0,0,0,0,0x48,0x8d,0x4c,0x24,0x60,0xe8,0,0,0,0,0x4c,0x8d,0x45,0x90,0x48,0x8b,0xcb,0x48,0x8d,0x54,0x24,0x60,0xe8,0,0,0,0,0x48,0x8b,0x4c,0x24,0x60,0x48,0x85,0xc9};
        auto name=image.relative(original.call-callOffset,3,7);
        size_t at=image.bytes.size(),otherOffset=november?27:29;
        auto code=november?rbp:rsp;
        image.bytes.insert(image.bytes.end(),code.begin(),code.end());
        image.sections.push_back({at,code.size(),IMAGE_SCN_MEM_READ|IMAGE_SCN_MEM_EXECUTE});
        int32_t delta=(int32_t)((int64_t)name-(int64_t)at-7);
        std::memcpy(image.bytes.data()+at+3,&delta,4);
        delta=(int32_t)((int64_t)original.start-(int64_t)(at+otherOffset)-5);
        std::memcpy(image.bytes.data()+at+otherOffset+1,&delta,4);
        bool rejected=false;
        try { resolveSites(image); } catch(const std::exception&) { rejected=true; }
        assert(rejected);
        auto first=original.call-callOffset;
        image.bytes[first]=0xcc;
        if(november) {
            auto other=resolveSites(image);
            assert(other.call==at+otherOffset && other.start==original.start && other.loggers==original.loggers);
        } else {
            // The new layout without the new validation pair is unsupported.
            rejected=false; try {resolveSites(image);} catch(const std::exception&) {rejected=true;} assert(rejected);
        }
        // Disagreeing FString slots must fail the complete instruction pattern.
        image.bytes[at+(november?26:28)]^=8;
        rejected=false;
        try { resolveSites(image); } catch(const std::exception&) { rejected=true; }
        assert(rejected);
        image.bytes[first]=0x48;
        image.sections.pop_back(); image.bytes.resize(at);
        std::cout<<"PASS both stack layouts, cross-layout ambiguity and mismatched stack rejection\n";
    }
    // Move all sections; also rebase absolute pointers used by logger descriptors.
    const size_t shift=0x2000; const U64 rebase=0x310000000ULL;
    image.bytes.insert(image.bytes.begin(),shift,0);
    for(auto& section:image.sections) section.begin+=shift;
    image.origin+=rebase;
    for(auto rva:original.loggers) {
        auto descriptor=image.relative(rva+shift+37,3,7);
        auto pointer=image.value<U64>(descriptor+8)+shift+rebase;
        std::memcpy(image.bytes.data()+descriptor+8,&pointer,8);
    }
    auto moved=resolveSites(image);
    assert(moved.call==original.call+shift && moved.start==original.start+shift);
    for(size_t i=0;i<3;++i) assert(moved.loggers[i]==original.loggers[i]+shift);
    std::cout<<"PASS shifted RVAs and ASLR, without fixed offsets or EXE hashes\n";
    // Independently move the Start thunk, changing the call's signed displacement.
    size_t relocated=moved.start+0x100;
    std::copy_n(image.bytes.begin()+moved.start,9,image.bytes.begin()+relocated);
    image.bytes[moved.start]=0xcc;
    int32_t displacement=(int32_t)((int64_t)relocated-(int64_t)moved.call-5);
    std::memcpy(image.bytes.data()+moved.call+1,&displacement,4);
    moved=resolveSites(image); assert(moved.start==relocated);
    std::cout<<"PASS changed call displacement and dynamic Start target\n";
    auto reject=[&] { bool rejected=false; try { resolveSites(image); } catch(const std::exception& e) { rejected=true; std::cout<<"PASS rejection: "<<e.what()<<'\n'; } assert(rejected); };
    auto saved=image.bytes[moved.start]; image.bytes[moved.start]=0xcc; reject(); image.bytes[moved.start]=saved;
    auto name=image.relative(moved.call-callOffset,3,7); image.bytes[name]='X'; reject(); image.bytes[name]='M';
    // Duplicate executable section ranges deliberately expose the same candidates twice.
    auto sections=image.sections; for(auto s:sections) if(s.flags&IMAGE_SCN_MEM_EXECUTE) image.sections.push_back(s);
    reject(); image.sections=sections;
    auto loggerSection=*std::find_if(sections.begin(),sections.end(),[&](ImageSection s){return moved.loggers[0]>=s.begin && moved.loggers[0]<s.begin+s.size;});
    // Duplicate just the first logger's 50-byte executable window.
    image.sections.push_back({moved.loggers[0],50,loggerSection.flags});
    {
        auto disabled=resolveSites(image);
        assert(!disabled.loggerHooks);
        assert((disabled.loggers==std::array<size_t,3>{}));
        assert(disabled.call==moved.call && disabled.start==moved.start);
    }
    image.sections=sections;
    auto category=image.relative(moved.loggers[1],2,7);
    for(auto offset:{size_t(0),size_t(30)}) {
        int32_t delta=(int32_t)((int64_t)category+1-(int64_t)(moved.loggers[1]+offset+7));
        std::memcpy(image.bytes.data()+moved.loggers[1]+offset+(offset?3:2),&delta,4);
    }
    {
        auto disabled=resolveSites(image);
        assert(!disabled.loggerHooks);
        assert((disabled.loggers==std::array<size_t,3>{}));
    }
    for(auto offset:{size_t(0),size_t(30)}) {
        int32_t delta=(int32_t)((int64_t)category-(int64_t)(moved.loggers[1]+offset+7));
        std::memcpy(image.bytes.data()+moved.loggers[1]+offset+(offset?3:2),&delta,4);
    }
    auto desc=image.relative(moved.loggers[0]+37,3,7), source=image.value<U64>(desc+8)-image.origin;
    saved=image.bytes[source]; image.bytes[source]=0; reject(); image.bytes[source]=saved;
    {
        auto disabled=resolveSites(image);
        assert(!disabled.loggerHooks);
        assert((disabled.loggers==std::array<size_t,3>{}));
    }
    for(auto& s:image.sections) s.flags&=~IMAGE_SCN_MEM_EXECUTE;
    reject();
}
