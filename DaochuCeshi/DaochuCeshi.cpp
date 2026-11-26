// 外部调用方示例：只包含交付头文件，所有函数均通过动态加载得到。
// 测试不注入、不绑定用户窗口；通过安全的未绑定及错误参数分支验证接口契约。
#include "FsDmJiekou.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using FsDm_ZhuaTuBeginFn = int (WINAPI*)(HWND, const char*, const char*, const char*, int);
using FsDm_ZhuaTuEndFn = int (WINAPI*)(void);
using FsDm_ZhuaTuSetCaptureFrameIntervalFn = int (WINAPI*)(int);
using FsDm_ZhuaTuCopyFrameFn = int (WINAPI*)(void*, uint64_t, FsZhen*);
using FsDm_ZhuaTuGetStatusFn = int (WINAPI*)(FsZhuangtai*);
using FsDm_ZhuaTuAcquireGpuFrameFn = int (WINAPI*)(FsGpuZhen*);
using FsDm_ZhuaTuReleaseGpuFrameFn = int (WINAPI*)(uint64_t);
using FsDm_ZhuaTuGetCaptureStatusFn = int (WINAPI*)(FsCaijiZhuangtai*);
using FsDm_ZhuaTuAcquireProcessorFn = HRESULT (WINAPI*)(uint32_t, int32_t, FsGpuChuli*);
using FsDm_ZhuaTuReleaseProcessorFn = HRESULT (WINAPI*)(uint64_t);
using FsDm_ZhuaTuMapPointFn = int (WINAPI*)(double, double, double*, double*, int);
struct DaChu { bool zhuatu; const char* name; };
constexpr DaChu exports[] = {
#define DAOCHU_ZHAOTU(name) {false, #name},
#define DAOCHU_ZHUATU(name) {true, #name},
#include "DaochuBiao.inc"
#undef DAOCHU_ZHAOTU
#undef DAOCHU_ZHUATU
};

std::string Utf8(const std::wstring& value) {
    if(value.empty()) return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!n) throw std::runtime_error("路径不是有效 Unicode");
    std::string out(n,0);
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),
        out.data(),n,nullptr,nullptr);
    return out;
}
fs::path Ziji() {
    std::wstring path(32768,0);
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n>=path.size()) throw std::runtime_error("无法取得测试程序路径");
    path.resize(n);return path;
}

// 同时写屏幕与 UTF-8 报告，失败项不会被最后一行汇总掩盖。
struct Jilu {
    std::ofstream file;
    unsigned passed{},failed{};
    explicit Jilu(const fs::path& path):file(path,std::ios::binary|std::ios::trunc) {
        if(!file) throw std::runtime_error("无法写入测试报告");
        file << "\xef\xbb\xbf";
    }
    void Line(const std::string& text) {
        std::cout<<text<<std::endl;file<<text<<'\n';file.flush();
    }
    void Check(bool ok,const std::string& text) {
        ok?++passed:++failed;Line(std::string(ok?"[PASS] ":"[FAIL] ")+text);
    }
};
HMODULE Jiazai(const fs::path& path) {
    HMODULE m=LoadLibraryExW(path.c_str(),nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!m) throw std::runtime_error("加载失败 "+Utf8(path.wstring())+
        "，Win32="+std::to_string(GetLastError()));
    // DLL 内部固定模块并安装钩子；不在运行中尝试卸载，让进程退出完成清理。
    return m;
}
template<class T> T Hanshu(HMODULE m,const char* name) {
    auto address=GetProcAddress(m,name);
    if(!address) throw std::runtime_error(std::string("缺少函数：")+name);
    return reinterpret_cast<T>(address);
}
#define QU(module,name) Hanshu<FsDm_##name##Fn>(module,#name)
template<class T> T Chushihua() { T value{};value.size=sizeof(value);return value; }

// 从已加载模块读取 Windows PE 导出名称，双向比较可发现头文件漏列的导出。
void DaochuBiao(Jilu& log,HMODULE zhao,HMODULE zhua) {
    for(bool lower:{false,true}) {
        HMODULE module=lower?zhua:zhao;
        std::set<std::string> expected;
        for(const auto& e:exports) if(e.zhuatu==lower) {
            expected.emplace(e.name);
            log.Check(GetProcAddress(module,e.name)!=nullptr,
                std::string(lower?"ZhuaTu.dll!":"ZhaoTu.dll!")+e.name);
        }
        const auto* base=reinterpret_cast<const unsigned char*>(module);
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        const auto& data=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if(!data.VirtualAddress) {log.Check(false,"模块没有导出表");continue;}
        const auto* table=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+data.VirtualAddress);
        const auto* names=reinterpret_cast<const DWORD*>(base+table->AddressOfNames);
        std::set<std::string> actual;
        for(DWORD i=0;i<table->NumberOfNames;++i)
            actual.emplace(reinterpret_cast<const char*>(base+names[i]));
        log.Check(expected==actual,std::string(lower?"ZhuaTu":"ZhaoTu")+
            " 头文件与实际导出名称完全一致，共 "+std::to_string(actual.size())+" 个");
        log.Check(table->NumberOfFunctions==actual.size(),"没有未声明的仅序号导出");
    }
    for(const char* removed:{"FreeFunctionResults","FreeStringResult","SetCaptureBackend","SetPicPwd"})
        log.Check(!GetProcAddress(zhao,removed),std::string("已移除接口不存在：")+removed);
}

// 每次检查后通过 DLL 自己释放，绝不跨模块 delete 返回的指针。
void Jieguo(Jilu& log,HMODULE zhao,const char* label,const FsTongyongJieguo* r,int status) {
    const bool ok=r && r->size==sizeof(*r) && r->version==FSDM_BANBEN &&
        r->status==status && r->count==0 && !r->items && r->fullText && !*r->fullText;
    log.Check(ok,std::string(label)+" 结果结构/状态="+
        (r?std::to_string(r->status):"nullptr"));
    log.Check(QU(zhao,FreeResult)(r)==1,std::string(label)+" 指定结构释放");
}

void Shibie(Jilu& log,HMODULE zhao) {
    for(const char* backend:{"cpu","gpu"}) {
        log.Check(QU(zhao,SetFindBackend)(backend)==1,std::string("选择查找后端 ")+backend);
        log.Line(std::string("检查未绑定识别分支：")+backend);
        Jieguo(log,zhao,"GetColor",QU(zhao,GetColor)(0,0),FS_WEIBANGDING);
        Jieguo(log,zhao,"CmpColor",QU(zhao,CmpColor)(0,0,"ffffff-000000",1),FS_WEIBANGDING);
        for(const char* name:{"FindColor","FindColorEx"}) {
            auto fn=Hanshu<FsDm_FindColorFn>(zhao,name);
            Jieguo(log,zhao,name,fn(0,0,100,100,"ffffff-000000",1,0),FS_WEIBANGDING);
        }
        for(const char* name:{"FindMultiColor","FindMultiColorEx"}) {
            auto fn=Hanshu<FsDm_FindMultiColorFn>(zhao,name);
            Jieguo(log,zhao,name,fn(0,0,100,100,"ffffff-000000","1|0|000000",1,0),FS_WEIBANGDING);
        }
        for(const char* name:{"FindPic","FindPicS","FindPicEx","FindPicExS"}) {
            auto fn=Hanshu<FsDm_FindPicFn>(zhao,name);
            Jieguo(log,zhao,name,fn(0,0,100,100,"DaochuCeshi.bmp","000000",1,0),FS_WEIBANGDING);
        }
        for(const char* name:{"Ocr","OcrEx","OcrExOne"}) {
            auto fn=Hanshu<FsDm_OcrFn>(zhao,name);
            Jieguo(log,zhao,name,fn(0,0,100,100,"",1),FS_WEIBANGDING);
        }
        Jieguo(log,zhao,"OcrInFile 空路径",QU(zhao,OcrInFile)(0,0,100,100,"","",1),FS_CANSHU_CUOWU);
        for(const char* name:{"FindStr","FindStrS","FindStrEx","FindStrExS",
            "FindStrFast","FindStrFastS","FindStrFastEx","FindStrFastExS"}) {
            auto fn=Hanshu<FsDm_FindStrFn>(zhao,name);
            Jieguo(log,zhao,name,fn(0,0,100,100,"测试","ffffff-000000",1),FS_WEIBANGDING);
        }
    }
}

void Zhuangtai(Jilu& log,HMODULE zhao,HMODULE zhua) {
    log.Check(QU(zhao,UnBindWindow)()==1 && QU(zhao,UnBindWindow)()==1,"上层重复解绑成功");
    log.Check(QU(zhua,ZhuaTuEnd)()==FS_CHENGGONG,"底层解绑成功");
    auto s=Chushihua<FsZhuangtai>();
    log.Check(QU(zhao,GetStatus)(&s)==0 && s.version==1 && !s.bound && !s.hasFrame &&
        s.result==FS_WEIBANGDING,"上层状态结构及未绑定状态");
    s=Chushihua<FsZhuangtai>();
    log.Check(QU(zhua,ZhuaTuGetStatus)(&s)==0 && s.version==1 && !s.bound && !s.hasFrame,
        "底层状态结构及未绑定状态");
    log.Check(QU(zhao,GetStatus)(nullptr)==FS_CANSHU_CUOWU &&
        QU(zhua,ZhuaTuGetStatus)(nullptr)==FS_CANSHU_CUOWU,"状态查询拒绝空结构");
    auto bad=Chushihua<FsZhuangtai>();--bad.size;
    log.Check(QU(zhao,GetStatus)(&bad)==FS_CANSHU_CUOWU &&
        QU(zhua,ZhuaTuGetStatus)(&bad)==FS_CANSHU_CUOWU,"状态查询拒绝错误结构大小");
    auto frame=Chushihua<FsZhen>();
    log.Check(QU(zhao,CopyFrame)(nullptr,0,&frame)==FS_WEIBANGDING && frame.version==1 &&
        !frame.requiredBytes,"上层未绑定不返回像素");
    frame=Chushihua<FsZhen>();
    log.Check(QU(zhua,ZhuaTuCopyFrame)(nullptr,0,&frame)==FS_WEIBANGDING,
        "底层未绑定不返回像素");
    auto gpu=Chushihua<FsGpuZhen>();
    log.Check(QU(zhao,AcquireGpuFrame)(&gpu)==FS_WEIBANGDING && !gpu.token,
        "上层未绑定不借出 GPU 纹理");
    gpu=Chushihua<FsGpuZhen>();
    log.Check(QU(zhua,ZhuaTuAcquireGpuFrame)(&gpu)==FS_WEIBANGDING && !gpu.token,
        "底层未绑定不借出 GPU 纹理");
    log.Check(QU(zhao,ReleaseGpuFrame)(0)==FS_CANSHU_CUOWU &&
        QU(zhua,ZhuaTuReleaseGpuFrame)(0)==FS_CANSHU_CUOWU,"无效 GPU 凭据被拒绝");
    double x{},y{};
    log.Check(QU(zhao,MapPoint)(0,0,&x,&y,0)==FS_WEIBANGDING &&
        QU(zhua,ZhuaTuMapPoint)(0,0,&x,&y,0)==FS_WEIBANGDING,"未绑定坐标换算被拒绝");
    auto capture=Chushihua<FsCaijiZhuangtai>();
    log.Check(QU(zhao,GetCaptureStatus)(&capture)==0 && capture.version==1 &&
        capture.cpuReadbacks==0 && capture.activeLeases==0,"上层采集扩展结构");
    capture=Chushihua<FsCaijiZhuangtai>();
    log.Check(QU(zhua,ZhuaTuGetCaptureStatus)(&capture)==0 && capture.version==1 &&
        capture.cpuReadbacks==0 && capture.activeLeases==0,"底层采集扩展结构");
    auto find=Chushihua<FsChazhaoZhuangtai>();
    log.Check(QU(zhao,GetFindStatus)(&find)==0 && find.version==1 && find.backend==1,
        "查找状态结构及默认 GPU");
    auto ocr=Chushihua<FsOcrZhuangtai>();
    log.Check(QU(zhao,GetOcrStatus)(&ocr)==0 && ocr.version==1 && ocr.backend==0 &&
        ocr.engine==0 && !ocr.loadedSessions,"OCR 状态结构及默认 CPU 模型");
    log.Check(QU(zhua,ZhuaTuAcquireProcessor)(0,0,nullptr)==E_INVALIDARG &&
        QU(zhua,ZhuaTuReleaseProcessor)(0)==E_INVALIDARG,"内部设备接口拒绝无效参数");
    log.Check(QU(zhao,BindWindow)(nullptr,"dx","normal","normal",0)==0 &&
        QU(zhua,ZhuaTuBegin)(nullptr,"dx","normal","normal",0)==FS_CANSHU_CUOWU,
        "绑定接口拒绝空窗口");
    QU(zhao,UnBindWindow)();
}

void Peizhi(Jilu& log,HMODULE zhao,const fs::path& directory) {
    const std::string dir=Utf8(directory.wstring());
    log.Check(QU(zhao,SetPath)(dir.c_str())==1,"SetPath 动态调用");
    log.Check(QU(zhao,LoadPic)(nullptr)==0,"LoadPic 拒绝空名称");
    log.Check(QU(zhao,LoadPicByte)(0,0,"DaochuCeshi.bmp")==0,"LoadPicByte 拒绝无效内存");
    log.Check(QU(zhao,FreePic)("DaochuCeshi.bmp")==1,"FreePic 未缓存名称可重复释放");
    log.Check(QU(zhao,Capture)(0,0,10,10,"",2)==0 &&
        QU(zhao,CapturePng)(0,0,10,10,"",2)==0 &&
        QU(zhao,CaptureJpg)(0,0,10,10,"",2)==0 &&
        QU(zhao,CaptureGif)(0,0,10,10,"",0,0)==0,"四种保存接口拒绝空路径");
    log.Check(QU(zhao,GetScreenData)(0,0,10,10)==0,"未绑定原始像素为空");
    log.Check(QU(zhao,IsDisplayDead)(0,0,10,10,0)==0,"未绑定不误判卡屏");
    const char* names=QU(zhao,MatchPicName)("__DaochuCeshi_BuCunzai_*.bmp");
    log.Check(names && !*names,"MatchPicName 无匹配返回空文字数组");
    log.Check(QU(zhao,SetOcrBackend)("cpu")==1 &&
        QU(zhao,SetOcrEngine)("model")==1,"OCR 设置接口动态调用");
    log.Check(QU(zhao,SetOcrModel)(-1,nullptr)==0 &&
        QU(zhao,UseOcrModel)(-1)==0 && QU(zhao,FreeOcrModel)(-1)==0,"模型接口拒绝无效序号");
    log.Check(QU(zhao,SetOcrModelFiles)(-1,nullptr,nullptr,nullptr,nullptr)==0,
        "SetOcrModelFiles 可调用并拒绝空模型路径");
    log.Check(QU(zhao,SetDict)(-1,nullptr)==0 && QU(zhao,UseDict)(-1)==0 &&
        QU(zhao,ClearDict)(-1)==0 && QU(zhao,GetDictCount)(-1)==0,"字库接口拒绝无效序号");
    log.Check(QU(zhao,GetNowDict)()==0,"GetNowDict 返回默认字库序号");
    log.Check(QU(zhao,GetOcrError)()!=nullptr,"GetOcrError 返回文字数组");
}

void Neicun(Jilu& log,HMODULE zhao) {
    const auto get=QU(zhao,GetColor);
    const auto release=QU(zhao,FreeResult);
    const auto* a=get(0,0);const auto* b=get(0,0);
    log.Check(a && b && a!=b,"同函数两次返回独立结构");
    const bool freed=release(a)==1;
    log.Check(freed && b && b->status==FS_WEIBANGDING && b->version==1,
        "释放一个结构不影响同函数其他结果");
    log.Check(release(a)==0,"重复释放被拒绝");
    log.Check(release(b)==1 && release(nullptr)==1,"释放剩余结构及空指针");
    const FsTongyongJieguo* across{};
    std::thread producer([&]{across=get(0,0);});producer.join();
    log.Check(across && across->status==FS_WEIBANGDING,"生产线程退出后结构仍可读取");
    int rc{};std::thread consumer([&]{rc=release(across);});consumer.join();
    log.Check(rc==1,"其他线程可释放指定结构");
    log.Check(get(0,0)!=nullptr && QU(zhao,FreeAllResults)()==1 &&
        QU(zhao,FreeAllResults)()==1,"全量释放及重复全量释放");
    log.Check(get(0,0)!=nullptr && QU(zhao,FreeAllStringResults)()==1,"全量释放兼容别名");
}

// 由不带控制台的独立子进程验证，否则父测试窗口会掩盖 DLL 自动创建控制台的行为。
bool YouGongzuotai() { return GetConsoleWindow()!=nullptr || GetConsoleCP()!=0; }
int JingyinZiJincheng(const fs::path& directory) {
    if(YouGongzuotai()) return 20;
    const auto zhao=Jiazai(directory/L"ZhaoTu.dll");
    const auto zhua=Jiazai(directory/L"ZhuaTu.dll");
    const auto deadline=GetTickCount64()+10000;
    do {
        if(YouGongzuotai()) return 21;
        auto s=Chushihua<FsZhuangtai>();
        if(QU(zhua,ZhuaTuGetStatus)(&s)!=0) return 22;
        if(!s.initializing) {
            if(s.result==FS_CHUSHIHUA_SHIBAI) return 23;
            // 给任何启动线程留出执行机会，避免刚加载就检查得到假通过。
            Sleep(150);
            auto upper=Chushihua<FsZhuangtai>();
            return !YouGongzuotai() && QU(zhao,GetStatus)(&upper)==0 ? 0:24;
        }
        Sleep(10);
    } while(GetTickCount64()<deadline);
    return 25;
}
void Jingyin(Jilu& log,const fs::path& directory) {
    std::wstring command=L"\""+Ziji().wstring()+L"\" --silent-child --dll-dir \""+
        directory.wstring()+L"\"";
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    // 显式脱离控制台启动，避免隐藏控制台与没有控制台混淆。
    // 子进程会先检查基线，再加载 DLL；不能把启动环境误报为 DLL 创建工作台。
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,DETACHED_PROCESS,
        nullptr,nullptr,&start,&process)) {
        log.Check(false,"创建静默验证子进程，Win32="+std::to_string(GetLastError()));return;
    }
    CloseHandle(process.hThread);
    const DWORD wait=WaitForSingleObject(process.hProcess,15000);
    DWORD code=STILL_ACTIVE;GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess);
    log.Check(wait==WAIT_OBJECT_0 && code==0,
        "两个 DLL 关闭工作台验证，子进程退出码="+std::to_string(code));
}
}

int wmain(int argc,wchar_t** argv) {
    bool pause=true,child=false;
    try {
        fs::path directory=Ziji().parent_path();
        for(int i=1;i<argc;++i) {
            const std::wstring arg=argv[i];
            if(arg==L"--no-pause") pause=false;
            else if(arg==L"--silent-child") {child=true;pause=false;}
            else if(arg==L"--dll-dir" && i+1<argc) directory=fs::absolute(argv[++i]);
            else throw std::runtime_error("参数：--no-pause | --dll-dir 目录");
        }
        if(child) return JingyinZiJincheng(directory);
        SetConsoleOutputCP(CP_UTF8);
        Jilu log(Ziji().parent_path()/L"DaochuCeshiJieguo.txt");
        log.Line("FsDm 独立导出测试（Windows x64）");
        log.Line("DLL 目录："+Utf8(directory.wstring()));
        Jingyin(log,directory);
        const auto zhao=Jiazai(directory/L"ZhaoTu.dll");
        const auto zhua=Jiazai(directory/L"ZhuaTu.dll");
        DaochuBiao(log,zhao,zhua);
        Zhuangtai(log,zhao,zhua);Peizhi(log,zhao,directory);
        Shibie(log,zhao);Neicun(log,zhao);
        QU(zhao,UnBindWindow)();
        log.Line("汇总：通过 "+std::to_string(log.passed)+"，失败 "+std::to_string(log.failed));
        log.Line("本测试验证导出及安全调用；实际画面和识别正确性需绑定场景另测。");
        if(pause) {log.Line("按回车退出。");std::cin.get();}
        return log.failed?1:0;
    } catch(const std::exception& e) {
        std::cerr<<"[FAIL] "<<e.what()<<std::endl;
        if(pause) {std::cerr<<"按回车退出。"<<std::endl;std::cin.get();}
        return 2;
    }
}
