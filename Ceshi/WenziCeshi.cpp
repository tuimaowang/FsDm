// OCR 集成测试：构造真实文字与点阵图，通过公开 C 接口验证识别、找字和资源复用。
// 测试文件只写入本进程专属临时目录，结束后清理，不依赖用户的图片或字库。
#include "../ZhaoTu/ZhaoTu.h"
#include "Xuanran.h"
#include "JieguoShipei.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

// 旧算法断言经测试适配器读取真实结构体；公开接口本身不保留旧返回格式。
namespace {
const auto RawOcrEx=&::OcrEx;
const auto RawFindStrEx=&::FindStrEx;
fsdm::ceshi::Adapter<decltype(&::Ocr),fsdm::ceshi::Kind::fullText,false,true> CeshiOcr{&::Ocr,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::OcrEx),fsdm::ceshi::Kind::ocrEx,false,true> CeshiOcrEx{&::OcrEx,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::OcrExOne),fsdm::ceshi::Kind::ocrExOne,false,true> CeshiOcrExOne{&::OcrExOne,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::OcrInFile),fsdm::ceshi::Kind::fullText,false,true> CeshiOcrInFile{&::OcrInFile,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStr),fsdm::ceshi::Kind::index,true,true> CeshiFindStr{&::FindStr,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrS),fsdm::ceshi::Kind::textS,true,true> CeshiFindStrS{&::FindStrS,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrEx),fsdm::ceshi::Kind::indexEx,false,true> CeshiFindStrEx{&::FindStrEx,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrExS),fsdm::ceshi::Kind::textEx,false,true> CeshiFindStrExS{&::FindStrExS,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrFast),fsdm::ceshi::Kind::index,true,true> CeshiFindStrFast{&::FindStrFast,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrFastS),fsdm::ceshi::Kind::textS,true,true> CeshiFindStrFastS{&::FindStrFastS,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrFastEx),fsdm::ceshi::Kind::indexEx,false,true> CeshiFindStrFastEx{&::FindStrFastEx,&::FreeResult};
fsdm::ceshi::Adapter<decltype(&::FindStrFastExS),fsdm::ceshi::Kind::textEx,false,true> CeshiFindStrFastExS{&::FindStrFastExS,&::FreeResult};
}
#define Ocr CeshiOcr
#define OcrEx CeshiOcrEx
#define OcrExOne CeshiOcrExOne
#define OcrInFile CeshiOcrInFile
#define FindStr CeshiFindStr
#define FindStrS CeshiFindStrS
#define FindStrEx CeshiFindStrEx
#define FindStrExS CeshiFindStrExS
#define FindStrFast CeshiFindStrFast
#define FindStrFastS CeshiFindStrFastS
#define FindStrFastEx CeshiFindStrFastEx
#define FindStrFastExS CeshiFindStrFastExS

namespace {
void Require(bool value,const char* message) {
    if(value) return;
    std::fprintf(stderr,"FAIL: %s; OCR: %s\n",message,GetOcrError());throw Failure{};
}
std::string Utf8(const std::filesystem::path& path) {
    const auto s=path.u8string();return {s.begin(),s.end()};
}
struct Fixture {
    std::filesystem::path folder;
    int width=640,height=180;
    std::vector<uint32_t> pixels=std::vector<uint32_t>(size_t(width)*height,0xffffffff);
    Fixture() {
        wchar_t temp[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH,temp)!=0,"temporary directory");
        folder=std::filesystem::path(temp)/(L"FsDmWenzi_"+std::to_wstring(GetCurrentProcessId()));
        Require(std::filesystem::create_directory(folder),"create isolated fixture directory");
        Require(SetPath(Utf8(folder).c_str())==1,"set relative path");
    }
    ~Fixture() {std::error_code ec;std::filesystem::remove_all(folder,ec);}
    void Save(const wchar_t* filename) {
        // 独立 BMP24 写入器，避免使用被测截图保存接口来生成期望输入。
        const int stride=(width*3+3)&~3;
        BITMAPFILEHEADER head{};BITMAPINFOHEADER info{};
        head.bfType=0x4d42;head.bfOffBits=sizeof(head)+sizeof(info);head.bfSize=head.bfOffBits+stride*height;
        info.biSize=sizeof(info);info.biWidth=width;info.biHeight=height;info.biPlanes=1;info.biBitCount=24;
        std::ofstream out(folder/filename,std::ios::binary);
        out.write(reinterpret_cast<const char*>(&head),sizeof(head));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
        std::vector<unsigned char> row(stride);
        for(int y=height-1;y>=0;--y) {
            for(int x=0;x<width;++x) {
                const auto p=pixels[size_t(y)*width+x];
                row[x*3]=static_cast<unsigned char>(p);row[x*3+1]=static_cast<unsigned char>(p>>8);
                row[x*3+2]=static_cast<unsigned char>(p>>16);
            }
            out.write(reinterpret_cast<const char*>(row.data()),row.size());
        }
        Require(out.good(),"save independent BMP fixture");
    }
    void Text() {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
        info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* data{};HDC dc=CreateCompatibleDC(nullptr);
        HBITMAP dib=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
        Require(dc&&dib&&data,"GDI text DIB");
        const auto oldBitmap=SelectObject(dc,dib);PatBlt(dc,0,0,width,height,WHITENESS);
        HFONT font=CreateFontW(-44,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei");
        const auto oldFont=SelectObject(dc,font);SetTextColor(dc,RGB(0,0,0));SetBkColor(dc,RGB(255,255,255));
        Require(TextOutW(dc,24,20,L"TEST 123",8)!=0,"draw Latin text");
        Require(TextOutW(dc,24,95,L"开始游戏",4)!=0,"draw Chinese text");
        GdiFlush();std::copy_n(static_cast<uint32_t*>(data),pixels.size(),pixels.data());
        SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(font);DeleteObject(dib);DeleteDC(dc);
        Save(L"wenzi.bmp");
    }
    void Dictionary() {
        // 列位流固定11位、高度7；非对称字形可暴露转置、上下翻转和补位错误。
        constexpr const char* rows[]={"01110","10001","10001","11111","10001","10001","10001"};
        std::string bits;
        for(int x=0;x<5;++x) for(int y=0;y<11;++y) bits+=y<7?rows[y][x]:'0';
        while(bits.size()%4) bits+='0';
        std::string hex;
        for(size_t i=0;i<bits.size();i+=4) {
            int n=0;for(int k=0;k<4;++k) n=n*2+bits[i+k]-'0';hex+="0123456789ABCDEF"[n];
        }
        {std::ofstream out(folder/L"ziku.txt",std::ios::binary);out<<"\xef\xbb\xbf"<<hex<<"$A$0$0$7\r\n";}
        // TSPlug 将高度字段独立于固定11位列点阵保存；覆盖高字段兼容及不越界匹配。
        for(const int sourceHeight:{12,23,255}) {
            std::ofstream out(folder/(L"ziku"+std::to_wstring(sourceHeight)+L".txt"),std::ios::binary);
            out<<hex<<"$A$0$0$"<<sourceHeight<<"\r\n";
            Require(out.good(),"write tall TSPlug dictionary fixture");
        }
        {std::ofstream out(folder/L"cuowu.txt");out<<hex<<"$A$0$0$256\n";}
        std::fill(pixels.begin(),pixels.end(),0xffffffff);
        for(const int start:{20,50}) for(int y=0;y<7;++y) for(int x=0;x<5;++x)
            if(rows[y][x]=='1') pixels[size_t(30+y)*width+start+x]=0xff000000;
        Save(L"zixing.bmp");
    }
    void FastDictionary() {
        std::fill(pixels.begin(),pixels.end(),0xffffffff);
        std::ofstream out(folder/L"kuaisu.txt",std::ios::binary);
        // 测试独立按行描述字形，再转换为固定11位列格式，不复用生产匹配器或字库解析器。
        auto glyph=[&](const std::string& text,const std::vector<std::string>& rows) {
            std::string bits,hex;
            for(size_t x=0;x<rows[0].size();++x) for(size_t y=0;y<11;++y)
                bits+=y<rows.size()?rows[y][x]:'0';
            while(bits.size()%4) bits+='0';
            for(size_t i=0;i<bits.size();i+=4) {
                int n=0;for(int k=0;k<4;++k) n=n*2+bits[i+k]-'0';hex+="0123456789ABCDEF"[n];
            }
            out<<hex<<'$'<<text<<"$0$0$"<<rows.size()<<"\n";
        };
        auto paint=[&](const std::vector<std::string>& rows,int left,int top) {
            for(size_t y=0;y<rows.size();++y) for(size_t x=0;x<rows[y].size();++x)
                if(rows[y][x]=='1') pixels[size_t(top+int(y))*width+left+x]=0xff000000;
        };
        const std::vector<std::string> a={"01110","10001","10001","11111","10001","10001","10001"};
        const std::vector<std::string> b={"11110","10001","10001","11110","10001","10001","11110"};
        const std::vector<std::string> c={"11111","00000","00000","11111","00000","00000","11111"};
        const std::vector<std::string> low={"1111","1001","1111"};
        const std::vector<std::string> high={"01110","10001","10001","10001","10101","10001","10001","10001","11111"};
        glyph("张",a);glyph("和",b);glyph("三",c);glyph("矮",low);glyph("高",high);
        std::vector<std::string> combined;
        for(size_t y=0;y<a.size();++y) combined.push_back(a[y]+"0"+c[y]);
        glyph("张三",combined);
        // 大量无关字形验证筛选路径；普通识别同分时仍应选原字库中排在前面的“张”。
        for(int i=0;i<1000;++i) glyph("无关"+std::to_string(i),a);
        Require(out.good(),"write fast dictionary");out.close();
        paint(a,20,30);paint(b,32,30);paint(c,44,30);
        paint(a,20,70);paint(c,32,70);paint(combined,200,30);
        paint(a,120,120);pixels[size_t(121)*width+120]=0xffffffff; // 首列缺一笔，验证模糊分支。
        paint(low,200,150);pixels[size_t(153)*width+200]=0xff000000; // 实际高度下方有笔画。
        paint(high,250,170);paint(low,636,177); // 右边界与下边界恰好贴边。
    }
};
FsOcrZhuangtai State() {
    FsOcrZhuangtai state{};state.size=sizeof(state);
    Require(GetOcrStatus(&state)==FS_CHENGGONG,"OCR status ABI");return state;
}
void ModelWindowTest(const Fixture& f);
void PpOcrV3Test(Fixture& f,const char* modelDirectory,bool directml) {
    f.Text();
    Require(SetOcrBackend(directml?"directml":"cpu")==1,"select PP-OCRv3 backend");
    Require(SetOcrEngine("model")==1,"select neural OCR for PP-OCRv3");
    Require(SetOcrModel(1,modelDirectory)==1,"register PP-OCRv3 model directory");
    Require(UseOcrModel(1)==1,"select PP-OCRv3 model");
    const std::string text=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
    const auto state=State();
    std::printf("PP-OCRv3 %s: %s; code=%d init=%.3f det=%.3f rec=%.3f ms\n",
        directml?"DirectML":"CPU",text.c_str(),state.result,state.initializationMs,state.detectionMs,state.recognitionMs);
    Require(state.result==FS_CHENGGONG,"PP-OCRv3 inference status");
    Require(!text.empty()&&text.find("开始游戏")!=text.npos,
        "PP-OCRv3 Chinese recognition");
    Require(state.loadedSessions==3&&state.sessionsCreated==3,
        "PP-OCRv3 detector, recognizer, and classifier sessions");
    Require(std::string(OcrInFile(0,0,640,180,"wenzi.bmp","",.5))==text,
        "PP-OCRv3 repeated inference");
    Require(State().sessionsCreated==3,"PP-OCRv3 sessions are reused");
    std::reverse(f.pixels.begin(),f.pixels.end());
    f.Save(L"wenzi_180.bmp");
    const std::string inverted=OcrInFile(0,0,640,180,"wenzi_180.bmp","",.5);
    Require(State().result==FS_CHENGGONG&&inverted.find("开始游戏")!=inverted.npos,
        "PP-OCRv3 180-degree orientation correction");
    Require(FreeOcrModel(1)==1&&State().loadedSessions==0,
        "release PP-OCRv3 sessions");

    const auto folder=std::filesystem::path(modelDirectory);
    const auto detector=std::filesystem::is_regular_file(folder/L"detCN.onnx")
        ?folder/L"detCN.onnx":folder/L"ch_PP-OCRv3_det_infer.onnx";
    const auto recognizer=std::filesystem::is_regular_file(folder/L"RecCN.onnx")
        ?folder/L"RecCN.onnx":folder/L"ch_PP-OCRv3_rec_chs.onnx";
    const auto classifier=std::filesystem::is_regular_file(folder/L"cls.onnx")
        ?folder/L"cls.onnx":folder/L"ch_ppocr_mobile_v2.0_cls_infer.onnx";
    const auto dictionary=std::filesystem::is_regular_file(folder/L"KeysCN.txt")
        ?folder/L"KeysCN.txt":folder/L"ppocr_keys_chs.txt";
    const auto detectorUtf8=Utf8(detector),recognizerUtf8=Utf8(recognizer);
    const auto classifierUtf8=Utf8(classifier),dictionaryUtf8=Utf8(dictionary);
    Require(SetOcrModelFiles(2,classifierUtf8.c_str(),detectorUtf8.c_str(),
        recognizerUtf8.c_str(),dictionaryUtf8.c_str())==1,
        "register explicit PP-OCRv3 component files");
    Require(UseOcrModel(2)==1,"select explicit PP-OCRv3 components");
    const std::string explicitText=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
    Require(State().result==FS_CHENGGONG&&explicitText.find("开始游戏")!=explicitText.npos,
        "explicit PP-OCRv3 component inference");
    Require(State().loadedSessions==3&&FreeOcrModel(2)==1&&State().loadedSessions==0,
        "explicit component sessions released");
    Require(SetOcrModelFiles(3,"",detectorUtf8.c_str(),recognizerUtf8.c_str(),
        dictionaryUtf8.c_str())==1&&UseOcrModel(3)==1,
        "register PP-OCRv3 components without orientation classifier");
    const std::string withoutClassifier=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
    Require(State().result==FS_CHENGGONG&&withoutClassifier.find("开始游戏")!=withoutClassifier.npos&&
        State().loadedSessions==2,"inference works without orientation classifier");
    Require(FreeOcrModel(3)==1&&State().loadedSessions==0,
        "release components without orientation classifier");

    const auto detectorEn=std::filesystem::is_regular_file(folder/L"detEN.onnx")
        ?folder/L"detEN.onnx":detector;
    const auto recognizerEn=std::filesystem::is_regular_file(folder/L"RecEN.onnx")
        ?folder/L"RecEN.onnx":folder/L"ch_PP-OCRv3_rec_en.onnx";
    const auto dictionaryEn=std::filesystem::is_regular_file(folder/L"KeysEN.txt")
        ?folder/L"KeysEN.txt":folder/L"ppocr_keys_en.txt";
    if(std::filesystem::is_regular_file(recognizerEn)&&std::filesystem::is_regular_file(dictionaryEn)) {
        const auto detectorEnUtf8=Utf8(detectorEn),recognizerEnUtf8=Utf8(recognizerEn);
        const auto dictionaryEnUtf8=Utf8(dictionaryEn);
        Require(SetOcrModelFiles(4,classifierUtf8.c_str(),detectorEnUtf8.c_str(),
            recognizerEnUtf8.c_str(),dictionaryEnUtf8.c_str())==1&&UseOcrModel(4)==1,
            "register explicit PP-OCRv3 English components");
        const std::string english=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
        std::printf("PP-OCRv3 English output: %s; status=%d\n",english.c_str(),State().result);
        Require(State().result==FS_CHENGGONG&&!english.empty()&&State().loadedSessions==3,
            "PP-OCRv3 English model inference");
        Require(FreeOcrModel(4)==1&&State().loadedSessions==0,
            "release explicit English model sessions");
    }
}
void ModelTest(Fixture& f,bool directml) {
    f.Text();
    Require(SetOcrBackend(directml?"directml":"cpu")==1,"select OCR backend");
    Require(SetOcrEngine("model")==1,"select neural model");
    const auto before=State();
    Require(before.loadedSessions==0,"lazy model loading");
    Require(std::string(Ocr(0,0,640,180,"",.5)).empty(),"unbound window OCR rejected");
    Require(State().loadedSessions==0,"unbound OCR did not initialize model");
    auto recognize=[&]() {
        const std::string text=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
        const auto s=State();
        std::printf("%s OCR: %s; code=%d init=%.3f det=%.3f rec=%.3f ms\n",
            directml?"DirectML":"CPU",text.c_str(),s.result,s.initializationMs,s.detectionMs,s.recognitionMs);
        Require(s.result==FS_CHENGGONG,"real model inference");
        Require(text.find("TEST")!=text.npos&&text.find("123")!=text.npos,"Latin and digit recognition");
        Require(text.find("开始游戏")!=text.npos,"Chinese recognition");return text;
    };
    recognize();const auto first=State();
    Require(first.loadedSessions==2&&first.sessionsCreated==2,"one detection/recognition session pair");
    Require(first.devicesCreated==(directml?1u:0u),"persistent DirectML device count");
    recognize();Require(State().sessionsCreated==first.sessionsCreated,"repeat reuses sessions");
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,DWORD(std::size(exe)));
    const auto modelPath=std::filesystem::path(exe).parent_path()/L"ocr_models";
    Require(SetOcrModel(0,Utf8(modelPath).c_str())==1,"set same canonical model directory");
    recognize();Require(State().sessionsCreated==first.sessionsCreated,"same path does not rebuild sessions");
    // 模型互斥锁保护同一个输入缓冲区；并发调用的诊断与结果分别属于各自线程。
    std::array<bool,2> good{};
    std::thread a([&] {const std::string text=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);good[0]=text.find("123")!=text.npos;});
    std::thread b([&] {const std::string text=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);good[1]=text.find("开始游戏")!=text.npos;});
    a.join();b.join();Require(good[0]&&good[1],"concurrent inference");
    Require(State().sessionsCreated==first.sessionsCreated,"concurrent reuse");
    ModelWindowTest(f);
    Require(State().sessionsCreated==first.sessionsCreated,"binding lifecycle preserves model sessions");
    if(directml) {
        // 两个后端各保留一对会话；切回之前用过的后端只切选择，不重建设备或会话。
        Require(SetOcrBackend("cpu")==1,"switch from DirectML to CPU");
        const std::string cpu=OcrInFile(0,0,640,180,"wenzi.bmp","",.5);
        Require(cpu.find("开始游戏")!=cpu.npos&&State().sessionsCreated==4,"CPU pair initialized once");
        Require(SetOcrBackend("directml")==1,"switch back to DirectML");
        recognize();Require(State().sessionsCreated==4&&State().devicesCreated==1,"backend switch reuses DirectML pair");
    }
    Require(!SetOcrModel(1,"missing_models")&&!UseOcrModel(99),"missing model diagnostics");
    Require(std::string(OcrInFile(0,0,640,180,"wenzi.bmp","bad",.5)).empty(),"invalid color");
    Require(std::string(OcrInFile(0,0,9999,180,"wenzi.bmp","",.5)).empty(),"file bounds");
    Require(FreeOcrModel(0)==1&&State().loadedSessions==0,"explicit session release");
    Require(!UseOcrModel(0),"freed model not silently reloaded");
    Require(SetOcrModel(0,Utf8(modelPath).c_str())&&UseOcrModel(0),"explicit model re-registration");
    recognize();Require(State().sessionsCreated==(directml?6u:4u),"one explicit recreation");
    Require(State().devicesCreated==first.devicesCreated,"device survives model release");
}
struct Renderer {
    Window window;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<IDXGISwapChain> chain;
    Renderer(const Fixture& f) {
        SetWindowLongPtrW(window.hwnd,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        Require(SetWindowPos(window.hwnd,nullptr,0,0,f.width,f.height,SWP_NOMOVE|SWP_NOZORDER|SWP_FRAMECHANGED)!=0,"exact client size");
        auto desc=LegacyDesc(window.hwnd);desc.BufferDesc.Width=f.width;desc.BufferDesc.Height=f.height;
        desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        Check(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &desc,&chain,&device,nullptr,&context),"OCR fixture DX11");
        ComPtr<ID3D11Texture2D> back,upload;Check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"OCR backbuffer");
        D3D11_TEXTURE2D_DESC td{};back->GetDesc(&td);td.BindFlags=0;td.MiscFlags=0;
        D3D11_SUBRESOURCE_DATA data{f.pixels.data(),UINT(f.width*4),0};
        Check(device->CreateTexture2D(&td,&data,&upload),"OCR upload");context->CopyResource(back.Get(),upload.Get());
    }
    void Capture() {
        Require(BindWindow(window.hwnd,"dx","normal","normal",0)==1,"bind fixture");
        const auto end=GetTickCount64()+5000;
        for(;;) {
            Check(chain->Present(0,0),"OCR present");Pump();
            FsZhuangtai s{};s.size=sizeof(s);
            if(GetStatus(&s)==FS_CHENGGONG&&s.hasFrame) break;
            Require(GetTickCount64()<end,"wait captured fixture");Sleep(2);
        }
    }
};
void ResultCacheTest(Renderer& renderer,const char* color,double sim) {
    // 固定快照后逐项改变条件，用公开计时确认命中确实跳过分析。
    Require(SetCaptureFrameInterval(-1)&&SetFrameResultCache(0),"enable cache on fixed frame");
    auto call=[&](int right,double confidence) {return RawOcrEx(0,0,right,180,color,confidence);};
    auto analysisSkipped=[] {
        const auto state=State();
        return state.acquireMs==0&&state.preprocessMs==0&&state.detectionMs==0&&state.recognitionMs==0;
    };
    const auto* first=call(640,sim);
    Require(first&&first->status==0&&first->count>0,"first cached OCR result");
    const auto* second=call(640,sim);
    Require(second&&second!=first&&second->status==0&&second->count==first->count&&
        strcmp(second->fullText,first->fullText)==0&&analysisSkipped(),"same frame skips analysis");
    Require(FreeResult(first)&&second->count>0,"cached public results have separate ownership");
    Require(FreeResult(second),"free cached result");
    const auto* afterFree=call(640,sim);
    Require(afterFree&&afterFree->status==0&&analysisSkipped(),"public release preserves cached result");
    FreeResult(afterFree);
    bool threadedHit{};
    std::thread reader([&] {
        const auto* result=call(640,sim);
        threadedHit=result&&result->status==0&&analysisSkipped();
        FreeResult(result);
    });
    reader.join();Require(threadedHit,"result cache shared across threads");
    const auto* changed=call(639,sim);
    Require(changed&&changed->status==0&&State().preprocessMs>0,"changed region reruns analysis");
    FreeResult(changed);
    changed=call(640,sim==1?.9:.6);
    Require(changed&&changed->status==0&&State().preprocessMs>0,"changed threshold reruns analysis");
    FreeResult(changed);
    // 找字接口还需要核对目标文本，正常无命中也应跳过下一次识别。
    const auto* empty=RawFindStrEx(0,0,640,180,"__FSDM_CACHE_MISSING__",color,sim);
    Require(empty&&empty->status==0&&empty->count==0,"successful empty find result");
    FreeResult(empty);
    empty=RawFindStrEx(0,0,640,180,"__FSDM_CACHE_MISSING__",color,sim);
    Require(empty&&empty->status==0&&empty->count==0&&analysisSkipped(),"empty find result reused");
    FreeResult(empty);
    const auto* target=RawFindStrEx(0,0,640,180,"A",color,sim);
    Require(target&&target->status==0&&State().preprocessMs>0,"changed target reruns recognition");
    FreeResult(target);
    target=RawFindStrEx(0,0,640,180,"A",color,sim);
    Require(target&&target->status==0&&analysisSkipped(),"same target reuses find result");
    FreeResult(target);
    const auto* invalid=RawFindStrEx(0,0,640,180,nullptr,color,sim);
    Require(invalid&&invalid->status==FS_CANSHU_CUOWU,"invalid target preserves error behavior");
    FreeResult(invalid);
    Require(SetFrameResultCache(1),"enable result-cache bypass");
    for(int i=0;i<2;++i) {
        const auto* fresh=call(640,sim);
        Require(fresh&&fresh->status==0&&State().preprocessMs>0,"bypass repeats analysis on same frame");
        FreeResult(fresh);
    }
    Require(SetFrameResultCache(0),"disable result-cache bypass");
    const auto state=State();
    Require(state.engine?UseDict(state.dictIndex):UseOcrModel(state.modelIndex),"reselect OCR resource");
    changed=call(640,sim);
    Require(changed&&changed->status==0&&State().preprocessMs>0,"resource selection invalidates old result");
    FreeResult(changed);
    Require(SetCaptureFrameInterval(0),"resume frames");
    Check(renderer.chain->Present(0,0),"publish new frame for result cache");Pump();
    Require(SetCaptureFrameInterval(-1),"freeze new frame");
    changed=call(640,sim);
    Require(changed&&changed->status==0&&State().preprocessMs>0,"new frame reruns analysis");
    FreeResult(changed);
    Require(SetCaptureFrameInterval(0),"restore capture interval");
    std::puts("PASS OCR same-frame results, ownership, threads, parameters, bypass, resource and frame invalidation");
}
void ModelWindowTest(const Fixture& f) {
    Renderer renderer(f);renderer.Capture();
    ResultCacheTest(renderer,"",.5);
    int x=-1,y=-1;
    Require(FindStr(0,0,640,180,"不存在|开始游戏","",.5,&x,&y)==1,"model FindStr target list");
    Require(x>=10&&x<100&&y>=70&&y<160,"model character coordinate bounds");
    Require(std::string(FindStrS(0,0,640,180,"TEST","",.5,&x,&y))=="TEST","model FindStrS");
    Require(std::string(FindStrEx(0,0,640,180,"123","",.5)).starts_with("0,"),"model FindStrEx");
    Require(std::string(FindStrExS(0,0,640,180,"游戏","",.5)).starts_with("游戏,"),"model FindStrExS");
    Require(FindStrFast(0,0,640,180,"不存在|开始游戏","",.5,&x,&y)==1&&x>=10&&y>=70,"model fast target priority");
    Require(std::string(FindStrFastS(0,0,640,180,"TEST","",.5,&x,&y))=="TEST","model fast string result");
    Require(std::string(FindStrFastEx(0,0,640,180,"123","",.5)).starts_with("0,"),"model fast all indices");
    Require(std::string(FindStrFastExS(0,0,640,180,"游戏","",.5)).starts_with("游戏,"),"model fast all text");
    Require(FindStr(0,0,640,180,"开游","",.5,&x,&y)==-1,"ordinary model retains intermediate character");
    Require(FindStrFast(0,0,640,180,"开游","",.5,&x,&y)==0&&x>=10&&y>=70,"model fast skips unrelated character within line");
    Require(FindStrFast(0,0,640,180,"T开","",.5,&x,&y)==-1,"model fast does not concatenate lines");
    Require(std::string(OcrEx(0,0,640,180,"",.5)).find("开$")!=std::string::npos,"model OcrEx UTF-8");
    Require(std::string(OcrExOne(0,0,640,180,"",.5)).starts_with("TEST 123开始游戏|"),"model OcrExOne");
    Require(std::string(Ocr(0,0,640,180,",/",.5))=="TEST 123/开始游戏","line separator");
    // 本项专门验证推理期间解绑，强制分析并发布新快照，确保能观察到真实的像素读回。
    // 同帧结果缓存命中时会立即完成，不能用它制造“识别仍在进行”的测试条件。
    Require(SetFrameResultCache(1)==1,"force analysis for in-flight unbind test");
    Check(renderer.chain->Present(0,0),"new snapshot for in-flight unbind test");Pump();
    FsCaijiZhuangtai before{};before.size=sizeof(before);GetCaptureStatus(&before);
    std::atomic_bool done{};int found=0,code=0;
    std::thread ongoing([&] {
        int hx=-1,hy=-1;found=FindStr(0,0,640,180,"TEST","",.5,&hx,&hy);
        code=State().result;done=true;
    });
    // CPU 读回次数增加，说明请求已经取得该绑定的画面或正在取得；随后解绑。
    // 不通过修改生产代码添加测试暂停点，直接验证耗时识别不能发布旧绑定结果。
    while(!done) {
        FsCaijiZhuangtai now{};now.size=sizeof(now);GetCaptureStatus(&now);
        if(now.cpuReadbacks>before.cpuReadbacks) break;
        SwitchToThread();
    }
    const bool wasRunning=!done;
    const int unbound=UnBindWindow();ongoing.join();
    Require(SetFrameResultCache(0)==1,"restore result cache after unbind test");
    Require(unbound==1&&wasRunning&&found==-1&&code==FS_WEIBANGDING,"unbind discards in-flight model result");
}
void WorkbenchTest(const Fixture& fixture) {
    const HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    const HANDLE output=CreateFileW(L"CONOUT$",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    Require(input!=INVALID_HANDLE_VALUE&&output!=INVALID_HANDLE_VALUE,"OCR workbench console");
    auto check=[&](const std::wstring& command,const std::wstring& expected,bool detailed) {
        CONSOLE_SCREEN_BUFFER_INFO screen{};Require(GetConsoleScreenBufferInfo(output,&screen)!=0,"workbench cursor");
        std::vector<INPUT_RECORD> records;
        for(const auto c:command+L"\r") {
            INPUT_RECORD r{};r.EventType=KEY_EVENT;r.Event.KeyEvent.bKeyDown=TRUE;r.Event.KeyEvent.wRepeatCount=1;
            r.Event.KeyEvent.wVirtualKeyCode=c==L'\r'?VK_RETURN:0;r.Event.KeyEvent.uChar.UnicodeChar=c;records.push_back(r);
        }
        DWORD n{};Require(WriteConsoleInputW(input,records.data(),DWORD(records.size()),&n)&&n==records.size(),"send OCR function command");
        const auto end=GetTickCount64()+5000;bool found=false;
        do {
            std::wstring observed(8192,0);DWORD read{};
            Require(ReadConsoleOutputCharacterW(output,observed.data(),DWORD(observed.size()),screen.dwCursorPosition,&read)!=0,"read OCR command result");
            observed.resize(read);
            observed.erase(std::remove_if(observed.begin(),observed.end(),[](wchar_t c){return !c||iswspace(c);}),observed.end());
            auto wanted=expected;wanted.erase(std::remove_if(wanted.begin(),wanted.end(),[](wchar_t c){return iswspace(c)!=0;}),wanted.end());
            found=observed.find(wanted)!=observed.npos&&observed.find(detailed?L"OCR=dict":L"耗时:")!=observed.npos;
            if(found&&detailed) {
                Require(observed.find(L"首次模型准备/初始化:未执行")!=observed.npos&&
                    observed.find(L"文字检测:未执行")!=observed.npos&&observed.find(L"文字识别:")!=observed.npos&&
                    observed.find(L"结果整理/找字:")!=observed.npos,"OCR workbench timing stages");
            }
            if(found) break;Sleep(10);
        } while(GetTickCount64()<end);
        if(!found) std::fwprintf(stderr,L"Missing OCR workbench output for %ls\n",command.c_str());
        Require(found,"automatically generated OCR dispatcher");
    };
    std::vector<char> dictionaryBytes;
    {
        std::ifstream file(fixture.folder/L"ziku.txt",std::ios::binary);
        dictionaryBytes.assign(std::istreambuf_iterator<char>(file),{});
    }
    const auto address=std::to_wstring(reinterpret_cast<uintptr_t>(dictionaryBytes.data()));
    check(L"SetDictMemory(7, "+address+L", "+std::to_wstring(dictionaryBytes.size())+L")",
        L"返回值:1",false);
    check(L"SetOcrEngine(\"dict\")",L"返回值:1",false);
    check(L"SetFrameResultCache(0)",L"返回值:1",false);
    check(L"SetCaptureFrameInterval(-1)",L"返回值:1",false);
    check(L"OcrEx(0,0,640,180,\"000000\",1.0)",L"fullText=\"AA\"",true);
    check(L"OcrEx(0,0,640,180,\"000000\",1.0)",L"结果缓存=命中",true);
    check(L"OcrInFile(0,0,640,180,\"zixing.bmp\",\"000000\",1.0)",L"fullText=\"AA\"",true);
    check(L"FindStr(0,0,640,180,\"B|A\",\"000000\",1.0)",L"items[0]:id=1 x=20 y=30 text=\"A\"",true);
    check(L"FindStrS(0,0,640,180,\"A\",\"000000\",1.0)",L"items[0]:id=0 x=20 y=30 text=\"A\"",true);
    check(L"FindStrFast(0,0,640,180,\"B|A\",\"000000\",1.0)",L"items[0]:id=1 x=20 y=30 text=\"A\"",true);
    check(L"FindStrFastS(0,0,640,180,\"A\",\"000000\",1.0)",L"items[0]:id=0 x=20 y=30 text=\"A\"",true);
    check(L"FindStrFastEx(0,0,640,180,\"A\",\"000000\",1.0)",L"items[1]:id=0 x=50 y=30 text=\"A\"",true);
    check(L"FindStrFastExS(0,0,640,180,\"A\",\"000000\",1.0)",L"items[1]:id=0 x=50 y=30 text=\"A\"",true);
    check(L"ClearDict(7)",L"返回值:1",false);
    check(L"SetCaptureFrameInterval(0)",L"返回值:1",false);
    CloseHandle(input);CloseHandle(output);
}
void FastDictionaryTest(Fixture& f) {
    f.FastDictionary();Require(SetDict(4,"kuaisu.txt")&&GetDictCount(4)==1006,"large fast dictionary");
    Renderer renderer(f);renderer.Capture();int x=-1,y=-1;
    Require(FindStr(0,0,80,100,"张三","000000",1,&x,&y)==0&&x==20&&y==70,"ordinary search keeps intermediate glyph");
    const auto normalMs=State().recognitionMs;
    Require(FindStrFast(0,0,80,100,"张三","000000",1,&x,&y)==0&&x==20&&y==30,"fast search ignores intermediate glyph");
    std::printf("1006 glyphs, 80x100: ordinary matching %.3f ms, fast matching %.3f ms\n",normalMs,State().recognitionMs);
    Require(std::string(FindStrFastS(0,0,80,100,"不存在|张三","000000",1,&x,&y))=="张三"&&x==20&&y==30,"UTF-8 fast S");
    Require(std::string(FindStrFastEx(0,0,80,100,"不存在|张三","000000",1))=="1,20,30|1,20,70","UTF-8 fast Ex");
    Require(std::string(FindStrFastExS(0,0,80,100,"张三","000000",1))=="张三,20,30|张三,20,70","UTF-8 fast ExS");
    Require(FindStrFast(0,0,80,100,"三张","000000",1,&x,&y)==-1&&x==-1&&y==-1,"dictionary fast does not join rows");
    Require(FindStrFast(0,0,80,100,"张|三","000000",1,&x,&y)==0&&x==20&&y==30,"target list order");
    Require(FindStrFast(200,30,211,37,"张三","000000",1,&x,&y)==0&&x==200&&y==30,"compound dictionary glyph");
    Require(FindStrFast(110,110,140,140,"张","000000",1,&x,&y)==-1,"exact matching rejects damaged first column");
    Require(FindStrFast(110,110,140,140,"张","000000",.9,&x,&y)==0&&x==120&&y==120,"fuzzy first column tolerates missing pixel");
    Require(FindStr(110,110,140,140,"张","000000",.9,&x,&y)==0&&x==120&&y==120,"fuzzy agrees with ordinary matching");
    Require(FindStrFast(195,145,210,160,"矮","000000",1,&x,&y)==0&&x==200&&y==150,"short glyph masks lower rows");
    Require(FindStrFast(630,170,640,180,"矮","000000",1,&x,&y)==0&&x==636&&y==177,"right and bottom edge candidate");
    Require(FindStrFast(195,140,270,180,"高|矮","000000",1,&x,&y)==0&&x==250&&y==170,"different height target priority");
    Require(FindStrFast(195,140,270,180,"矮|高","000000",1,&x,&y)==0&&x==200&&y==150,"different height index rebuild");
    Require(FindStrFast(0,0,80,100,"张三","0.0.0-0.0.0",1,nullptr,nullptr)==0,"HSV and nullable output pointers");
    for(const char* bad:std::array<const char*,6>{nullptr,"","张|","|张","张||三","\xff"}) {
        Require(FindStrFast(0,0,80,100,bad,"000000",1,&x,&y)==-1&&x==-1&&y==-1&&State().result==FS_CANSHU_CUOWU,"invalid fast targets");
    }
    Require(FindStrFast(-1,0,80,100,"张","000000",1,&x,&y)==-1&&State().result==FS_CANSHU_CUOWU,"invalid fast region");
    Require(FindStrFast(0,0,80,100,"张","bad",1,&x,&y)==-1&&State().result==FS_CANSHU_CUOWU,"invalid fast color");
    Require(FindStrFast(0,0,80,100,"张","000000",.09,&x,&y)==-1&&State().result==FS_CANSHU_CUOWU,"invalid fast similarity");
    Require(FindStrFast(0,0,80,100,"不存在","000000",1,&x,&y)==-1&&State().result==FS_CHENGGONG,"absent glyph is successful no match");
    // 两处命中 × 751个相同目标应触发1500项上限，不能继续扩张返回串。
    std::string manyTargets="张三";for(int i=1;i<751;++i) manyTargets+="|张三";
    const std::string capped=FindStrFastEx(0,0,80,100,manyTargets.c_str(),"000000",1);
    Require(std::count(capped.begin(),capped.end(),'|')==1499&&capped.ends_with("749,20,70"),"fast all-result limit and ordering");
    std::array<bool,2> good{};
    auto concurrent=[&](int i) {
        for(int j=0;j<8;++j) {
            const std::string value=FindStrFastEx(0,0,80,100,"张三","000000",j%2?.9:1);
            if(value!="0,20,30|0,20,70") return;
        }
        good[i]=true;
    };
    std::thread a(concurrent,0),b(concurrent,1);a.join();b.join();
    Require(good[0]&&good[1],"concurrent fast matching keeps per-thread workspace independent");
    Require(UnBindWindow()&&ClearDict(4)&&State().sessionsCreated==0,"fast dictionary cleanup without neural initialization");
    Require(FindStrFast(0,0,80,100,"张","000000",1,&x,&y)==-1&&x==-1&&y==-1&&State().result==FS_WEIBANGDING,
        "unbind rejects new fast requests before dictionary access");
}
void DictionaryTest(Fixture& f,bool quiet) {
    f.Dictionary();
    Require(SetOcrEngine("dict")&&SetDict(3,"ziku.txt"),"load real encoded dictionary");
    Require(GetNowDict()==3&&GetDictCount(3)==1&&UseDict(3),"dictionary selection");
    std::vector<char> dictionaryBytes;
    {
        std::ifstream input(f.folder/L"ziku.txt",std::ios::binary);
        dictionaryBytes.assign(std::istreambuf_iterator<char>(input),{});
    }
    Require(!dictionaryBytes.empty()&&
        SetDictMemory(6,reinterpret_cast<uintptr_t>(dictionaryBytes.data()),
            static_cast<int>(dictionaryBytes.size()))==1&&
        GetNowDict()==6&&GetDictCount(6)==1,"load and select dictionary from memory");
    std::fill(dictionaryBytes.begin(),dictionaryBytes.end(),'\0');
    Require(std::string(OcrInFile(0,0,640,180,"zixing.bmp","000000",1))=="AA",
        "memory dictionary owns a copy after caller buffer changes");
    Require(!SetDictMemory(6,0,static_cast<int>(dictionaryBytes.size()))&&
        GetDictCount(6)==1&&UseDict(3),"invalid memory dictionary preserves loaded entry");
    Require(!SetDict(3,"cuowu.txt")&&GetDictCount(3)==1,"invalid dictionary preserves previous value");
    Require(!UseDict(100)&&!ClearDict(-1),"dictionary index validation");
    for(const int sourceHeight:{12,23,255}) {
        const auto filename="ziku"+std::to_string(sourceHeight)+".txt";
        Require(SetDict(5,filename.c_str())&&GetDictCount(5)==1,
            "load TSPlug dictionary with height metadata above 11");
        Require(std::string(OcrInFile(0,0,640,180,"zixing.bmp","000000",1))=="AA",
            "match tall-metadata dictionary using bounded 11-bit columns");
    }
    Require(UseDict(3)&&ClearDict(5)&&ClearDict(6),"restore original-height dictionary after file checks");
    for(const char* filter:{"","000000","b@ffffff","#0-0","0.0.0-0.0.0"}) {
        const std::string text=OcrInFile(0,0,640,180,"zixing.bmp",filter,1);
        std::printf("dict filter=%s -> %s\n",filter,text.c_str());Require(text=="AA","point-matrix and color filters");
    }
    Renderer renderer(f);renderer.Capture();
    // 大高度字段不能进入快速匹配的高度数组；同时覆盖精确、模糊和区域下边界。
    ResultCacheTest(renderer,"000000",1);
    for(const int sourceHeight:{12,23,255}) {
        const auto filename="ziku"+std::to_string(sourceHeight)+".txt";
        Require(SetDict(5,filename.c_str()),"select tall-metadata dictionary for window checks");
        for(const double sim:{1.0,.9}) {
            int tx=-1,ty=-1;
            Require(FindStr(20,30,25,41,"A","000000",sim,&tx,&ty)==0&&tx==20&&ty==30,
                "ordinary tall-metadata matching at bottom edge");
            Require(FindStrFast(20,30,25,41,"A","000000",sim,&tx,&ty)==0&&tx==20&&ty==30,
                "fast tall-metadata matching at bottom edge");
            Require(FindStrFast(20,30,25,40,"A","000000",sim,&tx,&ty)==-1&&tx==-1&&ty==-1,
                "fast tall-metadata matching rejects region shorter than 11 rows");
        }
    }
    Require(UseDict(3)&&ClearDict(5),"restore original-height dictionary after window checks");
    Require(std::string(Ocr(0,0,640,180,"000000",1))=="AA","window OCR");
    Require(std::string(OcrEx(0,0,640,180,"000000",1))=="A$20$30|A$50$30","per-character coordinates");
    Require(std::string(OcrExOne(0,0,640,180,"000000",1))=="AA|20,30|50,30","text with coordinate sequence");
    int x=0,y=0;
    Require(FindStr(0,0,640,180,"B|A","000000",1,&x,&y)==1&&x==20&&y==30,"FindStr target index");
    Require(std::string(FindStrS(0,0,640,180,"A","000000",1,&x,&y))=="A"&&x==20&&y==30,"FindStrS");
    Require(std::string(FindStrEx(0,0,640,180,"A","000000",1))=="0,20,30|0,50,30","FindStrEx all positions");
    Require(std::string(FindStrExS(0,0,640,180,"A","000000",1))=="A,20,30|A,50,30","FindStrExS all positions");
    Require(FindStr(0,0,640,180,"B","000000",1,&x,&y)==-1&&x==-1&&y==-1,"not found coordinates");
    if(!quiet) WorkbenchTest(f);
    Require(UnBindWindow()==1&&std::string(Ocr(0,0,640,180,"",1)).empty(),"unbind invalidates OCR frame");
    Require(ClearDict(3)&&GetDictCount(3)==0&&!UseDict(3),"clear dictionary");
    Require(State().sessionsCreated==0,"dictionary does not load neural runtime");
    FastDictionaryTest(f);
}
}
int main(int argc,char** argv) {
    try {
        // --quiet 供关闭工作台的构建验收使用；从无控制台进程启动时不允许 DLL 新建控制台。
        if(argc>2&&std::string(argv[2])=="--quiet") Require(GetConsoleWindow()==nullptr,"disabled workbenches did not create console");
        Fixture fixture;
        Require(!SetOcrBackend("cuda")&&!SetOcrEngine("bad"),"invalid engine/backend rejected");
        if(argc>1&&std::string(argv[1])=="--ppocrv3") {
            Require(argc>2,"PP-OCRv3 test requires a model directory argument");
            PpOcrV3Test(fixture,argv[2],argc>3&&std::string(argv[3])=="--directml");
        }
        else if(argc>1&&std::string(argv[1])=="--dict") DictionaryTest(fixture,argc>2&&std::string(argv[2])=="--quiet");
        else ModelTest(fixture,argc>1&&std::string(argv[1])=="--directml");
        UnBindWindow();FreeAllStringResults();std::puts("OCR checks passed");return 0;
    } catch(...) {UnBindWindow();return 1;}
}
