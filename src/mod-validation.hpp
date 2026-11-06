#pragma once
// Only extend trust to files installed under the loader's own script tree.
// Official game scripts continue through the game's unmodified validators.
struct GameString { const wchar_t* data; int count,capacity; };
struct GameBytes { const unsigned char* data; int count,capacity; };
bool gameString(const GameString* value,std::wstring& result,size_t limit=32768) {
    if(!value || !value->data || value->count<1 || value->capacity<value->count || (size_t)value->count>limit ||
       value->data[value->count-1]!=0) return false;
    result.assign(value->data,value->count-1);
    return result.find(L'\0')==std::wstring::npos;
}
fs::path modScriptPath(const fs::path& root,std::wstring name) {
    std::replace(name.begin(),name.end(),L'\\',L'/');
    std::wstring lower=name;
    for(auto& c:lower) if(c>=L'A' && c<=L'Z') c+=L'a'-L'A';
    const std::wstring marker=L"pbzmodloader/";
    fs::path canonicalRoot; bool haveRoot=false;
    // A validator path may contain an unrelated PBZModLoader segment before the real loader tree.
    // Try every marker occurrence from the end; accept only a suffix that resolves inside root.
    for(size_t at=lower.size();;) {
        auto pos=lower.rfind(marker,at?at-1:0);
        if(pos==std::wstring::npos) break;
        if(pos==0 || lower[pos-1]==L'/') {
            auto suffix=name.substr(pos+marker.size());
            if(!suffix.empty() && suffix.find_first_of(L":\0",0,2)==std::wstring::npos) {
                fs::path relative(suffix);
                bool invalid=relative.is_absolute() || relative.has_root_path();
                if(!invalid) for(const auto& part:relative) {
                    if(part==L".." || part==L"." || part.empty()) { invalid=true; break; }
                }
                if(!invalid) {
                    auto normalized=relative.generic_wstring();
                    if(normalized==L"bootstrap.js" || normalized==L"manager.js" || normalized==L"generated-config.js" ||
                       normalized.rfind(L"mods/",0)==0 ||
                       normalized.rfind(L"generated-mods/",0)==0 || normalized.rfind(L"generated-reloads/",0)==0) {
                        try {
                            if(!haveRoot) { canonicalRoot=fs::canonical(root); haveRoot=true; }
                            auto candidate=fs::canonical(root/relative);
                            auto within=candidate.lexically_relative(canonicalRoot);
                            bool escaped=within.empty() || within.is_absolute();
                            if(!escaped) for(const auto& part:within) if(part==L"..") { escaped=true; break; }
                            if(!escaped) return candidate;
                        } catch(const std::exception&) {}
                    }
                }
            }
        }
        if(pos==0) break;
        at=pos;
    }
    return {};
}
bool readModScript(const fs::path& root,const GameString* path,Bytes& bytes) {
    std::wstring name;
    if(!gameString(path,name)) return false;
    auto file=modScriptPath(root,name);
    if(file.empty()) return false;
    std::ifstream stream(file,std::ios::binary|std::ios::ate);
    if(!stream) return false;
    auto length=stream.tellg();
    if(length<0 || length>64*1024*1024) return false;
    bytes.resize((size_t)length); stream.seekg(0);
    if(!bytes.empty()) stream.read((char*)bytes.data(),bytes.size());
    return bool(stream);
}
bool trustedModBytes(const fs::path& root,const GameString* path,const GameBytes* source) {
    if(!source || source->count<0 || source->capacity<source->count || (source->count && !source->data)) return false;
    Bytes expected;
    return readModScript(root,path,expected) && expected.size()==(size_t)source->count &&
        (expected.empty() || std::memcmp(expected.data(),source->data,expected.size())==0);
}
bool trustedModText(const fs::path& root,const GameString* path,const GameString* source) {
    Bytes expected; std::wstring actual;
    if(!readModScript(root,path,expected) || !gameString(source,actual,64*1024*1024)) return false;
    // PuerTS decodes UTF-8 and evaluates a CommonJS wrapper around .js files.
    size_t offset=expected.size()>=3 && expected[0]==0xef && expected[1]==0xbb && expected[2]==0xbf?3:0;
    auto size=(int)(expected.size()-offset);
    std::wstring decoded;
    if(size) {
        auto data=(const char*)expected.data()+offset;
        int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,data,size,nullptr,0);
        if(!length) return false;
        decoded.resize(length);
        if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,data,size,decoded.data(),length)!=length) return false;
    }
    return actual==decoded || actual==L"(function (exports, require, module, __filename, __dirname) { "+decoded+L"\n});";
}
