// Exercise the actual machine-code generators in an isolated native process.
#include "../src/core.hpp"
#include "../src/mod-validation.hpp"
#include <thread>
#include <cassert>
struct FString { const wchar_t* data; int count; int capacity; };
void* expectedContext; void* expectedArgv;
void capture(void* context,const FString* name,void* argv) {
    assert(context==expectedContext && argv==expectedArgv);
    assert(std::wstring(name->data)==L"PBZModLoader/bootstrap");
    assert(name->count==22 && name->capacity==22);
}
int main() {
    HANDLE process=GetCurrentProcess();
    {
        auto two=(unsigned char*)VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(two);
        DWORD old; assert(VirtualProtect(two,4096,PAGE_EXECUTE_READ,&old));
        Bytes cross={0x90,0x91,0x92,0x93,0x94}; patch(process,(U64)(two+4094),cross);
        assert(two[4094]==0x90 && two[4095]==0x91 && two[4096]==0x92 && two[4097]==0x93 && two[4098]==0x94);
        MEMORY_BASIC_INFORMATION first{},second{};
        assert(VirtualQuery(two,&first,sizeof(first))==sizeof(first));
        assert(VirtualQuery(two+4096,&second,sizeof(second))==sizeof(second));
        assert(first.Protect==PAGE_EXECUTE_READ);
        assert(second.Protect==PAGE_READWRITE);
    }
    auto slab=(U64)VirtualAlloc(nullptr,SLAB_SIZE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); assert(slab);
    auto start=startStub(slab,(U64)&capture); write(process,slab,start.data(),start.size());
    const wchar_t text[]=L"PBZModLoader/bootstrap";
    FString name={(wchar_t*)(slab+4160),22,22}; write(process,slab+4128,&name,sizeof(name)); write(process,slab+4160,text,sizeof(text));
    U64 original=slab+2048, operand=slab+4208;
    Bytes stolen={0x80,0x3d}; rel32(stolen,original+7,operand); stolen.push_back(5);
    // Original cmp followed by setae al; ret. The relocated cmp must preserve flags.
    Bytes fake=stolen; fake.insert(fake.end(),{0x0f,0x93,0xc0,0xc3}); write(process,original,fake.data(),fake.size());
    *(unsigned char*)operand=5;
    auto logger=loggerStub(slab+256,slab+4096,original,stolen); write(process,slab+256,logger.data(),logger.size());
    auto diagnostics=loggerStub(slab+768,slab+4096,original,stolen,true); write(process,slab+768,diagnostics.data(),diagnostics.size());
    DWORD old; assert(VirtualProtect((void*)slab,4096,PAGE_EXECUTE_READ,&old)); FlushInstructionCache(process,(void*)slab,4096);
    expectedContext=(void*)0x1234; expectedArgv=(void*)0x5678;
    ((void(*)(void*,FString*,void*))slab)(expectedContext,nullptr,expectedArgv);
    assert(*(U64*)(slab+4104)==1);
    auto emit=(unsigned char(*)(void*,const FString*))(slab+256);
    const wchar_t message[]=L"[PBZMOD] native stub test"; FString msg={message,(int)std::size(message),(int)std::size(message)};
    assert(emit(nullptr,&msg)==1); assert(*(U64*)(slab+4096)==1);
    assert(*(U64*)(slab+8192)==1); assert(std::wstring((wchar_t*)(slab+8192+16),std::size(message)-1)==message);
    const wchar_t other[]=L"unrelated"; FString skip={other,10,10}; emit(nullptr,&skip); emit(nullptr,nullptr); assert(*(U64*)(slab+4096)==1);
    std::wstring longMessage=L"[PBZMOD]"+std::wstring(3000,L'x'); FString longMsg={longMessage.c_str(),(int)longMessage.size()+1,(int)longMessage.size()+1}; emit(nullptr,&longMsg);
    assert(*(unsigned*)(slab+8192+2048+8)==1000);
    std::vector<std::thread> workers; for(int i=0;i<8;++i) workers.emplace_back([&]{for(int j=0;j<16;++j) assert(emit(nullptr,&msg)==1);});
    for(auto& t:workers)t.join(); assert(*(U64*)(slab+4096)==130);
    for(int i=0;i<130;++i) assert(*(U64*)(slab+8192+i*2048)==(U64)i+1);
    for(int i=0;i<300;++i) emit(nullptr,&msg);
    assert(*(U64*)(slab+4096)==430);
    for(U64 i=174;i<430;++i) assert(*(U64*)(slab+8192+(i%256)*2048)==i+1);
    auto emitDiagnostic=(unsigned char(*)(void*,const FString*))(slab+768);
    emitDiagnostic(nullptr,&skip);
    const wchar_t shortText[]=L"!"; FString shortMsg={shortText,2,2}; emitDiagnostic(nullptr,&shortMsg);
    emitDiagnostic(nullptr,nullptr);
    assert(*(U64*)(slab+4096)==432);
    assert(std::wstring((wchar_t*)(slab+8192+(430%256)*2048+16),9)==L"unrelated");
    assert(*(unsigned*)(slab+8192+(431%256)*2048+8)==1);
    VirtualFree((void*)slab,0,MEM_RELEASE);
    auto temp=fs::temp_directory_path()/(L"pbz-loader-test-"+std::to_wstring(GetCurrentProcessId()));
    assert(fs::create_directory(temp)); fs::create_directories(temp/L"mods/a");
    {
        auto root=temp/L"validation";
        fs::create_directories(root/L"generated-mods/test");
        const std::string code="module.exports = '\xe4\xb8\xad\xe6\x96\x87';";
        { std::ofstream f(root/L"generated-mods/test/main.js",std::ios::binary); f<<code; }
        auto string=[](const std::wstring& value) { return GameString{value.c_str(),(int)value.size()+1,(int)value.size()+1}; };
        std::wstring path=L"../../../ProjectPBZ/Content/JavaScript/PBZModLoader/generated-mods/test/main.js";
        auto name=string(path);
        GameBytes bytes{(const unsigned char*)code.data(),(int)code.size(),(int)code.size()};
        assert(trustedModBytes(root,&name,&bytes));
        std::wstring raw=L"module.exports = '\u4e2d\u6587';";
        auto source=string(raw); assert(trustedModText(root,&name,&source));
        std::wstring wrapped=L"(function (exports, require, module, __filename, __dirname) { "+raw+L"\n});";
        source=string(wrapped); assert(trustedModText(root,&name,&source));
        wrapped+=L"evil()"; source=string(wrapped); assert(!trustedModText(root,&name,&source));
        bytes.count--; assert(!trustedModBytes(root,&name,&bytes)); bytes.count++;
        for(const auto& denied:{L"project/mainstart.js",L"PBZModLoader/../MainStart.js",L"PBZModLoader/generated-mods/../manager.js",L"OtherPBZModLoader/generated-mods/test/main.js",L"PBZModLoader/generated-mods/test/main.js:stream"}) {
            std::wstring value=denied; auto rejected=string(value);
            assert(!trustedModBytes(root,&rejected,&bytes));
        }
        // A validator path may contain an unrelated PBZModLoader segment before the real tree.
        std::wstring prefixed=L"../../../PBZModLoader/ProjectPBZ/Content/JavaScript/PBZModLoader/generated-mods/test/main.js";
        auto prefixedPath=string(prefixed);
        assert(trustedModBytes(root,&prefixedPath,&bytes));
        assert(!trustedModBytes(root,nullptr,&bytes));
        source.count=0; assert(!trustedModText(root,&name,&source));
        std::cout<<"PASS MOD validation: exact bytes, UTF-8, CommonJS wrapper, game paths, traversal and content mismatch\n";
    }
    { std::ofstream f(temp/L"mods/a/main.js"); f<<"module.exports={};"; }
    auto iniTest=[&](const std::string& content,bool expected) {
        { std::ofstream f(temp/L"mods.ini"); f<<content; }
        // Invalidate Windows INI cache between fixture writes.
        WritePrivateProfileStringW(nullptr,nullptr,nullptr,(temp/L"mods.ini").c_str());
        bool accepted=false; try { config(temp); accepted=true; } catch(const std::exception&) {}
        assert(accepted==expected);
    };
    const std::string prefix="[Loader]\nVersion=1\nCount=1\n[Mod.0]\nId=a\n";
    iniTest(prefix+"Enabled=1\nEntry=mods/a/main.js\n",true);
    fs::create_directories(temp/L"scripts/generated-mods/removed");
    { std::ofstream f(temp/L"scripts/generated-mods/removed/main.js"); f<<"stale"; }
    { std::ofstream f(temp/L"mods/a/helper.js"); f<<"module.exports=42;"; }
    { std::ofstream f(temp/L"scripts/manager.js"); f<<"keep"; }
    syncMods(temp,temp/L"scripts");
    assert(fs::is_regular_file(temp/L"scripts/generated-mods/a/main.js"));
    assert(fs::is_regular_file(temp/L"scripts/generated-mods/a/helper.js"));
    assert(!fs::exists(temp/L"scripts/generated-mods/removed"));
    assert(fs::is_regular_file(temp/L"scripts/manager.js"));
    assert(fs::is_regular_file(temp/L"mods/a/main.js"));
    fs::remove(temp/L"mods/a/helper.js");
    syncMods(temp,temp/L"scripts");
    assert(!fs::exists(temp/L"scripts/generated-mods/a/helper.js"));
    iniTest(prefix+"Enabled=1\nEntry=mods/missing/main.js\n",false);
    iniTest(prefix+"Enabled=0\nEntry=mods/missing/main.js\n",true);
    iniTest(prefix+"Enabled=1\nEntry=../escape.js\n",false);
    iniTest(prefix+"Enabled=1\nEntry=mods/a/main.ts\n",false);
    iniTest(prefix+"Enabled=2\nEntry=mods/a/main.js\n",false);
    // Legacy configs (no Version key, or a [Mod.N] section without Enabled) remain loadable.
    iniTest("[Loader]\nCount=1\n[Mod.0]\nId=a\nEnabled=1\nEntry=mods/a/main.js\n",true);
    assert(config(temp).find("\"id\":\"a\",\"enabled\":true")!=std::string::npos);
    iniTest("[Loader]\nVersion=1\nCount=1\n[Mod.0]\nId=a\nEntry=mods/a/main.js\n",true);
    assert(config(temp).find("\"id\":\"a\",\"enabled\":true")!=std::string::npos);
    iniTest("[Loader]\nVersion=2\nCount=1\n[Mod.0]\nId=a\nEnabled=1\nEntry=mods/a/main.js\n",false);
    iniTest("[Loader]\nVersion=1\nCount=2\n[Mod.0]\nId=a\nEnabled=1\nEntry=mods/a/main.js\n[Mod.1]\nId=a\nEnabled=0\nEntry=mods/a/main.js\n",false);
    // Windows is case-insensitive for the same MOD directory; reject A/a to avoid silent collisions.
    iniTest("[Loader]\nVersion=1\nCount=2\n[Mod.0]\nId=A\nEnabled=1\nEntry=mods/A/main.js\n[Mod.1]\nId=a\nEnabled=1\nEntry=mods/a/main.js\n",false);
    const std::string automatic="[Loader]\nVersion=1\n";
    iniTest(automatic,true);
    assert(config(temp).find("\"id\":\"a\",\"enabled\":true")!=std::string::npos);
    fs::create_directories(temp/L"mods/z");
    fs::create_directories(temp/L"mods/helper/nested");
    { std::ofstream f(temp/L"mods/z/main.js"); f<<"module.exports={};"; }
    { std::ofstream f(temp/L"mods/helper/nested/main.js"); f<<"module.exports={};"; }
    { std::ofstream f(temp/L"mods/loose.js"); f<<"module.exports={};"; }
    auto generated=config(temp);
    assert(generated.find("\"id\":\"a\"")<generated.find("\"id\":\"z\""));
    assert(generated.find("helper")==std::string::npos && generated.find("loose")==std::string::npos);
    iniTest(automatic+"[Mods]\na=0\nz=1\nabsent=0\n",true);
    assert(config(temp).find("\"id\":\"a\",\"enabled\":false")!=std::string::npos);
    iniTest(automatic+"[Mods]\na=2\n",false);
    iniTest(prefix+"Enabled=0\nEntry=mods/a/main.js\n",true);
    generated=config(temp);
    assert(generated.find("\"id\":\"a\",\"enabled\":false")!=std::string::npos);
    assert(generated.find("\"id\":\"a\"",generated.find("\"id\":\"a\"")+1)==std::string::npos);
    assert(generated.find("\"id\":\"z\"")!=std::string::npos);
    iniTest("[Loader]\nVersion=1\nCount=1\n[Mod.0]\nId=alias\nEnabled=0\nEntry=mods/a/main.js\n",true);
    assert(config(temp).find("\"id\":\"a\"")==std::string::npos);
    iniTest(prefix+"Enabled=1\nEntry=mods/a/main.js\n[Mods]\na=0\n",true);
    assert(config(temp).find("\"id\":\"a\",\"enabled\":false")!=std::string::npos);
    fs::remove(temp/L"mods/z/main.js");
    assert(config(temp).find("\"id\":\"z\"")==std::string::npos);
    { std::ofstream f(temp/L"mods/a/main.ts"); f<<"import { value } from './helper'; export const answer: number = value;"; }
    { std::ofstream f(temp/L"mods/a/helper.ts"); f<<"export const value: number = 42;"; }
    fs::copy(fs::current_path()/L"runtime/ProjectPBZ/PBZModLoader/compiler",temp/L"compiler",fs::copy_options::recursive);
    iniTest(automatic,true);
    syncMods(temp,temp/L"scripts");
    generated=config(temp,temp/L"scripts");
    assert(generated.find("\"id\":\"a\",\"enabled\":true")!=std::string::npos);
    assert(fs::is_regular_file(temp/L"scripts/generated-mods/a/helper.js"));
    { std::ofstream f(temp/L"mods/a/main.ts"); f<<"export const broken: = ;"; }
    syncMods(temp,temp/L"scripts");
    generated=config(temp,temp/L"scripts");
    assert(generated.find("\"id\":\"a\",\"enabled\":false")!=std::string::npos);
    resetReloads(temp/L"scripts");
    assert(!fs::exists(temp/L"scripts/generated-reloads/1"));
    { std::ofstream f(temp/L"mods/a/main.ts"); f<<"import { value } from './helper'; export const answer = value;"; }
    publishReload(temp,temp/L"scripts",1);
    assert(fs::exists(temp/L"scripts/generated-reloads/1/config.js"));
    auto first=sha256(temp/L"scripts/generated-reloads/1/generated-mods/a/helper.js");
    { std::ofstream f(temp/L"mods/a/helper.ts"); f<<"export const value: number = 43;"; }
    publishReload(temp,temp/L"scripts",2);
    assert(fs::exists(temp/L"scripts/generated-reloads/2/config.js"));
    assert(first==sha256(temp/L"scripts/generated-reloads/1/generated-mods/a/helper.js"));
    assert(first!=sha256(temp/L"scripts/generated-reloads/2/generated-mods/a/helper.js"));
    { std::ofstream f(temp/L"mods/a/main.ts"); f<<"export const broken: = ;"; }
    bool rejected=false; try { publishReload(temp,temp/L"scripts",3); } catch(const std::exception&) { rejected=true; }
    assert(rejected && !fs::exists(temp/L"scripts/generated-reloads/3"));
    publishReloadFailure(temp/L"scripts",3);
    { std::ifstream f(temp/L"scripts/generated-reloads/3/config.js"); std::ostringstream content; content<<f.rdbuf();
      assert(content.str().find("error:")!=std::string::npos); }
    fs::remove(temp/L"mods/a/main.ts");
    publishReload(temp,temp/L"scripts",4);
    assert(fs::exists(temp/L"scripts/generated-reloads/4/config.js"));
    std::cout<<"PASS hot reload immutable dependencies, compile failure and recovery\n";
    fs::remove_all(temp/L"mods");
    iniTest(automatic,true);
    assert(config(temp).find("\"mods\":[]")!=std::string::npos);
    fs::remove_all(temp);
    auto layout=fs::temp_directory_path()/(L"pbz-layout-test-"+std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(layout/L"ProjectPBZ/Content/JavaScript/PBZModLoader");
    fs::create_directories(layout/L"ProjectPBZ/PBZModLoader");
    { std::ofstream f(layout/L"ProjectPBZ/PBZModLoader/mods.ini"); f<<"fixture"; }
    { std::ofstream f(layout/L"ProjectPBZ/Content/JavaScript/PBZModLoader/bootstrap.js"); f<<"fixture"; }
    for(const auto& relative:{L"platform/steam/PBZ.exe",L"platform/wegame/PBZ.exe",L"ProjectPBZ/Binaries/Win64/PBZ.exe",L"PBZ.exe"})
        assert(findProject(layout/relative)==fs::canonical(layout/L"ProjectPBZ"));
    fs::remove(layout/L"ProjectPBZ/PBZModLoader/mods.ini");
    bool missing=false; try { findProject(layout/L"platform/wegame/PBZ.exe"); } catch(const std::exception&) { missing=true; } assert(missing);
    fs::remove_all(layout);
    std::cout<<"PASS platform-independent project discovery and missing runtime rejection\n";
    std::cout<<"PASS MOD cache: nested dependencies, stale cleanup, source and loader preservation\n";
    std::cout<<"PASS native redirect, argv, counter, relocated flags, filtering, bounds, concurrency, ring wrap\n";
    std::cout<<"PASS native config: valid, missing, disabled, traversal, TS, invalid switch, duplicate ID (case-insensitive)\n";
    std::cout<<"PASS automatic discovery: sorting, dependencies ignored, switches, legacy merge, removal, empty directory\n";
}
