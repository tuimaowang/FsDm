// 直接编译生产数组管理器，检查真实登记数量和释放边界，不增加测试专用 DLL 导出。
// 字符串兼容登记和通用结构体共用释放入口，覆盖跨线程和工作台精确释放。
#include "../ZhaoTu/ZifuchuanNeicun.h"
#include "../ZhaoTu/TongyongJieguo.h"
#include "../ZhaoTu/ZhaoTu.h"
#include "../Gonggong/Gongzuotai.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace fsdm::zifuchuan;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
constexpr std::array names{
    "GetColor", "MatchPicName", "FindColorEx", "FindMultiColorEx",
    "FindPicS", "FindPicEx", "FindPicExS", "GetOcrError",
    "Ocr", "OcrEx", "OcrExOne", "OcrInFile",
    "FindStrS", "FindStrEx", "FindStrExS",
    "FindStrFastS", "FindStrFastEx", "FindStrFastExS"
};
void Ownership() {
    Require(FreeAllStringResults() == 1 && Outstanding() == 0, "initial cleanup");
    std::vector<const char*> results;
    for (size_t i = 0; i < names.size(); ++i) {
        const auto function = static_cast<Function>(i);
        const auto* empty = Save(function, "");
        std::string source = "中文,123,456";
        const auto* text = Save(function, source);
        source.assign(4096, 'X');
        Require(empty && text && empty != text && empty[0] == '\0' &&
            std::strcmp(text, "中文,123,456") == 0, "independent empty/UTF-8 arrays");
        results.push_back(empty);results.push_back(text);
    }
    Require(Outstanding() == names.size() * 2, "all results registered");
    for (size_t i = 0; i < results.size(); ++i) {
        FreeOne(results[i]);
        Require(Outstanding() == results.size() - i - 1, "per-address internal array cleanup");
    }

    const std::string large(1024 * 1024, 'Q');
    const auto* retained = Save(Function::FindPicEx, large);
    const auto* other = Save(Function::FindPicExS, "keep");
    {
        WorkbenchResult own(Save(Function::FindPicEx, "workbench"));
        Require(Outstanding() == 3, "workbench temporary registered");
    }
    Require(Outstanding() == 2 && retained && std::string_view(retained) == large &&
        std::strcmp(other, "keep") == 0, "workbench frees only its own result");
    FreeOne(retained);
    Require(Outstanding() == 1 && std::strcmp(other, "keep") == 0, "other array retained");
    FreeOne(other); FreeOne(other); FreeOne(nullptr); FreeOne("not registered");
    Require(Outstanding() == 0, "precise and repeated internal cleanup");
    Require(!Save(Function::count, "bad"), "invalid internal function rejected");
}
void Threads() {
    const char* fromThread{};
    std::thread producer([&] { fromThread = Save(Function::GetColor, "aabbcc"); });
    producer.join();
    Require(fromThread && std::strcmp(fromThread, "aabbcc") == 0 && Outstanding() == 1,
        "thread exit preserves published array");
    std::thread consumer([&] { FreeOne(fromThread); });
    consumer.join();
    Require(Outstanding() == 0, "other thread releases array");

    std::atomic<bool> correct{true};
    std::vector<std::thread> workers;
    for (size_t i = 0; i < names.size(); ++i) workers.emplace_back([&, i] {
        const std::string expected = std::string(names[i]) + ":中文";
        for (int k = 0; k < 100; ++k) {
            const auto* result = Save(static_cast<Function>(i), expected);
            if (!result || std::strcmp(result, expected.c_str())) correct = false;
            // 各线程只释放本次创建的地址，不删除其他线程正在读取的数组。
            FreeOne(result);
        }
    });
    for (auto& worker : workers) worker.join();
    Require(correct && Outstanding() == 0, "parallel registration and exact cleanup");

    workers.clear();
    for (int i = 0; i < 8; ++i) workers.emplace_back([] {
        for (int k = 0; k < 200; ++k) Save(Function::Ocr, "same function");
    });
    for (auto& worker : workers) worker.join();
    Require(Outstanding() == 1600, "same function concurrent results retained");
    Require(FreeAllStringResults() == 1 && Outstanding() == 0 &&
        FreeAllStringResults() == 1, "global and repeated cleanup");
}
void NativeResults() {
    namespace j=fsdm::jieguo;
    FreeAllResults();
    j::Item item;
    item.fields=FS_JG_ID|FS_JG_XY|FS_JG_COLOR|FS_JG_TEXT|FS_JG_FILE;
    item.id=2;item.x=303;item.y=230;item.color=0x12f3f3;
    item.text="中文";item.file="test.bmp";
    std::vector<j::Item> values(1500,item);
    values.back().x=678;values.back().y=72;
    const auto* result=j::Save("FindMultiColorEx",0,0,values,"完整全文");
    item.text="改变";values.clear();
    Require(result && result->size==sizeof(*result) && result->version==1 &&
        result->status==0 && result->count==1500 &&
        result->items[1499].x==678 && result->items[1499].y==72 &&
        result->items[0].id==2 && result->items[0].color==0x12f3f3 &&
        !strcmp(result->items[0].text,"中文") && !strcmp(result->items[0].file,"test.bmp") &&
        !strcmp(result->fullText,"完整全文"),"native array and deep copied fields");
    const auto* other=j::Save("FindMultiColorEx",0,0,{});
    Require(other && result!=other && other->count==0 && !other->items && !*other->fullText,
        "independent successful no-match result");
    {
        j::WorkbenchResult temporary(j::Save("FindMultiColorEx",0,0,{item}));
        Require(j::Outstanding()==3,"native workbench owner registered");
    }
    Require(j::Outstanding()==2 && result->items[1499].x==678,
        "workbench frees only its result, retained large result stays readable");
    Require(FreeResult(other) && !FreeResult(other) && FreeResult(nullptr) &&
        !FreeResult(reinterpret_cast<const FsTongyongJieguo*>(1)),
        "exact free, null and unregistered addresses");
    const auto* failure=j::Save("FindPic",FS_CAIJI_SHIBAI,123,{item},"不可发布");
    Require(failure && failure->status==FS_CAIJI_SHIBAI && failure->detail==123 &&
        !failure->count && !failure->items && !*failure->fullText,
        "errors suppress partial items and text");
    // 同名函数再次返回结构体后，只释放旧地址，新旧结果不能互相影响。
    const auto* retained=j::Save("FindMultiColorEx",0,0,{item},"保留结果");
    Require(retained && j::Outstanding()==3 && FreeResult(result) && j::Outstanding()==2 &&
        retained->count==1 && !strcmp(retained->fullText,"保留结果") &&
        failure->status==FS_CAIJI_SHIBAI,"exact cleanup preserves same and other function results");
    const auto* legacy=Save(Function::MatchPicName,"MatchPicName");
    Require(legacy && Outstanding()==1,"legacy array retained until global cleanup");
    Require(FreeAllResults() && j::Outstanding()==0 && Outstanding()==0,"mixed global cleanup");

    const FsTongyongJieguo* fromThread{};
    std::thread producer([&] {fromThread=j::Save("GetColor",0,0,{item});});
    producer.join();
    Require(fromThread && fromThread->items[0].color==0x12f3f3,"native producer exit retains data");
    std::thread consumer([&] {Require(FreeResult(fromThread)==1,"native cross-thread exact free");});
    consumer.join();
    std::atomic_bool ok{true};std::vector<std::thread> workers;
    for(int i=0;i<8;++i) workers.emplace_back([&] {
        for(int k=0;k<100;++k) {
            const auto* r=j::Save("FindColorEx",0,0,{item});
            if(!r || r->items[0].color!=0x12f3f3 || !FreeResult(r)) ok=false;
        }
    });
    for(auto& thread:workers) thread.join();
    Require(ok && !j::Outstanding(),"concurrent native publication and exact cleanup");
}
// 使用独立的非活动屏幕缓冲区验证超长全文，不改工作台正在显示的内容。
// 三万字符超过旧输出上限及单个写入块，逐字符核对，防止只保留首尾的假通过。
void ConsoleLongText() {
    const bool allocated=AllocConsole()!=FALSE;
    HANDLE output=CreateConsoleScreenBuffer(GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CONSOLE_TEXTMODE_BUFFER,nullptr);
    struct Owner {
        HANDLE output;bool allocated;
        ~Owner() {
            if(output!=INVALID_HANDLE_VALUE) CloseHandle(output);
            if(allocated) FreeConsole();
        }
    } owner{output,allocated};
    Require(output!=INVALID_HANDLE_VALUE,"create isolated console buffer");
    Require(SetConsoleScreenBufferSize(output,COORD{200,400})!=FALSE,"long text buffer size");
    Require(SetConsoleMode(output,ENABLE_PROCESSED_OUTPUT|ENABLE_WRAP_AT_EOL_OUTPUT)!=FALSE,
        "long text console wrap mode");
    Require(SetConsoleCursorPosition(output,COORD{0,0})!=FALSE,"long text cursor start");
    std::wstring source(30000,L'A');
    for(size_t i=0;i<source.size();++i) source[i]=L'A'+wchar_t(i%26);
    FsConsoleLine(output,source.c_str());
    std::wstring actual(source.size(),L'\0');DWORD read{};
    Require(ReadConsoleOutputCharacterW(output,actual.data(),DWORD(actual.size()),COORD{0,0},&read)!=FALSE &&
        read==source.size() && actual==source,"complete console output beyond chunk boundaries");
}
}
int main() {
    try {
        Ownership(); Threads(); NativeResults(); ConsoleLongText();
        std::puts("PASS: native and string results, lifetime, exact ownership and concurrent cleanup");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        FreeAllStringResults();
        return 1;
    }
}
