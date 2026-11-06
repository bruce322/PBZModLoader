#pragma once
#include <utility>
// PE image view. Addresses are relative to origin: the runtime base in-process
// and preferred ImageBase in file tests. Either owns a file copy (bytes) or
// borrows an already mapped image (mapped).
struct ImageSection { size_t begin, size; DWORD flags; };
struct ImageView {
    Bytes bytes;
    const unsigned char* mapped=nullptr;
    U64 origin;
    std::vector<ImageSection> sections;
    ImageView(Bytes b,U64 o,std::vector<ImageSection> s):bytes(std::move(b)),origin(o),sections(std::move(s)) {}
    ImageView(const unsigned char* m,U64 o,std::vector<ImageSection> s):mapped(m),origin(o),sections(std::move(s)) {}
    const unsigned char* raw() const { return mapped ? mapped : bytes.data(); }
    bool contains(size_t at,size_t n,DWORD flags=0) const {
        for(auto s:sections) if((s.flags&flags)==flags && at>=s.begin && n<=s.size && at-s.begin<=s.size-n) return true;
        return false;
    }
    template<class T> T value(size_t at) const {
        if(!contains(at,sizeof(T))) throw std::runtime_error("Signature operand outside image sections");
        T v; std::memcpy(&v,raw()+at,sizeof(v)); return v;
    }
    size_t relative(size_t at,size_t displacement,size_t length) const {
        auto target=(long long)at+length+value<int32_t>(at+displacement);
        if(target<0 || !contains((size_t)target,1)) throw std::runtime_error("Signature relative target outside image");
        return (size_t)target;
    }
    std::string ascii(size_t at) const {
        std::string s;
        for(size_t i=0;i<512;++i) { auto c=value<char>(at+i); if(!c) return s; s+=c; }
        throw std::runtime_error("Unterminated signature string");
    }
    std::vector<size_t> find(const std::vector<int>& pattern) const {
        std::vector<size_t> hits;
        const unsigned char* base=raw();
        for(auto s:sections) if(s.flags&IMAGE_SCN_MEM_EXECUTE) {
            if(pattern.empty() || s.size<pattern.size()) continue;
            size_t at=s.begin,end=s.begin+s.size-pattern.size()+1;
            while(at<end) {
                auto p=(const unsigned char*)std::memchr(base+at,pattern[0],end-at);
                if(!p) break;
                at=(size_t)(p-base); bool match=true;
                for(size_t j=1;j<pattern.size();++j) if(pattern[j]>=0 && p[j]!=pattern[j]) { match=false; break; }
                if(match) hits.push_back(at);
                ++at;
            }
        }
        return hits;
    }
};
struct HookSites {
    size_t call,start; std::array<size_t,3> loggers;
    size_t validateBytes=0,validateText=0;
    std::array<size_t,5> validationCalls{};
};
void resolveValidationSites(const ImageView& image,HookSites& sites,bool required) {
    const std::vector<int> bytes={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0xe8,-1,-1,-1,-1,0x48,0x85,0xc0,0x74,0x22,0x48,0x63,0x53,0x08,0x48,0x3b,0x50,0x08,0x75,0x18,0x48,0x8b,0x0b,0x4c,0x8d,0x40,0x18,0xe8,-1,-1,-1,-1,0x84,0xc0,0x74,0x08,0xb0,0x01,0x48,0x83,0xc4,0x20,0x5b,0xc3,0x32,0xc0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
    const std::vector<int> text={0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xf0,0x01,0,0,0x48,0x8b,0x05,-1,-1,-1,-1,0x48,0x33,0xc4,0x48,0x89,0x84,0x24,0xe0,0x01,0,0,0x4c,0x8b,0xf2,0xe8,-1,-1,-1,-1,0x48,0x8b,0xf0,0x48,0x85,0xc0,0x0f,0x84,-1,-1,-1,-1,0x41,0x8b,0x4e,0x08,0x8d,0x59,0xff};
    auto a=image.find(bytes),b=image.find(text);
    if(!required && a.empty() && b.empty()) return; // Original pre-validation build.
    if(a.size()!=1 || b.size()!=1) throw std::runtime_error("Script validation signatures missing or ambiguous");
    auto lookup=image.relative(a[0]+9,1,5);
    if(lookup!=image.relative(b[0]+44,1,5) || !image.contains(lookup,1,IMAGE_SCN_MEM_EXECUTE) ||
       !image.contains(image.relative(a[0]+36,1,5),1,IMAGE_SCN_MEM_EXECUTE))
        throw std::runtime_error("Script validation references disagree");
    sites.validateBytes=a[0]; sites.validateText=b[0];
    // Patch complete, observed call sites, leaving validator prologues/unwind intact.
    const std::array<std::vector<int>,5> calls={{
        {0x48,0x8d,0x55,0x08,0x48,0x8d,0x4d,0xf8,0xe8,-1,-1,-1,-1,0x84,0xc0,0x74,0x05,0x45,0x32,0xf6},
        {0x48,0x8d,0x55,0x90,0x48,0x8b,0xce,0xe8,-1,-1,-1,-1,0x84,0xc0,0x0f,0x84,-1,-1,-1,-1,0x48,0x89,0x5c,0x24,0x68},
        {0x48,0x8d,0x55,0xc8,0x48,0x8d,0x4c,0x24,0x50,0xe8,-1,-1,-1,-1,0x84,0xc0,0x0f,0x84,-1,-1,-1,-1,0x48,0x89,0x5d,0xe8},
        {0x48,0x8d,0x54,0x24,0x60,0x48,0x8d,0x4d,0x80,0xe8,-1,-1,-1,-1,0x84,0xc0,0x74,0x30,0x49,0x8b,0x3e},
        {0x48,0x8d,0x54,0x24,0x38,0x48,0x8b,0xc8,0xe8,-1,-1,-1,-1,0x0f,0xb6,0xd8,0x48,0x8b,0x4c,0x24,0x68}
    }};
    const size_t offsets[]={8,7,9,9,8};
    for(size_t i=0;i<calls.size();++i) {
        size_t count=0;
        for(auto at:image.find(calls[i])) {
            auto call=at+offsets[i];
            if(image.relative(call,1,5)!=(i==4?sites.validateText:sites.validateBytes)) continue;
            sites.validationCalls[i]=call; ++count;
        }
        if(count!=1) throw std::runtime_error("Script validation call missing or ambiguous; index="+std::to_string(i));
    }
}
HookSites resolveSites(const ImageView& image) {
    HookSites sites{};
    // lea rdx,"MainStart"; construct FString; prepare context, name and argv;
    // call PuerTS Start. Displacements vary with builds and ASLR.
    const std::vector<int> mainPattern={0x48,0x8d,0x15,-1,-1,-1,-1,0x48,0x8d,0x4d,0x97,0xe8,-1,-1,-1,-1,0x4c,0x8d,0x45,0xd7,0x48,0x8b,0xcb,0x48,0x8d,0x55,0x97,0xe8,-1,-1,-1,-1,0x48,0x8b,0x4d,0x97,0x48,0x85,0xc9};
    // November 2026 update: FString moved from [rbp-69h] to [rsp+60h].
    // Keep complete, build-observed instruction layouts rather than wildcarding
    // stack operands: the constructor, Start and destructor must use one slot.
    const std::vector<int> mainPatternRsp={0x48,0x8d,0x15,-1,-1,-1,-1,0x48,0x8d,0x4c,0x24,0x60,0xe8,-1,-1,-1,-1,0x4c,0x8d,0x45,0x90,0x48,0x8b,0xcb,0x48,0x8d,0x54,0x24,0x60,0xe8,-1,-1,-1,-1,0x48,0x8b,0x4c,0x24,0x60,0x48,0x85,0xc9};
    size_t count=0; bool rspLayout=false;
    for(const auto& variant:std::array<std::pair<std::vector<int>,size_t>,2>{{{mainPattern,27},{mainPatternRsp,29}}})
    for(auto at:image.find(variant.first)) {
        try {
            if(image.ascii(image.relative(at,3,7))!="MainStart") continue;
            auto call=at+variant.second,start=image.relative(call,1,5);
            const Bytes thunk={0x48,0x8b,0x09,0x48,0x8b,0x01,0x48,0xff,0x20};
            if(!image.contains(start,thunk.size(),IMAGE_SCN_MEM_EXECUTE) ||
               !std::equal(thunk.begin(),thunk.end(),image.raw()+start)) continue;
            sites.call=call; sites.start=start; rspLayout=variant.second==29; ++count;
        } catch(const std::exception&) { /* malformed candidate is not a match */ }
    }
    if(count!=1) throw std::runtime_error("MainStart signature requires one validated match; found="+std::to_string(count));
    size_t category=0,source=0;
    for(size_t i=0;i<3;++i) {
        const int level[]={5,3,2};
        const std::vector<int> logger={0x80,0x3d,-1,-1,-1,-1,level[i],0x72,0x28,0x83,0x7a,0x08,0,0x74,5,0x4c,0x8b,0x0a,0xeb,7,0x4c,0x8d,0x0d,-1,-1,-1,-1,0x4c,0x8b,0xc1,0x48,0x8d,0x15,-1,-1,-1,-1,0x48,0x8d,0x0d,-1,-1,-1,-1,0xe9,-1,-1,-1,-1,0xc3};
        count=0; size_t matchedCategory=0,matchedSource=0;
        for(auto at:image.find(logger)) {
            try {
                auto cat=image.relative(at,2,7);
                if(!image.contains(cat,1,IMAGE_SCN_MEM_WRITE) || image.relative(at+30,3,7)!=cat) continue;
                auto target=image.relative(at+44,1,5);
                if(!image.contains(target,1,IMAGE_SCN_MEM_EXECUTE)) continue;
                auto descriptor=image.relative(at+37,3,7);
                U64 ptr=image.value<U64>(descriptor+8);
                if(ptr<image.origin) continue;
                auto src=(size_t)(ptr-image.origin);
                auto path=image.ascii(src);
                std::replace(path.begin(),path.end(),'\\','/');
                const std::string suffix="/Puerts/Source/JsEnv/Private/JSLogger.cpp";
                if(path.size()<suffix.size() || path.compare(path.size()-suffix.size(),suffix.size(),suffix)) continue;
                sites.loggers[i]=at; matchedCategory=cat; matchedSource=src; ++count;
            } catch(const std::exception&) {}
        }
        if(count!=1) throw std::runtime_error("PuerTS logger signature requires one validated match; level="+std::to_string(level[i])+" found="+std::to_string(count));
        if(i && (category!=matchedCategory || source!=matchedSource)) throw std::runtime_error("PuerTS logger references disagree");
        category=matchedCategory; source=matchedSource;
    }
    resolveValidationSites(image,sites,rspLayout);
    return sites;
}

fs::path findProject(const fs::path& executable) {
    for(auto dir=fs::absolute(executable).parent_path();!dir.empty();) {
        auto candidate=dir.filename()==L"ProjectPBZ"?dir:dir/L"ProjectPBZ";
        if(fs::is_directory(candidate/L"Content") && fs::is_regular_file(candidate/L"PBZModLoader/mods.ini") &&
           fs::is_regular_file(candidate/L"Content/JavaScript/PBZModLoader/bootstrap.js")) return fs::canonical(candidate);
        auto parent=dir.parent_path(); if(parent==dir) break; dir=parent;
    }
    throw std::runtime_error("ProjectPBZ runtime not found above game executable");
}
