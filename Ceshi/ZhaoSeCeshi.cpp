// 使用真实 DX11 WARP 画面和公开 DLL 接口，验证找色、BMP 模板和坐标规则。
// 测试图案含多个命中、透明角点和不同颜色，避免只用纯色验证算法。
#include "../ZhaoTu/ZhaoTu.h"
#include "Xuanran.h"
#include "JieguoShipei.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace {
using Microsoft::WRL::ComPtr;
bool gpuTest{};
bool hardwareTest{};
const char* SearchMode() {return gpuTest?"gpu":"cpu";}
constexpr DWORD kTestStyle=WS_POPUP|WS_VISIBLE;
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); throw Failure{}; }
}
struct Api {
    HMODULE module{};
#define FS_FUNCTION(name) decltype(&::name) name{}
    FS_FUNCTION(BindWindow); FS_FUNCTION(UnBindWindow);
    FS_FUNCTION(SetFindBackend); fsdm::ceshi::Adapter<decltype(&::GetColor),fsdm::ceshi::Kind::color,false> GetColor; fsdm::ceshi::Adapter<decltype(&::FindColor),fsdm::ceshi::Kind::index,true> FindColor;
    fsdm::ceshi::Adapter<decltype(&::FindColorEx),fsdm::ceshi::Kind::indexEx,false> FindColorEx; fsdm::ceshi::Adapter<decltype(&::FindMultiColor),fsdm::ceshi::Kind::multi,true> FindMultiColor; fsdm::ceshi::Adapter<decltype(&::FindMultiColorEx),fsdm::ceshi::Kind::indexEx,false> FindMultiColorEx;
    fsdm::ceshi::Adapter<decltype(&::FindPic),fsdm::ceshi::Kind::index,true> FindPic; fsdm::ceshi::Adapter<decltype(&::FindPicS),fsdm::ceshi::Kind::pictureS,true> FindPicS; fsdm::ceshi::Adapter<decltype(&::FindPicEx),fsdm::ceshi::Kind::indexEx,false> FindPicEx; fsdm::ceshi::Adapter<decltype(&::FindPicExS),fsdm::ceshi::Kind::fileEx,false> FindPicExS;
    FS_FUNCTION(CvFindPic); FS_FUNCTION(CvFindPicEx); FS_FUNCTION(CvFindPicMem); FS_FUNCTION(CvFindPicMemEx);
    FS_FUNCTION(SetPath); FS_FUNCTION(Capture); FS_FUNCTION(CaptureJpg); FS_FUNCTION(CapturePng);
    FS_FUNCTION(GetStatus); FS_FUNCTION(GetCaptureStatus);
    FS_FUNCTION(GetFindStatus);
    FS_FUNCTION(SetTemplateCacheLimit); FS_FUNCTION(LoadPic); FS_FUNCTION(FreePic); FS_FUNCTION(LoadPicByte);
    fsdm::ceshi::Adapter<decltype(&::CmpColor),fsdm::ceshi::Kind::compare,false> CmpColor; FS_FUNCTION(GetScreenData); FS_FUNCTION(IsDisplayDead); FS_FUNCTION(MatchPicName);
    FS_FUNCTION(FreeResult); FS_FUNCTION(FreeAllStringResults);
#undef FS_FUNCTION
    Api() {
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        module = LoadLibraryW((std::filesystem::path(executable).parent_path()/L"ZhaoTu.dll").c_str());
        Require(module != nullptr, "load DLL");
#define FS_LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(module, #name)); Require(name != nullptr, #name)
        FS_LOAD(BindWindow); FS_LOAD(UnBindWindow); FS_LOAD(SetFindBackend);
        GetColor.Load(GetProcAddress(module,"GetColor"),GetProcAddress(module,"FreeResult")); FindColor.Load(GetProcAddress(module,"FindColor"),GetProcAddress(module,"FreeResult")); FindPic.Load(GetProcAddress(module,"FindPic"),GetProcAddress(module,"FreeResult")); FindPicS.Load(GetProcAddress(module,"FindPicS"),GetProcAddress(module,"FreeResult")); FindPicEx.Load(GetProcAddress(module,"FindPicEx"),GetProcAddress(module,"FreeResult"));
        FindColorEx.Load(GetProcAddress(module,"FindColorEx"),GetProcAddress(module,"FreeResult")); FindMultiColor.Load(GetProcAddress(module,"FindMultiColor"),GetProcAddress(module,"FreeResult")); FindMultiColorEx.Load(GetProcAddress(module,"FindMultiColorEx"),GetProcAddress(module,"FreeResult"));
        FindPicExS.Load(GetProcAddress(module,"FindPicExS"),GetProcAddress(module,"FreeResult"));
        FS_LOAD(CvFindPic); FS_LOAD(CvFindPicEx); FS_LOAD(CvFindPicMem); FS_LOAD(CvFindPicMemEx);
        FS_LOAD(SetPath); FS_LOAD(Capture); FS_LOAD(CaptureJpg); FS_LOAD(CapturePng);
        FS_LOAD(GetStatus); FS_LOAD(GetCaptureStatus);
        FS_LOAD(GetFindStatus);
        FS_LOAD(SetTemplateCacheLimit); FS_LOAD(LoadPic); FS_LOAD(FreePic); FS_LOAD(LoadPicByte);
        CmpColor.Load(GetProcAddress(module,"CmpColor"),GetProcAddress(module,"FreeResult")); FS_LOAD(GetScreenData); FS_LOAD(IsDisplayDead); FS_LOAD(MatchPicName);
    FS_LOAD(FreeResult); FS_LOAD(FreeAllStringResults);
#undef FS_LOAD
    }
    ~Api() {
        if (UnBindWindow) UnBindWindow();
        if (FreeAllStringResults) FreeAllStringResults();
    }
};
struct Renderer {
    Window window;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    DXGI_FORMAT format;
    int width, height;
    std::vector<uint32_t> pixels;
    Renderer(int w=64,int h=48,DXGI_FORMAT f=DXGI_FORMAT_R8G8B8A8_UNORM) :
        format(f),width(w),height(h),pixels(size_t(w)*h,0xff000000) {
        // 有标题栏窗口会被系统强制最小宽度；测试使用无边框窗口精确控制客户区。
        SetWindowLongPtrW(window.hwnd,GWL_STYLE,kTestStyle);
        RECT r{0,0,width,height};
        AdjustWindowRectEx(&r,kTestStyle,FALSE,0);
        Require(SetWindowPos(window.hwnd, nullptr, 0, 0, r.right-r.left, r.bottom-r.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED) != 0, "client size");
        auto desc = LegacyDesc(window.hwnd);
        desc.BufferDesc.Width=width; desc.BufferDesc.Height=height;
        desc.BufferDesc.Format=format;
        Check(D3D11CreateDeviceAndSwapChain(nullptr,hardwareTest?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&desc,&chain,&device,nullptr,&context),"DX11 test device");
    }
    // 内存按照 RGBA 写入交换链；公开 GetColor 应将读回 BGRA 转为 RRGGBB。
    void pixel(int x,int y,uint32_t rgb) {
        if(format==DXGI_FORMAT_R10G10B10A2_UNORM) {
            pixels[size_t(y)*width+x]=0xc0000000u|(((rgb>>16)*1023/255))|
                ((((rgb>>8)&255)*1023/255)<<10)|(((rgb&255)*1023/255)<<20);
        } else if(format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            pixels[size_t(y)*width+x]=0xff000000|rgb;
        } else pixels[size_t(y)*width+x] = 0xff000000 | ((rgb & 0xff)<<16) | (rgb & 0xff00) | (rgb>>16);
    }
    void present() {
        // 采集端为了不阻塞呈现，状态锁忙时允许跳过一次快照。测试不能把
        // “Present 返回”误当作“新快照已发布”，否则偶发地用上一张图验证新图案。
        auto statusFn=reinterpret_cast<FsStatusFn>(GetProcAddress(GetModuleHandleW(L"ZhaoTu.dll"),"GetStatus"));
        FsZhuangtai before{};before.size=sizeof(before);
        const bool bound=statusFn && statusFn(&before)==FS_CHENGGONG && before.bound &&
            before.hwnd==reinterpret_cast<uint64_t>(window.hwnd);
        ComPtr<ID3D11Texture2D> back, upload;
        Check(chain->GetBuffer(0,IID_PPV_ARGS(&back)),"back buffer");
        D3D11_TEXTURE2D_DESC desc{}; back->GetDesc(&desc);
        desc.BindFlags = 0; desc.MiscFlags = 0;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),static_cast<UINT>(width)*4,0};
        Check(device->CreateTexture2D(&desc,&data,&upload),"pattern texture");
        context->CopyResource(back.Get(),upload.Get());
        Check(chain->Present(0,0),"pattern present");
        if(bound) {
            const auto deadline=GetTickCount64()+2000;
            for(;;) {
                FsZhuangtai after{};after.size=sizeof(after);
                Require(statusFn(&after)==FS_CHENGGONG,"pattern capture status");
                if(after.hasFrame && after.frame.frameNumber>before.frame.frameNumber) break;
                Require(GetTickCount64()<deadline,"pattern snapshot published");
                Sleep(1);Check(chain->Present(0,0),"retry skipped pattern present");
            }
        }
    }
};
struct Folder {
    std::filesystem::path path;
    Folder() {
        wchar_t temp[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH,temp) != 0,"temp path");
        path = std::filesystem::path(temp)/(L"FsDmZhaoSe_"+std::to_wstring(GetCurrentProcessId()));
        Require(std::filesystem::create_directory(path),"test folder");
    }
    ~Folder() { std::error_code ec; std::filesystem::remove_all(path,ec); }
    std::string utf8() const {
        const auto value=path.u8string();
        return std::string(value.begin(),value.end());
    }
};
// 自行构造 BMP24，独立于截图编码器，包含 BMP 行补齐字节。
void Bitmap(const std::filesystem::path& path,int w,int h,const std::vector<uint32_t>& rgb) {
    const int stride=(w*3+3)&~3;
    BITMAPFILEHEADER header{}; header.bfType=0x4d42;
    header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+stride*h;
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=w; info.biHeight=h;
    info.biPlanes=1; info.biBitCount=24; info.biSizeImage=stride*h;
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(&header),sizeof(header));
    file.write(reinterpret_cast<const char*>(&info),sizeof(info));
    std::vector<unsigned char> row(stride);
    for(int y=h-1;y>=0;--y) {
        for(int x=0;x<w;++x) {
            const auto c=rgb[size_t(y)*w+x];
            row[x*3]=static_cast<unsigned char>(c);
            row[x*3+1]=static_cast<unsigned char>(c>>8);
            row[x*3+2]=static_cast<unsigned char>(c>>16);
        }
        file.write(reinterpret_cast<const char*>(row.data()),row.size());
    }
    Require(file.good(),"write BMP fixture");
}
void EncodedImage(const std::filesystem::path& path,int w,int h,const std::vector<uint32_t>& rgb,
    const std::vector<unsigned char>& alpha,REFGUID container) {
    Require(rgb.size()==size_t(w)*h && alpha.size()==rgb.size(),"encoded image fixture size");
    const HRESULT apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Require(SUCCEEDED(apartment) || apartment==RPC_E_CHANGED_MODE,"initialize WIC fixture apartment");
    struct ApartmentExit { HRESULT hr; ~ApartmentExit(){if(SUCCEEDED(hr)) CoUninitialize();} } cleanup{apartment};
    ComPtr<IWICImagingFactory> factory;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),
        "create WIC fixture factory");
    std::vector<unsigned char> bgra(rgb.size()*4);
    for(size_t i=0;i<rgb.size();++i) {
        bgra[i*4]=static_cast<unsigned char>(rgb[i]);
        bgra[i*4+1]=static_cast<unsigned char>(rgb[i]>>8);
        bgra[i*4+2]=static_cast<unsigned char>(rgb[i]>>16);
        bgra[i*4+3]=alpha[i];
    }
    ComPtr<IWICBitmap> bitmap;
    Require(SUCCEEDED(factory->CreateBitmapFromMemory(w,h,GUID_WICPixelFormat32bppBGRA,
        UINT(w)*4,UINT(bgra.size()),bgra.data(),&bitmap)),"create WIC fixture bitmap");
    ComPtr<IWICStream> stream;
    Require(SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE)),"create WIC fixture stream");
    ComPtr<IWICBitmapEncoder> encoder;
    Require(SUCCEEDED(factory->CreateEncoder(container,nullptr,&encoder)) &&
        SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache)),"create WIC fixture encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    Require(SUCCEEDED(encoder->CreateNewFrame(&frame,&options)) && SUCCEEDED(frame->Initialize(options.Get())) &&
        SUCCEEDED(frame->SetSize(w,h)),"initialize WIC fixture frame");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(IsEqualGUID(container,GUID_ContainerFormatJpeg)) format=GUID_WICPixelFormat24bppBGR;
    Require(SUCCEEDED(frame->SetPixelFormat(&format)),"set WIC fixture pixel format");
    ComPtr<IWICFormatConverter> converter;
    Require(SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(bitmap.Get(),format,WICBitmapDitherTypeNone,nullptr,0,
            WICBitmapPaletteTypeCustom)) && SUCCEEDED(frame->WriteSource(converter.Get(),nullptr)) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()),"write WIC fixture image");
}
std::vector<unsigned char> BitmapBytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Require(file.good(),"open BMP bytes");
    const auto size=file.tellg();
    Require(size>0 && size<INT_MAX,"BMP byte length");
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    Require(file.good(),"read BMP bytes");
    return bytes;
}
void CheckColors(Api& a,Renderer& renderer) {
    const int expected[][2]={{1,2},{1,40},{60,2},{60,40},{32,24},{1,2},{60,2},{1,40},{60,40}};
    for(int dir=0;dir<9;++dir) {
        int x=99,y=99;
        const int result=a.FindColor(0,0,64,48,"123456-000000",1.,dir,&x,&y);
        if(result!=0 || x!=expected[dir][0] || y!=expected[dir][1]) {
            FsZhuangtai s{}; s.size=sizeof(s); a.GetStatus(&s);
            std::fprintf(stderr,"dir=%d result=%d actual=%d,%d client=%u,%u render=%u,%u color=%s\n",
                dir,result,x,y,s.frame.clientWidth,s.frame.clientHeight,s.frame.width,s.frame.height,a.GetColor(1,2));
        }
        Require(result==0 &&
            x==expected[dir][0] && y==expected[dir][1],"all nine color directions");
    }
    int x{},y{};
    Require(std::string(a.GetColor(1,2))=="123456","lowercase RRGGBB and default CPU");
    Require(std::string(a.GetColor(-1,-1))=="000000" &&
        std::string(a.GetColor(64,0)).empty(),"color clamp and positive bounds");
    Require(a.FindColor(0,0,64,48,"ffffff-000000|123456-000000",1.,0,&x,&y)==1 &&
        x==1 && y==2,"multiple colors preserve index");
    Require(a.FindColor(1,2,2,3,"223456-100000",1.,0,&x,&y)==0,"per-channel hex tolerance");
    Require(a.FindColor(1,2,2,3,"223456-000000",.95,0,&x,&y)==-1 && x==-1 && y==-1,"similarity rejects");
    Require(a.FindColor(1,2,2,3,"223456-000000",.9,0,&x,&y)==-1,"sim .9 adds nine, not sixteen");
    Require(a.FindColor(0,0,65,49,"123456-000000",1.,0,&x,&y)==0 &&
        a.FindColor(-9,-9,1,2,"123456-000000",1.,0,&x,&y)==0 && x==1 && y==2,
        "color clipping and inclusive bottom-right");
    Require(a.FindColor(0,0,64,48,"123456-000000|000000-000000",1.,0,&x,&y)==0 &&
        x==1 && y==2,"color list precedes position");
    Require(a.FindColor(0,0,64,48,"12345x-000000",1.,0,&x,&y)==-1 &&
        a.FindColor(0,0,64,48,"123456-000000",std::numeric_limits<double>::quiet_NaN(),0,&x,&y)==-1 &&
        a.FindColor(0,0,64,48,"123456-000000",1.,9,&x,&y)==-1 &&
        a.FindColor(64,0,65,48,"123456-000000",1.,0,&x,&y)==-1 &&
        a.FindColor(1,2,1,3,"123456-000000",1.,0,&x,&y)==-1 &&
        a.FindColor(0,0,64,48,"123456-000000",1.,0,nullptr,&y)==-1,"invalid color inputs");
    // 独立枚举边界：离散表内外的 sim 都覆盖，分别改变 R/G/B，检查 <= 和偏色相加。
    renderer.pixel(8,8,0x808080); renderer.present();
    const std::pair<double,int> table[]={{.5,49},{.6,38},{.7,29},{.8,18},{.9,9},
        {1.,0},{.95,0},{.4,0},{.1,0}};
    for(const auto [sim,extra]:table) for(int shift: {0,8,16}) {
        for(int beyond=0;beyond<2;++beyond) {
            char color[14]; sprintf_s(color,"%06x-000000",0x808080+((extra+beyond)<<shift));
            Require(a.FindColor(8,8,9,9,color,sim,0,&x,&y)==(beyond ? -1:0),"discrete similarity RGB boundary");
        }
        char color[14],delta[7];
        sprintf_s(delta,"%06x",2<<shift);
        sprintf_s(color,"%06x-%s",0x808080+((extra+2)<<shift),delta);
        Require(a.FindColor(8,8,9,9,color,sim,0,&x,&y)==0,"explicit and similarity tolerances add");
    }
    // 上→右→下→左螺旋的前九个坐标直接写出，逐个移除命中，验证顺序而不复用实现。
    const int spiral[][2]={{5,5},{5,4},{6,4},{6,5},{6,6},{5,6},{4,6},{4,5},{4,4}};
    for(const auto& point:spiral) renderer.pixel(point[0],point[1],0x654321);
    for(const auto& point:spiral) {
        renderer.present();
        Require(a.FindColor(3,3,7,7,"654321-000000",1.,4,&x,&y)==0 &&
            x==point[0] && y==point[1],"spiral exact visit order");
        renderer.pixel(point[0],point[1],0);
    }
    // 中心螺旋须覆盖狭长区域的全部位置，特别是偶数尺寸和边缘。
    for(int py=0;py<4;++py) for(int px=0;px<9;++px) {
        renderer.pixel(px+10,py+30,0x980000+py*9+px);
    }
    renderer.present();
    for(int py=0;py<4;++py) for(int px=0;px<9;++px) {
        char color[14]; sprintf_s(color,"%06x-000000",0x980000+py*9+px);
        Require(a.FindColor(10,30,18,33,color,1.,4,&x,&y)==0 && x==px+10 && y==py+30,"spiral covers rectangle");
    }
    renderer.pixel(63,47,0x765432); renderer.present();
    for(int dir=0;dir<9;++dir)
        Require(a.FindColor(0,0,64,48,"765432-000000",1.,dir,&x,&y)==0 &&
            x==63 && y==47,"all directions include client last pixel");
}
// 独立多点图案：四个首点各带正/负偏移和反色点，预期坐标直接列出，
// 不借用生产扫描函数生成答案。结束时还原画面，避免影响原有找图用例。
void CheckExtendedColors(Api& a,Renderer& r) {
    const auto saved=r.pixels;
    std::fill(r.pixels.begin(),r.pixels.end(),0xff000000);
    const int origins[][2]={{10,10},{40,10},{10,30},{40,30}};
    for (const auto& p:origins) {
        r.pixel(p[0],p[1],0x123456);
        r.pixel(p[0]+2,p[1]+1,0x22cc44);
        r.pixel(p[0]-3,p[1]-2,0xaabbcc);
    }
    r.present();
    const char* offsets="2|1|22cc44,-3|-2|aabbcc-000000,1|-1|-ffffff";
    const char* expected[]={
        "0,10,10|0,40,10|0,10,30|0,40,30",
        "0,10,30|0,40,30|0,10,10|0,40,10",
        "0,40,10|0,10,10|0,40,30|0,10,30",
        "0,40,30|0,10,30|0,40,10|0,10,10"
    };
    const int first[][2]={{10,10},{10,30},{40,10},{40,30}};
    for (int dir=0;dir<4;++dir) {
        int x=9,y=9;
        Require(a.FindMultiColor(0,0,64,48,"123456-000000",offsets,1.,dir,&x,&y)==1 &&
            x==first[dir][0] && y==first[dir][1],"multi-color first result in four directions");
        Require(std::string(a.FindMultiColorEx(0,0,64,48,"123456",offsets,1.,dir))==expected[dir],
            "multi-color all results and source index format");
        Require(std::string(a.FindColorEx(0,0,64,48,"123456-000000",1.,dir))==expected[dir],
            "color Ex row directions");
    }
    const int columnDirs[]={5,6,7,8};
    const char* columns[]={
        "0,10,10|0,10,30|0,40,10|0,40,30",
        "0,40,10|0,40,30|0,10,10|0,10,30",
        "0,10,30|0,10,10|0,40,30|0,40,10",
        "0,40,30|0,40,10|0,10,30|0,10,10"
    };
    for (int i=0;i<4;++i)
        Require(std::string(a.FindColorEx(0,0,64,48,"123456",1.,columnDirs[i]))==columns[i],
            "color Ex column directions");
    Require(std::string(a.FindColorEx(0,0,64,48,"aabbcc-000000|123456-000000",1.,0))==
        "0,7,8|0,37,8|0,7,28|0,37,28|1,10,10|1,40,10|1,10,30|1,40,30",
        "color Ex color-list priority");
    Require(std::string(a.FindColorEx(9,9,11,11,"123456|123456",1.,0))=="0,10,10|1,10,10",
        "color Ex duplicate choices keep indexes");
    int x{},y{};
    Require(a.FindMultiColor(0,0,64,48,"123456", "0|0|123456,+2|+1|22cc44",1.,0,&x,&y)==1,
        "zero offset and signed decimal");
    Require(a.FindMultiColor(0,0,64,48,"123456","2|1|-22cc44",1.,0,&x,&y)==0 &&
        x==-1 && y==-1,"anti-color rejects matching RGB");
    Require(a.FindMultiColor(0,0,64,48,"123456","2|1|-23cc44-010000",1.,0,&x,&y)==0,
        "anti-color rejects inclusive tolerance boundary");
    Require(a.FindMultiColor(0,0,64,48,"123456","2|1|-24cc44-010000",1.,0,&x,&y)==1,
        "anti-color accepts outside RGB tolerance box");
    Require(a.FindMultiColor(0,0,64,48,"133456-010000","2|1|23cc44-010000",1.,0,&x,&y)==1,
        "multi explicit RGB tolerance");
    const std::pair<double,int> table[]={{.5,49},{.6,38},{.7,29},{.8,18},{.9,9},
        {1.,0},{.95,0},{.4,0},{.1,0}};
    // 三个通道分别检查 sim 容差边界，及反色在同一边界的互补结果。
    for (const auto [sim,extra]:table) for (int shift: {0,8,16}) for(int beyond=0;beyond<2;++beyond) {
        char firstColor[14],point[40],inverse[40];
        sprintf_s(firstColor,"%06x-000000",0x123456+((extra+beyond)<<shift));
        sprintf_s(point,"2|1|%06x-000000",0x22cc44+((extra+beyond)<<shift));
        sprintf_s(inverse,"2|1|-%06x-000000",0x22cc44+((extra+beyond)<<shift));
        Require(a.FindMultiColor(9,9,13,12,firstColor,"2|1|22cc44",sim,0,&x,&y)==!beyond,
            "multi first-color similarity boundary");
        Require(a.FindMultiColor(9,9,13,12,"123456",point,sim,0,&x,&y)==!beyond &&
            a.FindMultiColor(9,9,13,12,"123456",inverse,sim,0,&x,&y)==beyond,
            "multi offset and anti-color similarity boundary");
        Require(std::string(a.FindColorEx(9,9,11,11,firstColor,sim,0)).empty()==bool(beyond),
            "color Ex similarity boundary");
    }
    Require(a.FindMultiColor(10,10,11,11,"123456",offsets,1.,0,&x,&y)==0 &&
        std::string(a.FindMultiColorEx(10,10,11,11,"123456",offsets,1.,0)).empty(),
        "offsets must stay inside search region");
    // INT_MIN/MAX 必须在 64 位中运算；边缘候选和反色点也不能越界访问。
    for (const char* point: {"-2147483648|0|000000","2147483647|0|000000",
                            "0|-2147483648|-ffffff","0|2147483647|-ffffff"})
        Require(a.FindMultiColor(0,0,64,48,"123456",point,1.,0,&x,&y)==0,
            "large offsets cannot overflow");
    r.pixel(0,0,0x654321); r.pixel(63,47,0x654321); r.present();
    Require(a.FindMultiColor(0,0,64,48,"654321","-1|0|-ffffff",1.,0,&x,&y)==1 && x==63 && y==47 &&
        a.FindMultiColor(0,0,64,48,"654321","1|0|-ffffff",1.,3,&x,&y)==1 && x==0 && y==0,
        "negative and positive offsets skip invalid edge origins");
    Require(a.FindMultiColor(-9,-9,10,10,"123456","-3|-2|aabbcc",1.,0,&x,&y)==1 &&
        x==10 && y==10,"multi clipping and inclusive bottom-right");
    // 有效输入在首次访问当前帧时读回一次；随后找色接口共享该帧缓存。
    FsCaijiZhuangtai before{},after{}; before.size=sizeof(before); after.size=sizeof(after);
    a.GetCaptureStatus(&before);
    Require(std::string(a.FindColorEx(0,0,64,48,"123456",1.,0))==expected[0],"valid Ex before readback count");
    a.GetCaptureStatus(&after);
    Require(after.cpuReadbacks==before.cpuReadbacks,"color Ex reuses shared CPU frame");
    before=after;
    a.FindMultiColor(0,0,64,48,"123456",offsets,1.,0,&x,&y);
    a.GetCaptureStatus(&after);
    Require(after.cpuReadbacks==before.cpuReadbacks,"multi reuses shared CPU frame");
    before=after;
    a.FindMultiColorEx(0,0,64,48,"123456",offsets,1.,0);
    a.GetCaptureStatus(&after);
    Require(after.cpuReadbacks==before.cpuReadbacks,"multi Ex reuses shared CPU frame");
    before=after;
    for (const char* point: {static_cast<const char*>(nullptr),"","1|2","1|2|abcdef,","1||abcdef","1|2|--abcdef",
                            "1|2|abcdef|123456","1|2|abcdef-00000x","2147483648|0|ffffff",
                            "1.5|0|ffffff","1|2|abcdef,garbage"})
        Require(a.FindMultiColor(0,0,64,48,"123456",point,1.,0,&x,&y)==0 &&
            x==-1 && y==-1 &&
            std::string(a.FindMultiColorEx(0,0,64,48,"123456",point,1.,0)).empty(),
            "malformed offsets rejected atomically");
    for (const char* color: {static_cast<const char*>(nullptr),"","-123456","123456|ffffff","12345x","123456-00000x"})
        Require(a.FindMultiColor(0,0,64,48,color,offsets,1.,0,&x,&y)==0,"invalid first color rejected");
    Require(a.FindMultiColor(0,0,64,48,"123456",offsets,.05,0,&x,&y)==0 &&
        a.FindMultiColor(0,0,64,48,"123456",offsets,1.,4,&x,&y)==0 &&
        a.FindMultiColor(0,0,64,48,"123456",offsets,1.,0,nullptr,&y)==0 && y==-1 &&
        a.FindMultiColor(0,0,64,48,"123456",offsets,1.,0,&x,nullptr)==0 && x==-1 &&
        std::string(a.FindColorEx(0,0,64,48,"bad",1.,0)).empty() &&
        std::string(a.FindColorEx(0,0,64,48,"123456",1.,4)).empty() &&
        std::string(a.FindColorEx(0,0,64,48,"123456",NAN,0)).empty(),"invalid new API arguments");
    a.GetCaptureStatus(&after);
    Require(after.cpuReadbacks==before.cpuReadbacks,"invalid parameters do not read GPU pixels");
    // 客户区扩大两倍而渲染尺寸不变，偏移和返回值仍以客户区像素计算。
    SetWindowPos(r.window.hwnd,nullptr,0,0,128,96,SWP_NOMOVE|SWP_NOZORDER);
    r.present();
    Require(a.FindMultiColor(0,0,128,96,"123456","4|2|22cc44,-6|-4|aabbcc",1.,0,&x,&y)==1 &&
        x==20 && y==20,"multi offsets use scaled client coordinates");
    std::atomic_bool concurrentOk{true};
    auto read=[&](bool multi) {
        for(int n=0;n<5;++n) {
            const char* result=multi ?
                a.FindMultiColorEx(19,19,27,25,"123456","4|2|22cc44",1.,0) :
                a.FindColorEx(19,19,23,23,"123456",1.,0);
            std::this_thread::yield();
            if (std::string(result)!="0,20,20|0,21,20|0,20,21|0,21,21") concurrentOk=false;
        }
    };
    std::thread t1(read,true),t2(read,false); t1.join(); t2.join();
    Require(concurrentOk,"new string results and frame buffers isolated across threads");
    SetWindowPos(r.window.hwnd,nullptr,0,0,64,48,SWP_NOMOVE|SWP_NOZORDER);
    // 密集命中超过旧字符串预算，确认 CPU/GPU 都返回搜索区域内的全部坐标。
    std::fill(r.pixels.begin(),r.pixels.end(),0xff000000); r.present();
    std::string expectedDenseColorHits;
    for(int py=0;py<48;++py) for(int px=0;px<64;++px) {
        if(!expectedDenseColorHits.empty()) expectedDenseColorHits+='|';
        expectedDenseColorHits+="0,"+std::to_string(px)+","+std::to_string(py);
    }
    const std::string denseColors=a.FindColorEx(0,0,64,48,"000000",1.,0);
    Require(denseColors==expectedDenseColorHits && denseColors.size()>1024 &&
        std::count(denseColors.begin(),denseColors.end(),'|')==64*48-1,
        "color Ex returns every dense hit beyond the legacy string budget");
    const std::string dense=a.FindMultiColorEx(0,0,64,48,"000000","0|0|000000",1.,0);
    Require(std::count(dense.begin(),dense.end(),'|')==1499 &&
        dense.starts_with("0,0,0|0,1,0") && dense.ends_with("|0,27,23"),
        "multi Ex 1500 result cap and exact order");
    r.pixels=saved; r.present();
}
void CheckPictures(Api& a,Renderer& renderer,const Folder& folder) {
    constexpr uint32_t m=0xff00ff;
    const std::vector<uint32_t> cross={m,0xff0000,m,0x00ff00,0xabcdef,0x0000ff,m,0xffffff,m};
    Bitmap(folder.path/L"图案.bmp",3,3,cross);
    const std::vector<uint32_t> other={0x112233,0x445566,0x778899,0xaabbcc};
    Bitmap(folder.path/L"other.bmp",2,2,other);
    Bitmap(folder.path/L"empty.bmp",2,2,{0,0,0,0});
    Bitmap(folder.path/L"uniform.bmp",3,2,{0,0,0,0,0,0});
    EncodedImage(folder.path/L"mask.png",2,2,{0xf01010,0x11aa22,0x3344cc,0xeedd55},
        {0,255,255,255},GUID_ContainerFormatPng);
    EncodedImage(folder.path/L"photo.jpg",8,8,std::vector<uint32_t>(64,0x56789a),
        std::vector<unsigned char>(64,255),GUID_ContainerFormatJpeg);
    { std::ofstream broken(folder.path/L"broken.bmp",std::ios::binary); broken<<"not a BMP"; }
    auto stamp=[&](int x,int y) {
        for(int j=0;j<3;++j) for(int i=0;i<3;++i)
            if(cross[j*3+i]!=m) renderer.pixel(x+i,y+j,cross[j*3+i]);
    };
    stamp(3,4); stamp(20,12);
    for(int j=0;j<2;++j) for(int i=0;i<2;++i) renderer.pixel(40+i,30+j,other[j*2+i]);
    renderer.pixel(45,35,0x000000);
    renderer.pixel(46,35,0x11aa22);
    renderer.pixel(45,36,0x3344cc);
    renderer.pixel(46,36,0xeedd55);
    for(int j=0;j<8;++j) for(int i=0;i<8;++i) renderer.pixel(50+i,20+j,0x56789a);
    renderer.present();
    const char* pattern="\xe5\x9b\xbe\xe6\xa1\x88.bmp"; // UTF-8 的“图案.bmp”。
    const std::string list=std::string("other.bmp|")+pattern;
    // 验证 OpenCV 文件模板的透明角点、多结果和方向。
    auto checkCv=[&](const FsTongyongJieguo* result, unsigned count, int firstX, int firstY) {
        Require(result && result->status==FS_CHENGGONG && result->count==count,"OpenCV result count");
        if(count) Require(result->items[0].x==firstX && result->items[0].y==firstY,"OpenCV result coordinate");
        Require(a.FreeResult(result)!=0,"release OpenCV result");
    };
    checkCv(a.CvFindPic(0,0,64,48,pattern,1.,0),1,3,4);
    checkCv(a.CvFindPicEx(0,0,64,48,pattern,1.,0),2,3,4);
    checkCv(a.CvFindPicEx(0,0,64,48,pattern,1.,1),2,20,12);
    checkCv(a.CvFindPic(0,0,2,2,pattern,1.,0),0,0,0);
    const auto cvBytes=BitmapBytes(folder.path/L"图案.bmp");
    Require(a.LoadPicByte(reinterpret_cast<uintptr_t>(cvBytes.data()),int(cvBytes.size()),"cv_memory.bmp")==1,
        "register OpenCV memory template");
    checkCv(a.CvFindPicMem(0,0,64,48,"cv_memory.bmp",1.,0),1,3,4);
    checkCv(a.CvFindPicMemEx(0,0,64,48,"cv_memory.bmp",1.,0),2,3,4);
    Require(a.FreePic("cv_memory.bmp")==1,"release OpenCV memory template");
    int x{},y{};
    Require(a.FindPic(0,0,64,48,list.c_str(),"000000",1.,0,&x,&y)==0 &&
        x==40 && y==30,"template list precedes position");
    Require(a.LoadPic("mask.png")==1 &&
        a.FindPic(0,0,64,48,"mask.png","000000",1.,0,&x,&y)==0 && x==45 && y==35,
        "PNG decoding and fully transparent pixel masking");
    Require(a.FindPic(0,0,64,48,"photo.jpg","202020",1.,0,&x,&y)==0 && x==50 && y==20,
        "JPEG decoding to RGB template samples");
    Require(std::string(a.MatchPicName("*.png|*.jpg"))=="mask.png|photo.jpg",
        "enumerate PNG and JPEG template names");
    // 禁止重新打开文件读取，但允许查询文件属性；命中证明复用了解码结果。
    // 切换方向仍应从缓存像素准备样本，不能因为方向变化再次读取文件。
    {
        replica::Handle denyRead(CreateFileW((folder.path/L"other.bmp").c_str(),GENERIC_WRITE,
            FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
        Require(!!denyRead,"lock warmed template against reads");
        for(int dir=0;dir<4;++dir)
            Require(a.FindPic(0,0,64,48,"other.bmp","000000",1.,dir,&x,&y)==0 &&
                x==40 && y==30,"cached template and directions work without file read");
    }
    // 内容和大小不变时，时间戳变化也必须重新检查文件；覆盖成不同图案不能继续命中旧模板。
    const auto oldStamp=std::filesystem::last_write_time(folder.path/L"other.bmp");
    Bitmap(folder.path/L"other.bmp",2,2,{0x010203,0x040506,0x070809,0x0a0b0c});
    std::filesystem::last_write_time(folder.path/L"other.bmp",oldStamp+std::chrono::seconds(2));
    Require(a.FindPic(0,0,64,48,"other.bmp","000000",1.,0,&x,&y)==-1 &&
        x==-1 && y==-1,"same-size modified template invalidates cache");
    Bitmap(folder.path/L"other.bmp",2,2,other);
    Require(a.FindPic(0,0,64,48,"other.bmp","000000",1.,0,&x,&y)==0,
        "restored template reloads");
    for(int dir=0;dir<4;++dir) {
        const int ret=a.FindPic(0,0,64,48,pattern,"000000",1.,dir,&x,&y);
        Require(ret==0 && x==(dir==1||dir==3 ? 20:3) && y==(dir==1||dir==3 ? 12:4),"four picture directions");
    }
    Require(std::string(a.FindPicS(0,0,64,48,list.c_str(),"000000",1.,0,&x,&y))=="other.bmp,40,30",
        "FindPicS source type=2 filename and coordinates");
    Require(std::string(a.FindPicEx(0,0,64,48,list.c_str(),"000000",1.,0))=="0,40,30|1,3,4|1,20,12",
        "FindPicEx template-first results");
    Require(std::string(a.FindPicExS(0,0,64,48,pattern,"000000",1.,0))==
        std::string(pattern)+",3,4|"+pattern+",20,12","FindPicExS filenames");
    Require(a.Capture(3,4,6,7,"crop.bmp",2)==1 &&
        a.FindPic(0,0,64,48,"crop.bmp","000000",1.,0,&x,&y)==0 && x==3 && y==4,"saved BMP can be searched");
    Require(a.Capture(3,4,6,7,"capture-memory",1)==1 &&
        a.FindPic(0,0,64,48,"capture-memory","000000",1.,0,&x,&y)==0 && x==3 && y==4 &&
        !std::filesystem::exists(folder.path/L"capture-memory"),
        "memory-only capture registers searchable alias without a file");
    Require(a.CapturePng(3,4,6,7,"capture-both.png",3)==1 &&
        a.FindPic(0,0,64,48,"capture-both.png","000000",1.,0,&x,&y)==0 && x==3 && y==4 &&
        std::filesystem::exists(folder.path/L"capture-both.png"),
        "combined PNG capture writes file and registers original pixels");
    Require(std::filesystem::remove(folder.path/L"capture-both.png"),
        "remove combined capture fixture before enumeration tests");
    Require(a.Capture(3,4,6,7,"capture-invalid.bmp",0)==0 &&
        !std::filesystem::exists(folder.path/L"capture-invalid.bmp"),
        "capture rejects invalid mode");
    Require(a.FindPic(0,0,64,48,"empty.bmp","000000",1.,0,&x,&y)==0 &&
        a.FindPic(0,0,64,48,"uniform.bmp","000000",1.,0,&x,&y)==0,"uniform square and rectangle are opaque");
    Require(a.FindPic(0,0,64,48,"broken.bmp","000000",1.,0,&x,&y)==-1 &&
        a.FindPic(0,0,64,48,"missing.bmp","000000",1.,0,&x,&y)==-1 &&
        a.FindPic(0,0,64,48,pattern,"bad",1.,0,&x,&y)==-1 &&
        a.FindPic(3,4,5,6,pattern,"000000",1.,0,&x,&y)==-1,"invalid BMP, bias and small region");
    const std::string skip=std::string("missing.bmp|broken.bmp|")+pattern;
    Require(a.FindPic(0,0,64,48,skip.c_str(),"000000",1.,0,&x,&y)==2 && x==3 && y==4 &&
        a.FindPic(0,0,64,48,"other.bmp|missing.bmp","000000",1.,0,&x,&y)==0,
        "skip unloadable templates preserving input index");
    Require(a.FindPic(-10,-10,100,100,pattern,"000000",1.,0,&x,&y)==0 && x==3 && y==4,
        "picture clipping");
    for(int dir=0;dir<4;++dir) {
        const std::string expected=dir==1||dir==3 ? "0,20,12|0,3,4":"0,3,4|0,20,12";
        Require(std::string(a.FindPicEx(0,0,64,48,pattern,"000000",1.,dir))==expected,
            "Ex reverse directions preserve all origins and order");
    }
    renderer.pixel(4,5,0xabcdee); renderer.present();
    Require(a.FindPic(3,4,6,7,pattern,"000000",1.,0,&x,&y)==-1 &&
        a.FindPic(3,4,6,7,pattern,"000001",1.,0,&x,&y)==0 &&
        a.FindPic(3,4,6,7,pattern,"000000",.8,0,&x,&y)==0,"picture per-channel tolerance");
    renderer.pixel(4,5,0xabcd02); renderer.present();
    Require(a.FindPic(3,4,6,7,pattern,"000000",.8,0,&x,&y)==-1,
        "one mismatching effective pixel rejects whole template at sim .8");
    // 找图同样逐项验证离散表，固定区域只容纳一个模板位置。
    for(const auto [sim,extra]:std::array<std::pair<double,int>,9>{{
            {.5,49},{.6,38},{.7,29},{.8,18},{.9,9},{1.,0},{.95,0},{.4,0},{.1,0}}}) {
        renderer.pixel(4,5,0xabcdef-extra); renderer.present();
        Require(a.FindPic(3,4,6,7,pattern,"000000",sim,0,&x,&y)==0,"picture discrete similarity accepts");
        renderer.pixel(4,5,0xabcdef-extra-1); renderer.present();
        Require(a.FindPic(3,4,6,7,pattern,"000000",sim,0,&x,&y)==-1,"picture discrete similarity rejects");
        Require(a.FindPic(3,4,6,7,pattern,"000001",sim,0,&x,&y)==0,"picture tolerance is additive");
    }
    renderer.pixel(4,5,0xabcdef); renderer.present();
    // 非方形、不透明模板在同一行/列各有两个命中，区分四种方向并核对非零区域起点。
    const std::vector<uint32_t> rectangle={0x213141,0x516171,0x8191a1,0xb1c1d1,0x354555,0x657585};
    Bitmap(folder.path/L"rectangle.bmp",3,2,rectangle);
    for(int py: {35,43}) for(int px: {10,30})
        for(int j=0;j<2;++j) for(int i=0;i<3;++i) renderer.pixel(px+i,py+j,rectangle[j*3+i]);
    renderer.present();
    const char* ordered[]={
        "0,10,35|0,30,35|0,10,43|0,30,43",
        "0,10,43|0,30,43|0,10,35|0,30,35",
        "0,30,35|0,10,35|0,30,43|0,10,43",
        "0,30,43|0,10,43|0,30,35|0,10,35"
    };
    for(int dir=0;dir<4;++dir) {
        Require(a.FindPic(9,34,34,46,"rectangle.bmp","000000",1.,dir,&x,&y)==0 &&
            x==(dir>=2 ? 30:10) && y==(dir%2 ? 43:35),"non-square opaque directional first hit");
        Require(std::string(a.FindPicEx(9,34,34,46,"rectangle.bmp","000000",1.,dir))==ordered[dir],
            "non-square opaque directional complete ordering");
    }
    // 分辨率与客户区尺寸不同：模板仍按原图像素匹配，返回客户区坐标。
    RECT r{0,0,128,96}; AdjustWindowRectEx(&r,kTestStyle,FALSE,0);
    Require(SetWindowPos(renderer.window.hwnd,nullptr,0,0,r.right-r.left,r.bottom-r.top,
        SWP_NOMOVE|SWP_NOZORDER)!=0,"scale client");
    renderer.present();
    Require(std::string(a.GetColor(8,10))=="abcdef" &&
        a.Capture(6,8,12,14,"scaled.bmp",2)==1 &&
        a.FindPic(0,0,128,96,"scaled.bmp","000000",1.,0,&x,&y)==0 && x==6 && y==8,"scaled capture/template coordinates");
    // 相同的相对文件名在不同 SetPath 下是不同模板；文件删除后不能返回缓存旧结果。
    std::filesystem::create_directory(folder.path/L"another");
    Bitmap(folder.path/L"another"/L"other.bmp",2,2,{0x010203,0x040506,0x070809,0x0a0b0c});
    Require(a.SetPath((folder.utf8()+"/another").c_str())==1 &&
        a.FindPic(0,0,128,96,"other.bmp","000000",1.,0,&x,&y)==-1,"cache uses resolved SetPath");
    Require(a.SetPath(folder.utf8().c_str())==1 &&
        a.FindPic(0,0,128,96,"other.bmp","000000",1.,0,&x,&y)==0 && x==80 && y==60,
        "original SetPath keeps its own cached image");
    std::filesystem::remove(folder.path/L"other.bmp");
    Require(a.FindPic(0,0,128,96,"other.bmp","000000",1.,0,&x,&y)==-1,"deleted template cannot use old cache");
    Bitmap(folder.path/L"other.bmp",2,2,other);
    Require(a.FindPic(0,0,128,96,"other.bmp","000000",1.,0,&x,&y)==0,"recreated template reloads");
    // 缓存只按字节预算淘汰；140 张小模板均应驻留，不因项目使用量增长而被数量上限挤出。
    for(int i=0;i<140;++i) {
        const auto name="cache_"+std::to_string(i)+".bmp";
        Bitmap(folder.path/name,2,2,other);
        Require(a.FindPic(0,0,128,96,name.c_str(),"000000",1.,0,&x,&y)==0,"dynamic template cache fixture");
    }
    {
        replica::Handle denyRead(CreateFileW((folder.path/L"cache_0.bmp").c_str(),GENERIC_WRITE,
            FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
        Require(!!denyRead &&
            a.FindPic(0,0,128,96,"cache_0.bmp","000000",1.,0,&x,&y)==0 && x==80 && y==60,
            "oldest template remains cached after growing past 128 entries");
    }
    if(gpuTest) {
        FsChazhaoZhuangtai before{},after{};before.size=sizeof(before);after.size=sizeof(after);
        Require(a.GetFindStatus(&before)==FS_CHENGGONG,"read GPU cache counters");
        {
            replica::Handle denyRead(CreateFileW((folder.path/L"cache_0.bmp").c_str(),GENERIC_WRITE,
                FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
            Require(!!denyRead && a.FindPic(0,0,128,96,"cache_0.bmp","000000",1.,0,&x,&y)==0 &&
                a.GetFindStatus(&after)==FS_CHENGGONG && after.templateUploads==before.templateUploads,
                "GPU cache retains templates beyond 128 entries");
        }
    }
    Require(a.FindPic(0,0,128,96,"other.bmp","000000",1.,0,&x,&y)==0,
        "older templates remain usable after cache growth");
    // 字符串缓冲区属于各线程；并发查找及选择 CPU 不得覆盖另一线程的返回内容。
    std::atomic_bool ok{true};
    auto read=[&](int px,int py,const char* expected) {
        for(int n=0;n<10;++n) {
            const char* value=a.GetColor(px,py);
            a.SetFindBackend(SearchMode());
            std::this_thread::yield();
            if(std::string(value)!=expected) ok=false;
        }
    };
    std::thread first(read,8,10,"abcdef"),second(read,2,4,"123456");
    first.join(); second.join(); Require(ok,"thread-local strings and concurrent search");
    auto pictures=[&](int dir) {
        for(int n=0;n<8;++n) {
            int px{},py{};
            if(a.FindPic(0,0,128,96,"other.bmp","000000",1.,dir,&px,&py)!=0 ||
                px!=80 || py!=60) ok=false;
        }
    };
    std::thread forward(pictures,0),reverse(pictures,3);
    forward.join(); reverse.join();
    Require(ok,"concurrent immutable cached samples and per-thread CPU pixels");
}
// 对文件和内存预加载使用同一组 CPU / WARP / 硬件 GPU 测试，不从实现内部读取缓存状态。
void CheckPreload(Api& a,const Folder& folder) {
    Require(a.SetTemplateCacheLimit(256ull*1024*1024)==1 &&
        a.FreePic("*")==1,"restore default budget before explicit cache test");
    const std::vector<uint32_t> other={0x112233,0x445566,0x778899,0xaabbcc};
    Bitmap(folder.path/L"pre_a.bmp",2,2,other);
    Bitmap(folder.path/L"pre_b.bmp",2,2,other);
    int x{},y{};
    const auto found=[&](const char* name) {
        return a.FindPic(0,0,128,96,name,"000000",1.,0,&x,&y)==0 && x==80 && y==60;
    };
    Require(a.LoadPic("pre_?.bmp")==1 && a.LoadPic("pre_a.bmp|pre_b.bmp")==1,
        "preload wildcard, file list and repeated names");
    std::filesystem::remove(folder.path/L"pre_a.bmp");
    Require(a.LoadPic("pre_a.bmp")==1 && found("PRE_A.BMP"),
        "explicit file cache survives source deletion and path case changes");
    Bitmap(folder.path/L"pre_b.bmp",2,2,{1,2,3,4});
    Require(a.LoadPic("pre_b.bmp")==1 && found("pre_b.bmp"),
        "explicit cache retains old contents until FreePic");
    Require(a.FreePic("pre_b.bmp")==1 && !found("pre_b.bmp"),
        "FreePic discards explicit contents and next search loads changed file");
    Require(a.FindPic(0,0,128,96,"missing.bmp|pre_a.bmp","000000",1.,0,&x,&y)==1 &&
        x==80 && y==60,"preloaded template preserves original list index");
    Require(a.FreePic("pre_?.bmp")==1 && !found("pre_a.bmp"),
        "wildcard release matches deleted source names");
    Bitmap(folder.path/L"pre_b.bmp",2,2,other);
    auto bytes=BitmapBytes(folder.path/L"pre_b.bmp");
    const auto addr=[&] { return reinterpret_cast<uintptr_t>(bytes.data()); };
    const int size=static_cast<int>(bytes.size());
    Require(a.LoadPicByte(addr(),size,"mem_virtual.bmp")==1 && !std::filesystem::exists(folder.path/L"mem_virtual.bmp"),
        "register complete BMP bytes without a disk alias");
    std::fill(bytes.begin(),bytes.end(),static_cast<unsigned char>(0));
    Require(found("mem_virtual.bmp"),"loaded pixels are independent of caller bytes");
    std::vector<unsigned char>().swap(bytes);
    Require(std::string(a.FindPicS(0,0,128,96,"mem_virtual.bmp","000000",1.,0,&x,&y))=="mem_virtual.bmp,80,60" &&
        std::string(a.FindPicEx(0,0,128,96,"mem_virtual.bmp","000000",1.,0))=="0,80,60" &&
        std::string(a.FindPicExS(0,0,128,96,"mem_virtual.bmp","000000",1.,0))=="mem_virtual.bmp,80,60",
        "all four picture functions share byte-loaded template");
    bytes=BitmapBytes(folder.path/L"pre_b.bmp");
    Require(a.LoadPicByte(0,size,"bad.bmp")==0 && a.LoadPicByte(1,size,"bad.bmp")==0 &&
        a.LoadPicByte(UINTPTR_MAX-1,size,"bad.bmp")==0 &&
        a.LoadPicByte(addr(),-1,"bad.bmp")==0 && a.LoadPicByte(addr(),29,"bad.bmp")==0 &&
        a.LoadPicByte(addr(),size-1,"bad.bmp")==0 &&
        a.LoadPicByte(addr(),size,"a.bmp|b.bmp")==0 && a.LoadPicByte(addr(),size,"*.bmp")==0 &&
        a.LoadPicByte(addr(),size,nullptr)==0,"invalid address, length, truncated data and alias rejected");
    // 页尾数据不可读时也必须失败，而不是让宿主进程产生访问异常。
    SYSTEM_INFO info{};GetSystemInfo(&info);
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,size_t(info.dwPageSize)*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    Require(pages!=nullptr,"allocate protected BMP fixture");
    auto* partial=pages+info.dwPageSize-30;
    memcpy(partial,bytes.data(),30);
    DWORD previous{};
    Require(VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&previous)!=0,"protect BMP tail");
    const int unreadable=a.LoadPicByte(reinterpret_cast<uintptr_t>(partial),size,"bad.bmp");
    VirtualFree(pages,0,MEM_RELEASE);
    Require(unreadable==0,"unreadable memory tail returns failure safely");
    // 替换时 GPU 旧模板不得再次命中；失败的替换则应保留当前有效模板。
    Bitmap(folder.path/L"pre_b.bmp",2,2,{1,2,3,4});
    auto changed=BitmapBytes(folder.path/L"pre_b.bmp");
    FsChazhaoZhuangtai before{},after{};before.size=sizeof(before);after.size=sizeof(after);
    Require(a.GetFindStatus(&before)==FS_CHENGGONG &&
        a.LoadPicByte(reinterpret_cast<uintptr_t>(changed.data()),static_cast<int>(changed.size()),"mem_virtual.bmp")==1 &&
        !found("mem_virtual.bmp"),"replacement retires previous CPU and GPU template");
    Require(a.LoadPicByte(addr(),size,"mem_virtual.bmp")==1 && found("mem_virtual.bmp") &&
        a.LoadPicByte(addr(),size-1,"mem_virtual.bmp")==0 && found("mem_virtual.bmp") &&
        a.GetFindStatus(&after)==FS_CHENGGONG,"failed byte replacement preserves loaded template");
    if(gpuTest)
        Require(after.devicesCreated==before.devicesCreated && after.shaderBuilds==before.shaderBuilds &&
            after.templateUploads==before.templateUploads+2,"replacing template uploads once per version without rebuilding device");
    std::filesystem::create_directory(folder.path/L"another");
    Require(a.SetPath((folder.utf8()+"/another").c_str())==1 && !found("mem_virtual.bmp") &&
        a.SetPath(folder.utf8().c_str())==1 && found("mem_virtual.bmp"),"byte alias follows resolved SetPath namespace");
    // 查找线程已经拿到的样本可继续用，主线程释放/替换不能破坏像素或让 GPU 使用已释放资源。
    std::atomic_bool ok{true},started{false};
    std::thread reader([&] {
        started=true;
        for(int i=0;i<20;++i) {
            int px{},py{};
            const auto result=a.FindPic(0,0,128,96,"mem_virtual.bmp","000000",1.,0,&px,&py);
            if((result==0 && (px!=80 || py!=60)) || (result==-1 && (px!=-1 || py!=-1)) ||
                (result!=0 && result!=-1)) ok=false;
        }
    });
    while(!started.load()) SwitchToThread();
    for(int i=0;i<20;++i)
        if(!a.FreePic("mem_virtual.bmp") || !a.LoadPicByte(addr(),size,"mem_virtual.bmp")) ok=false;
    reader.join();
    Require(ok && found("mem_virtual.bmp"),"concurrent release, replace and search");
    Require(a.LoadPic(nullptr)==0 && a.LoadPic("")==0 && a.LoadPic("none_*.bmp")==0 &&
        a.LoadPic("broken.bmp")==0 && a.FreePic(nullptr)==0 && a.FreePic("")==0 &&
        a.FreePic("nonexistent.bmp")==1 && a.FreePic("mem_*.bmp")==1 && !found("mem_virtual.bmp"),
        "preload errors and idempotent release");
    // 数量不再设上限；140 个小模板均可显式驻留，预算不足时拒绝缩小且保留旧设置。
    Require(a.FreePic("*")==1 && a.SetTemplateCacheLimit(0)==0,"reject zero cache budget");
    for(int i=0;i<140;++i) {
        const auto name="pin_"+std::to_string(i)+".bmp";
        Bitmap(folder.path/name,2,2,other);
    }
    Require(a.LoadPic("pin_*.bmp")==1,"LoadPic explicitly loads more than 128 templates");
    Require(found("pin_0.bmp") && found("pin_139.bmp") &&
        a.SetTemplateCacheLimit(1024)==0,"pinned templates prevent unsafe budget reduction");
    Require(a.FreePic("pin_*.bmp")==1 && a.SetTemplateCacheLimit(1024)==1,
        "free pinned templates then reduce configured byte budget");
    int fitting=0;
    for(int i=0;i<140;++i) {
        const auto name="small_"+std::to_string(i)+".bmp";
        if(!a.LoadPicByte(addr(),size,name.c_str())) break;
        ++fitting;
    }
    Require(fitting>0 && fitting<140 && a.SetTemplateCacheLimit(2048)==1 &&
        a.LoadPicByte(addr(),size,"small_more.bmp")==1 &&
        a.SetTemplateCacheLimit(256ull*1024*1024)==1 && a.FreePic("small_*.bmp")==1 &&
        a.FreePic("small_more.bmp")==1,"configurable byte budget and capacity recovery");
    Require(a.LoadPicByte(addr(),size,"pin_anchor.bmp")==1,"retain one pinned template during automatic cache pressure");
    for(int i=0;i<140;++i) {
        const auto name="cache_"+std::to_string(i)+".bmp";
        Require(found(name.c_str()),"automatic templates work transiently when all slots pinned");
    }
    Require(found("pin_anchor.bmp") && a.FreePic("pin_*.bmp")==1,
        "automatic pressure preserves explicitly pinned templates");
    // 1024x1024 的已解码像素和比较样本共约 28 MiB：9 张可容纳，第 10 张超过 256 MiB。
    // 仅验证 CPU 预算，不做大图查找或预先分配 GPU 资源。
    Require(a.FreePic("*")==1,"clear for pinned byte limit");
    Bitmap(folder.path/L"budget.bmp",1024,1024,std::vector<uint32_t>(1024*1024,0x123456));
    const auto large=BitmapBytes(folder.path/L"budget.bmp");
    const auto largeAddr=reinterpret_cast<uintptr_t>(large.data());
    const int largeSize=static_cast<int>(large.size());
    for(int i=0;i<9;++i)
        Require(a.LoadPicByte(largeAddr,largeSize,("budget_"+std::to_string(i)+".bmp").c_str())==1,
            "explicit templates fit byte budget");
    Require(a.LoadPicByte(largeAddr,largeSize,"budget_9.bmp")==0 &&
        a.FreePic("budget_0.bmp")==1 && a.LoadPicByte(largeAddr,largeSize,"budget_9.bmp")==1 &&
        a.FreePic("budget_*.bmp")==1,"byte limit rejects overflow and FreePic returns capacity");
}
void CheckLimit(Api& a,Renderer& r,const Folder& folder) {
    RECT smallerRect{0,0,32,24}; AdjustWindowRectEx(&smallerRect,kTestStyle,FALSE,0);
    SetWindowPos(r.window.hwnd,nullptr,0,0,smallerRect.right-smallerRect.left,smallerRect.bottom-smallerRect.top,SWP_NOMOVE|SWP_NOZORDER);
    r.present();
    int hitX{},hitY{};
    Require(a.FindPic(0,0,32,24,"\xe5\x9b\xbe\xe6\xa1\x88.bmp","000000",1.,0,&hitX,&hitY)==0 &&
        hitX==1 && hitY==2,"render-space search does not miss odd source coordinate");
    // 四角不相同时无透明色：每个 2x2 棋盘格都可命中，检查精确的 1500 上限。
    RECT rect{0,0,64,48}; AdjustWindowRectEx(&rect,kTestStyle,FALSE,0);
    SetWindowPos(r.window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER);
    Bitmap(folder.path/L"checker.bmp",2,2,{0,0xffffff,0xffffff,0});
    for(int y=0;y<48;++y) for(int x=0;x<64;++x) r.pixel(x,y,(x+y)%2 ? 0xffffff:0);
    r.present();
    // 两份相同模板有不同索引，先列完第一份，再列第二份，合计上限 1500。
    const std::string hits=a.FindPicEx(0,0,64,48,"checker.bmp|checker.bmp","000000",1.,0);
    Require(1+std::count(hits.begin(),hits.end(),'|')==1500,"Ex result limit");
    Require(hits.starts_with("0,0,0|0,2,0") && hits.find("|1,0,0")!=std::string::npos,
        "overlapping matches and duplicate templates keep template-first order");
    // 模板恰好贴住右下边缘时也必须命中，修复旧源码漏扫边缘的问题。
    Bitmap(folder.path/L"edge.bmp",2,2,{0x102030,0x405060,0x708090,0xa0b0c0});
    r.pixel(62,46,0x102030); r.pixel(63,46,0x405060);
    r.pixel(62,47,0x708090); r.pixel(63,47,0xa0b0c0); r.present();
    for(int dir=0;dir<4;++dir)
        Require(a.FindPic(0,0,64,48,"edge.bmp","000000",1.,dir,&hitX,&hitY)==0 &&
            hitX==62 && hitY==46,"four picture directions include last fitting origin");
}
// 真正向控制台输入函数，然后读取输出，覆盖自动生成调用表，而非另写一套调用逻辑。
void CheckWorkbench(Api& a,Renderer& renderer,const Folder& folder) {
    const HANDLE input=CreateFileW(L"CONIN$",GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    const HANDLE output=CreateFileW(L"CONOUT$",GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    Require(input!=INVALID_HANDLE_VALUE && output!=INVALID_HANDLE_VALUE,"open workbench console");
    // 同时从另一线程调用找色和取色，验证工作台的分段不会被其他调用重置或覆盖。
    std::atomic_bool concurrentOk{true};
    std::jthread concurrent;
    struct Timing {
        const wchar_t* operation{};
        bool acquire{},prepare{},search{};
    };
    auto check=[&](const std::wstring& command,const std::wstring& expected,Timing timing=Timing{}) {
        CONSOLE_SCREEN_BUFFER_INFO screen{};
        Require(GetConsoleScreenBufferInfo(output,&screen)!=0,"console cursor");
        const COORD start=screen.dwCursorPosition;
        std::vector<INPUT_RECORD> records;
        for(wchar_t c:command+L"\r") {
            INPUT_RECORD record{}; record.EventType=KEY_EVENT;
            record.Event.KeyEvent.bKeyDown=TRUE; record.Event.KeyEvent.wRepeatCount=1;
            record.Event.KeyEvent.wVirtualKeyCode=c==L'\r' ? VK_RETURN:0;
            record.Event.KeyEvent.uChar.UnicodeChar=c;
            records.push_back(record);
        }
        DWORD written{};
        Require(WriteConsoleInputW(input,records.data(),static_cast<DWORD>(records.size()),&written) &&
            written==records.size(),"send search command");
        const auto deadline=GetTickCount64()+5000;
        bool found=false;
        std::wstring observed;
        do {
            // 只读本条命令之后的内容，防止上一条相同返回值造成假阳性。
            std::wstring text(8192,L'\0'); DWORD read{};
            Require(ReadConsoleOutputCharacterW(output,text.data(),static_cast<DWORD>(text.size()),start,&read)!=0,
                "read workbench output");
            text.resize(read);
            // 控制台按行存储，自动换行可能把字段分开；去掉填充空白后再检查。
            text.erase(std::remove_if(text.begin(),text.end(),
                [](wchar_t c) { return !c || iswspace(c); }),text.end());
            std::wstring wanted=expected;
            wanted.erase(std::remove_if(wanted.begin(),wanted.end(),
                [](wchar_t c) { return iswspace(c)!=0; }),wanted.end());
            if(text.find(wanted)!=std::wstring::npos) {
                // 等完整数值写出后再前进，防止第二行尚未输出就开始下一条命令。
                const size_t last=text.find(timing.operation ? L"，其他:":L"，耗时:");
                if(last!=std::wstring::npos && text.find(L"毫秒",last)!=std::wstring::npos) {
                    observed=std::move(text); found=true; break;
                }
                // 参数错误和不存在的导出函数不会进入调用阶段，因此没有调用耗时行。
                // 删除旧接口后，应检查明确的拒绝提示，不能继续等待不存在的耗时。
                if(!timing.operation && (expected==L"小数参数无效。" ||
                    expected==L"找不到该导出函数，请检查名称及大小写。")) {
                    observed=std::move(text); found=true; break;
                }
            }
            Sleep(20);
        } while(GetTickCount64()<deadline);
        if(!found) std::fwprintf(stderr,L"Command output missing: %ls\n",command.c_str());
        Require(found,"workbench result, coordinates and timing");
        if(!timing.operation) {
            Require(observed.find(L"采集取帧:")==std::wstring::npos &&
                observed.find(L"总耗时:")==std::wstring::npos,"non-search call has no stale breakdown");
            return;
        }
        const auto duration=[&](const std::wstring& label,bool executed) {
            const auto pos=observed.find(label);
            Require(pos!=std::wstring::npos,"timing phase label");
            const auto begin=pos+label.size();
            if(!executed) {
                Require(observed.compare(begin,3,L"未执行")==0,"skipped phase marked unexecuted");
                return 0.;
            }
            wchar_t* end{};
            const double value=std::wcstod(observed.c_str()+begin,&end);
            Require(end!=observed.c_str()+begin && std::isfinite(value) && value>=0. &&
                std::wstring_view(end).starts_with(L"毫秒"),"nonnegative measured duration");
            return value;
        };
        const double total=duration(L"总耗时:",true);
        const double acquire=duration(L"采集取帧:",timing.acquire);
        // GPU 工作台明确标注提交耗时；CPU 仍使用原来的操作名称。
        std::wstring searchLabel=timing.operation;
        if(gpuTest) searchLabel.insert(searchLabel.size()-1,L"提交");
        const double search=duration(searchLabel,timing.search);
        double prepare{};
        if(std::wstring_view(timing.operation)==L"找图:")
            prepare=duration(L"模板准备:",timing.prepare);
        else Require(observed.find(L"模板准备:")==std::wstring::npos,"color calls do not prepare templates");
        const double other=duration(L"其他:",true);
        const auto resPos=observed.find(L"GPU资源准备:");
        const bool resExecuted=gpuTest && resPos!=std::wstring::npos &&
            observed.compare(resPos+8,3,L"未执行")!=0;
        const double resources=gpuTest?duration(L"GPU资源准备:",resExecuted):0.;
        // 有效帧但无匹配工作时，异常清理仍须等 GPU 复制；这里只检查实际显示的字段。
        const std::wstring readLabel=L"GPU等待及结果读取:";
        const auto readPos=observed.find(readLabel);
        const bool readExecuted=gpuTest && readPos!=std::wstring::npos &&
            observed.compare(readPos+readLabel.size(),3,L"未执行")!=0;
        const double read=gpuTest?duration(readLabel,readExecuted):0.;
        const double sum=acquire+prepare+search+other+resources+read;
        // 各字段保留三位小数，允许分别舍入形成的小于 0.004ms 的总误差。
        Require(std::abs(total-sum)<.0061,"phase durations sum to same call total");
        // 使用 ASCII 诊断字段，避免 CTest 重定向时中文被 C 运行库替换成问号。
        std::fprintf(stderr,"TIMING total=%.3f, acquire=%.3f, prepare=%.3f, search=%.3f, other=%.3f ms; executed=%d/%d/%d\n",
            total,acquire,prepare,search,other,int(timing.acquire),int(timing.prepare),int(timing.search));
    };
    // 真实输入绑定函数，确认自动生成的调用表使用第二参数；失败切换不能改变当前帧。
    wchar_t binding[128]{};
    swprintf_s(binding,L"BindWindow(0x%llX, \"dx\", \"normal\", \"normal\", 0)",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(renderer.window.hwnd)));
    FsZhuangtai bound{}; bound.size=sizeof(bound);
    Require(a.GetStatus(&bound)==FS_CHENGGONG,"workbench binding before command");
    const auto bindingVersion=bound.bindingVersion;
    check(binding,L"返回值: 1，耗时:");
    swprintf_s(binding,L"BindWindow(0x%llX, \"obs\", \"normal\", \"normal\", 0)",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(renderer.window.hwnd)));
    check(binding,L"返回值: 0，耗时:");
    Require(a.GetStatus(&bound)==FS_CHENGGONG && bound.bound &&
        bound.bindingVersion==bindingVersion && !strcmp(bound.display,"dx") &&
        bound.result==FS_YIBANGDING,"workbench capture switch preserves binding");
    swprintf_s(binding,L"BindWindow(0x%llX, \"dx\", \"normal\", \"normal\", 0)",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(renderer.window.hwnd)));
    check(binding,L"返回值: 1，耗时:");
    // 预期失败已清除，再并发查找，避免测试线程把故意制造的绑定错误当成正常状态。
    concurrent=std::jthread([&](std::stop_token stop) {
        while(!stop.stop_requested()) {
            int x{},y{};
            if(a.FindColor(0,0,128,96,"123456-000000",.05,0,&x,&y)!=-1 ||
               std::string(a.GetColor(8,10))!="abcdef") concurrentOk=false;
            Sleep(1);
        }
    });
    check(L"LoadPic(\"other.bmp\")",L"返回值: 1，耗时:");
    check(L"FreePic(\"other.bmp\")",L"返回值: 1，耗时:");
    auto workbenchBytes=BitmapBytes(folder.path/L"other.bmp");
    wchar_t address[32]{};
    swprintf_s(address,L"0x%llX",static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(workbenchBytes.data())));
    check(std::wstring(L"LoadPicByte(")+address+L", "+std::to_wstring(workbenchBytes.size())+L", \"workbench_mem.bmp\")",
        L"返回值: 1，耗时:");
    std::fill(workbenchBytes.begin(),workbenchBytes.end(),static_cast<unsigned char>(0));
    check(L"FindPic(0, 0, 128, 96, \"workbench_mem.bmp\", \"000000\", 1.0, 0)",
        L"items[0]: id=0 x=80 y=60",{L"找图:",true,true,true});
    check(L"FreePic(\"workbench_*.bmp\")",L"返回值: 1，耗时:");
    check(L"LoadPicByte(0x100000000, 64, \"invalid.bmp\")",L"返回值: 0，耗时:");
    check(L"GetColor(8, 10)",L"items[0]: x=8 y=10 color=0xabcdef",{L"取色:",true,false,true});
    check(L"CmpColor(8, 10, \"abcdef-000000\", 1.0)",L"items[0]: id=0 x=8 y=10 color=0xabcdef",
        {L"比色:",true,false,true});
    // 其他三个新接口的分段名称不同，仍通过真实输入检查调用表及地址格式。
    auto extended=[&](const std::wstring& command,const std::wstring& expected,const std::wstring& phase) {
        CONSOLE_SCREEN_BUFFER_INFO screen{};
        Require(GetConsoleScreenBufferInfo(output,&screen)!=0,"extended console cursor");
        std::vector<INPUT_RECORD> records;
        for(auto c:command+L"\r") {
            INPUT_RECORD record{};record.EventType=KEY_EVENT;
            record.Event.KeyEvent.bKeyDown=TRUE;record.Event.KeyEvent.wRepeatCount=1;
            record.Event.KeyEvent.wVirtualKeyCode=c==L'\r'?VK_RETURN:0;
            record.Event.KeyEvent.uChar.UnicodeChar=c;records.push_back(record);
        }
        DWORD count{};
        Require(WriteConsoleInputW(input,records.data(),DWORD(records.size()),&count) &&
            count==records.size(),"extended console input");
        const auto deadline=GetTickCount64()+5000;
        do {
            std::wstring text(8192,0);
            Require(ReadConsoleOutputCharacterW(output,text.data(),DWORD(text.size()),screen.dwCursorPosition,&count)!=0,
                "extended console read");
            text.resize(count);
            text.erase(std::remove_if(text.begin(),text.end(),[](wchar_t c){return !c || iswspace(c);}),text.end());
            if(text.find(expected)!=text.npos && text.find(phase)!=text.npos && text.find(L"其他:")!=text.npos) return;
            Sleep(10);
        } while(GetTickCount64()<deadline);
        Require(false,"extended workbench output and timing");
    };
    extended(L"GetScreenData(8, 10, 9, 11)",L"返回值:0x",L"区域整理:");
    extended(L"IsDisplayDead(0, 0, 2, 2, 0)",L"返回值:1",L"检测等待:未执行");
    extended(L"MatchPicName(\"other.bmp\")",L"返回值:\"other.bmp\"",L"文件枚举:");
    check(L"FindColor(0, 0, 128, 96, \"123456-000000\", 1.0, 0)",
        L"items[0]: id=0 x=2 y=4",{L"找色:",true,false,true});
    check(L"FindColorEx(0, 0, 5, 7, \"123456-000000\", 1.0, 0)",
        L"items[3]: id=0 x=3 y=5",{L"找色:",true,false,true});
    check(L"FindMultiColor(0, 0, 5, 7, \"123456\", \"0|0|123456,-2|0|-ffffff\", 1.0, 0)",
        L"items[0]: id=0 x=2 y=4",{L"找色:",true,false,true});
    check(L"FindMultiColorEx(0, 0, 5, 7, \"123456\", \"0|0|123456,-2|0|-ffffff\", 1.0, 0)",
        L"items[3]: id=0 x=3 y=5",{L"找色:",true,false,true});
    check(L"FindMultiColor(0, 0, 5, 7, \"123456\", \"0|0|-123456\", 1.0, 0)",
        L"返回值: status=0，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,true});
    check(L"FindColorEx(0, 0, 5, 7, \"123456\", 1.0, 4)",
        L"返回值: status=4，detail=0x00000000，count=0，总耗时:",{L"找色:",false,false,false});
    check(L"FindMultiColorEx(0, 0, 5, 7, \"123456\", \"1|2|bad\", 1.0, 0)",
        L"返回值: status=4，detail=0x00000000，count=0，总耗时:",{L"找色:",false,false,false});
    check(L"FindPicS(0, 0, 128, 96, \"图案.bmp\", \"000000\", 0.9, 0)",
        L"items[0]: id=0 x=6 y=8 file=\"图案.bmp\"",{L"找图:",true,true,true});
    check(L"FindPic(0, 0, 128, 96, \"other.bmp\", \"000000\", 1.0, 0)",
        L"items[0]: id=0 x=80 y=60",{L"找图:",true,true,true});
    check(L"FindPicEx(0, 0, 128, 96, \"图案.bmp\", \"000000\", 1.0, 0)",
        L"items[1]: id=0 x=40 y=24 file=\"图案.bmp\"",{L"找图:",true,true,true});
    check(L"FindPicExS(0, 0, 128, 96, \"图案.bmp\", \"000000\", 1.0, 0)",
        L"items[1]: id=0 x=40 y=24 file=\"图案.bmp\"",{L"找图:",true,true,true});
    // 外部保留同名函数旧结果，工作台显示后只能按地址释放自己的结果。
    const auto* externalResult=a.GetColor.native(8,10);
    const auto* externalResult2=a.GetColor.native(2,4);
    check(L"GetColor(8, 10)",L"items[0]: x=8 y=10 color=0xabcdef",{L"取色:",true,false,true});
    Require(externalResult&&externalResult2&&externalResult!=externalResult2&&
        externalResult->count==1&&externalResult->items[0].color==0xabcdef&&
        externalResult2->count==1&&externalResult2->items[0].color==0x123456,
        "workbench cleanup preserves external same-function results");
    Require(a.FreeResult(externalResult)&&a.FreeResult(externalResult2),"exact native result release");
    check(L"FindColor(0, 0, 1, 1, \"ffffff-000000\", 1.0, 0)",
        L"返回值: status=0，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,true});
    check(L"GetColor(128, 96)",L"返回值: status=4，detail=0x00000000，count=0，总耗时:",{L"取色:",true,false,false});
    check(L"FindColor(0, 0, 128, 96, \"123456-000000\", 0.05, 0)",
        L"返回值: status=4，detail=0x00000000，count=0，总耗时:",{L"找色:",false,false,false});
    check(L"FindPic(0, 0, 128, 96, \"missing.bmp\", \"000000\", 1.0, 0)",
        L"返回值: status=9，detail=0x00000000，count=0，总耗时:",{L"找图:",true,true,false});
    check(L"FindPic(0, 0, 2, 2, \"other.bmp\", \"000000\", 1.0, 0)",
        L"返回值: status=0，detail=0x00000000，count=0，总耗时:",{L"找图:",true,true,false});
    check(L"FindPic(0, 0, 4, 4, \"other.bmp\", \"000000\", 1.0, 0)",
        L"返回值: status=0，detail=0x00000000，count=0，总耗时:",{L"找图:",true,true,true});
    check(L"FindPic(0, 0, 128, 96, \"broken.bmp|other.bmp\", \"000000\", 1.0, 0)",
        L"items[0]: id=1 x=80 y=60",{L"找图:",true,true,true});
    check(L"FindPic(0, 0, 128, 96, \"other.bmp\", \"bad\", 1.0, 0)",
        L"返回值: status=4，detail=0x00000000，count=0，总耗时:",{L"找图:",false,false,false});
    check(L"FindColor(0, 0, 128, 96, \"123456-000000\", nan, 0)",L"小数参数无效。");
    check(L"SetFindBackend(\"unknown\")",L"返回值: 0，耗时:");
    concurrent.request_stop(); concurrent.join();
    Require(concurrentOk,"other thread search results unchanged during workbench timing");
    check(L"FreeResult(0)",L"返回值: 1，耗时:");
    check(L"FreeFunctionResults(\"GetColor\")",L"找不到该导出函数，请检查名称及大小写。");
    check(L"FreeAllResults()",L"返回值: 1，耗时:");
    check(L"FreeStringResult(\"GetColor\")",L"找不到该导出函数，请检查名称及大小写。");
    check(L"FreeAllStringResults()",L"返回值: 1，耗时:");
    check(L"FreeAllStringResults()",L"返回值: 1，耗时:");
    // 读回本身失败时也应记录本次尝试；模板/匹配尚未开始，不能沿用前一次成功值。
    check(L"UnBindWindow()",L"返回值: 1，耗时:");
    check(L"FindColorEx(0, 0, 5, 7, \"123456\", 1.0, 0)",
        L"返回值: status=1，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,false});
    check(L"FindMultiColor(0, 0, 5, 7, \"123456\", \"0|0|123456\", 1.0, 0)",
        L"返回值: status=1，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,false});
    check(L"FindMultiColorEx(0, 0, 5, 7, \"123456\", \"0|0|123456\", 1.0, 0)",
        L"返回值: status=1，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,false});
    check(L"GetColor(8, 10)",L"返回值: status=1，detail=0x00000000，count=0，总耗时:",{L"取色:",true,false,false});
    check(L"FindPic(0, 0, 128, 96, \"other.bmp\", \"000000\", 1.0, 0)",
        L"返回值: status=1，detail=0x00000000，count=0，总耗时:",{L"找图:",true,false,false});
    Require(a.BindWindow(renderer.window.hwnd,"dx","normal","normal",0)==1,"rebind for timing no-first-frame");
    check(L"FindMultiColor(0, 0, 5, 7, \"123456\", \"0|0|123456\", 1.0, 0)",
        L"返回值: status=2，detail=0x00000000，count=0，总耗时:",{L"找色:",true,false,false});
    check(L"GetColor(8, 10)",L"返回值: status=2，detail=0x00000000，count=0，总耗时:",{L"取色:",true,false,false});
    renderer.present();
    check(L"GetColor(8, 10)",L"items[0]: x=8 y=10 color=0xabcdef",{L"取色:",true,false,true});
    CloseHandle(input); CloseHandle(output);
}
// 与用户案例相同尺寸的端到端基准：1172x590 画面、216x139 模板、命中 (463,137)。
// 图案独立生成，统计含真实 DLL 的读回、WIC 解码和查找，不能替代用户游戏实测。
void CheckLarge(Api& a,const Folder& folder) {
    Renderer r(1172,590);
    constexpr int w=216,h=139;
    std::vector<uint32_t> pattern(w*h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const uint32_t color=0x303030+uint32_t((x*37+y*101)%128)*0x010101;
        pattern[y*w+x]=color; r.pixel(463+x,137+y,color);
    }
    Bitmap(folder.path/L"large.bmp",w,h,pattern);
    Require(a.BindWindow(r.window.hwnd,"dx","normal","normal",0)==1,"bind full-size benchmark");
    r.present();
    const auto run=[&](const char* label,double sim,int expected) {
        int x=-1,y=-1;
        const auto start=std::chrono::steady_clock::now();
        const int result=a.FindPic(0,0,1172,590,"large.bmp","000000",sim,0,&x,&y);
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        FsCaijiZhuangtai status{}; status.size=sizeof(status);
        Require(a.GetCaptureStatus(&status)==FS_CHENGGONG,"benchmark readback status");
        FsChazhaoZhuangtai find{};find.size=sizeof(find);
        Require(a.GetFindStatus(&find)==FS_CHENGGONG,"benchmark find status");
        // GPU 模式没有完整像素读回，不能拿采集模块的旧 CPU 读回时间冒充本次统计。
        // 同时输出结果字节数，区分少量命中结果与整帧 BGRA 读取。
        std::fprintf(stderr,"BENCH %s %s: total=%.3f ms, cpuPixelReadback=%.3f ms, resultBytes=%llu, result=%d, xy=%d,%d\n",
            hardwareTest?"hardware":gpuTest?"WARP-GPU":"WARP-CPU",label,ms,
            gpuTest?0.:status.lastReadbackMs,
            static_cast<unsigned long long>(find.resultReadbackBytes),result,x,y);
        Require(result==expected && (expected<0 ? x==-1&&y==-1:x==463&&y==137),"full-size search result");
    };
    run("first sim=.9",.9,0);
    run("repeat sim=.9",.9,0);
    run("sim=.95",.95,0);
    // 只有一个有效像素超出容差，也必须拒绝该候选位置。
    r.pixel(463+w/2,137+h/2,0xff00ff); r.present();
    run("single pixel mismatch",.9,-1);
    Require(a.UnBindWindow()==1,"unbind benchmark");
}
// CPU 读回先创建处理设备，GPU 必须取得同一 serial；反复切换、解绑和尺寸变化
// 只能重建尺寸相关纹理，不能再次创建健康设备或再次编译着色器。
void CheckGpuReuse(Api& a,Renderer& r,const Folder& folder) {
    Require(a.SetPath(folder.utf8().c_str()) &&
        a.BindWindow(r.window.hwnd,"dx","normal","normal",0),"reuse bind");
    r.pixel(10,10,0x123456);r.present();
    Require(a.SetFindBackend("cpu") && std::string(a.GetColor(10,10))=="123456","CPU creates shared processor");
    auto acquire=reinterpret_cast<FsProcessorAcquireFn>(GetProcAddress(GetModuleHandleW(L"ZhuaTu.dll"),"ZhuaTuAcquireProcessor"));
    auto release=reinterpret_cast<FsProcessorReleaseFn>(GetProcAddress(GetModuleHandleW(L"ZhuaTu.dll"),"ZhuaTuReleaseProcessor"));
    auto frameFn=reinterpret_cast<FsGpuAcquireFn>(GetProcAddress(a.module,"AcquireGpuFrame"));
    auto freeFn=reinterpret_cast<FsGpuReleaseFn>(GetProcAddress(a.module,"ReleaseGpuFrame"));
    Require(acquire && release && frameFn && freeFn,"processor exports");
    FsGpuZhen frame{};frame.size=sizeof(frame);
    Require(frameFn(&frame)==FS_CHENGGONG,"get adapter");
    FsGpuChuli cpu{};cpu.size=sizeof(cpu);
    Require(SUCCEEDED(acquire(frame.adapterLuidLow,frame.adapterLuidHigh,&cpu)) &&
        SUCCEEDED(release(cpu.token)) && freeFn(frame.token)==FS_CHENGGONG,"get CPU processor identity");
    const auto state=[&] {
        FsChazhaoZhuangtai s{};s.size=sizeof(s);
        Require(a.GetFindStatus(&s)==FS_CHENGGONG,"find diagnostic");
        return s;
    };
    int x{},y{};
    const auto color=[&] {
        Require(a.FindColor(0,0,64,48,"123456-000000",1.,0,&x,&y)==0 && x==10 && y==10,"reuse color");
    };
    Require(a.SetFindBackend("gpu"),"select GPU reuse");
    color();
    const auto first=state();
    Require(first.deviceSerial==cpu.deviceSerial && first.devicesCreated==cpu.devicesCreated,
        "CPU and GPU use exactly the same processing device");
    for(int i=0;i<20;++i) color();
    auto repeated=state();
    Require(first.deviceSerial==repeated.deviceSerial && first.devicesCreated==repeated.devicesCreated &&
        first.shaderBuilds==repeated.shaderBuilds && first.resourceBuilds==repeated.resourceBuilds,
        "repeated color search reuses device, shaders and buffers");
    // 图片缓存以文件内容版本为边界。重复调用不上传，修改后恰好生成一份新模板。
    Bitmap(folder.path/L"reuse.bmp",2,2,{0x123456,0,0,0});
    const auto picture=[&] {
        Require(a.FindPic(0,0,64,48,"reuse.bmp","000000",1.,0,&x,&y)==0 &&
            x==10 && y==10,"reuse picture");
    };
    picture();const auto prepared=state();
    for(int i=0;i<10;++i) picture();
    repeated=state();
    Require(prepared.templateUploads==repeated.templateUploads && prepared.resourceBuilds==repeated.resourceBuilds,
        "GPU template upload cached");
    r.pixel(11,11,0x224466);r.present();
    Bitmap(folder.path/L"reuse.bmp",2,2,{0x123456,0,0,0x224466});
    std::filesystem::last_write_time(folder.path/L"reuse.bmp",
        std::filesystem::file_time_type::clock::now()+std::chrono::seconds(2));
    picture();
    Require(state().templateUploads==prepared.templateUploads+1,"changed template replaces GPU content");
    // 两种后端切换只更新选择，不重启采集。画面不再呈现时，仍可读取最后一张快照。
    for(int i=0;i<5;++i) {
        Require(a.SetFindBackend("cpu") && std::string(a.GetColor(10,10))=="123456","switch CPU");
        Require(a.SetFindBackend("gpu"),"switch GPU");color();
    }
    repeated=state();
    Require(repeated.deviceSerial==first.deviceSerial && repeated.devicesCreated==first.devicesCreated &&
        repeated.shaderBuilds==first.shaderBuilds,"backend switch does not recreate processor");
    // 故意占住共享上下文，让查找取得旧快照后等待。主线程先解绑重绑，
    // 再放开上下文；旧调用即使算出命中，也必须丢弃，不能返回新绑定的“结果”。
    FsGpuChuli held{};held.size=sizeof(held);
    Require(SUCCEEDED(acquire(frame.adapterLuidLow,frame.adapterLuidHigh,&held)),"hold processor");
    auto pending=std::async(std::launch::async,[&] {
        int px=99,py=99;
        const int result=a.FindColor(0,0,64,48,"123456-000000",1.,0,&px,&py);
        return std::array<int,3>{result,px,py};
    });
    bool leased=false;
    const auto deadline=GetTickCount64()+2000;
    do {
        FsCaijiZhuangtai capture{};capture.size=sizeof(capture);a.GetCaptureStatus(&capture);
        leased=capture.activeLeases!=0;
        if(leased) break;
        Sleep(1);
    } while(GetTickCount64()<deadline);
    const bool rebound=leased && a.UnBindWindow() &&
        a.BindWindow(r.window.hwnd,"dx","normal","normal",0);
    Require(SUCCEEDED(release(held.token)),"release blocked processor");
    const auto old=pending.get();
    Require(rebound && old==std::array<int,3>{-1,-1,-1},"old GPU binding result rejected");
    r.present();color();repeated=state();
    const auto version=repeated.bindingVersion;
    Require(a.UnBindWindow() && std::string(a.GetColor(10,10)).empty(),"GPU unbind rejects pixels");
    Require(state().result==FS_WEIBANGDING,"unbound GPU diagnostic");
    Require(a.BindWindow(r.window.hwnd,"dx","normal","normal",1),"GPU rebind");
    Require(std::string(a.GetColor(10,10)).empty() && state().result==FS_DENGDAISHOUZHEN,"GPU no first frame diagnostic");
    r.present();color();repeated=state();
    Require(repeated.bindingVersion!=version && repeated.deviceSerial==first.deviceSerial &&
        repeated.devicesCreated==first.devicesCreated && repeated.shaderBuilds==first.shaderBuilds,
        "rebind uses new snapshot and existing device");
    Check(r.chain->ResizeBuffers(1,83,57,DXGI_FORMAT_R8G8B8A8_UNORM,0),"GPU reuse resize");
    r.width=83;r.height=57;r.pixels.assign(83*57,0xff000000);
    r.pixel(10,10,0x123456);r.present();
    // 客户区未变，取色应按比例换算到新的原图像素。
    Require(std::string(a.GetColor(8,9))=="123456","GPU resize coordinate mapping");
    repeated=state();
    Require(repeated.deviceSerial==first.deviceSerial && repeated.devicesCreated==first.devicesCreated &&
        repeated.shaderBuilds==first.shaderBuilds,"resize rebuilds texture only");
    Require(a.UnBindWindow(),"reuse unbind");
    // 不同格式使用原始通道语义。CPU 作为公开接口对照，同时覆盖 SRGB 禁止伽马变换。
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,
                    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM}) {
        Renderer other(64,48,format);
        other.pixel(10,10,0x81a43f);
        Require(a.BindWindow(other.window.hwnd,"dx","normal","normal",0),"format bind");
        other.present();
        Require(a.SetFindBackend("cpu"),"format CPU");
        const std::string expected=a.GetColor(10,10);
        Require(expected.size()==6 && expected!="000000" && a.SetFindBackend("gpu") &&
            std::string(a.GetColor(10,10))==expected,"GPU raw format matches CPU truncation");
        const std::string choice=expected+"-000000";
        Require(a.FindColor(0,0,64,48,choice.c_str(),1.,0,&x,&y)==0 && x==10 && y==10,"format channel comparison");
        Require(a.UnBindWindow(),"format unbind");
    }
    std::puts("PASS: shared CPU/GPU device, stable shader/buffer/template cache, switch, pause, rebind, resize and format parity.");
}
}
// 通过真实渲染/导出接口验证扩展函数；冻结测试在工作线程读取，绑定仍归主线程。
void CheckTuSe(Api& a,const Folder& folder) {
    Require(a.UnBindWindow() && a.SetPath(folder.utf8().c_str()),"extended setup unbound");
    FsChazhaoZhuangtai find{};find.size=sizeof(find);
    Require(!a.GetScreenData(0,0,1,1) && !a.IsDisplayDead(0,0,1,1,0) &&
        a.CmpColor(0,0,"000000",1.)==1,"extended calls reject unbound");
    std::filesystem::create_directory(folder.path/L"match_sub");
    Bitmap(folder.path/L"match_002.bmp",2,2,{1,2,3,4});
    Bitmap(folder.path/L"match_001.BMP",2,2,{1,2,3,4});
    Bitmap(folder.path/L"match_sub"/L"图.bmp",2,2,{1,2,3,4});
    std::ofstream(folder.path/L"match_003.png").put('x');
    std::ofstream(folder.path/L"match_005.jpg").put('x');
    std::ofstream(folder.path/L"match_006.jpeg").put('x');
    std::ofstream(folder.path/L"match_004.bmpx").put('x');
    std::filesystem::create_directory(folder.path/L"match_dir.bmp");
    Require(std::string(a.MatchPicName("match_???.bmp|match_001.BMP|missing.bmp|*.png|*.jpg|*.jpeg"))==
        "match_001.BMP|match_002.bmp|mask.png|match_003.png|match_005.jpg|photo.jpg|match_006.jpeg",
        "supported image enumeration, ordering, duplicates, missing exact paths");
    Require(std::string(a.MatchPicName("match_sub/*.bmp"))=="match_sub/图.bmp" ||
        std::string(a.MatchPicName("match_sub/*.bmp"))=="match_sub\\图.bmp","relative Unicode subdirectory");
    const auto absolute=(folder.path/L"match_002.bmp").u8string();
    const std::string full(absolute.begin(),absolute.end());
    Require(std::string(a.MatchPicName(full.c_str()))==full,"absolute pattern preserved");
    Require(std::string(a.MatchPicName(nullptr)).empty() &&
        std::string(a.MatchPicName("bad*/*.bmp")).empty(),"invalid enumeration arguments");
    Renderer r;
    r.pixel(1,2,0x123456);r.pixel(2,2,0xabcdef);r.pixel(3,2,0x654321);
    Require(a.BindWindow(r.window.hwnd,"dx","normal","normal",0),"extended bind");
    Require(!a.GetScreenData(0,0,1,1) && !a.IsDisplayDead(0,0,1,1,0),"extended no first frame");
    r.present();
    for(const auto* mode:{"cpu","gpu"}) {
        Require(a.SetFindBackend(mode),"extended backend");
        FsCaijiZhuangtai before{},after{};before.size=sizeof(before);after.size=sizeof(after);
        a.GetCaptureStatus(&before);
        Require(a.CmpColor(1,2,"ffffff|123456-000000",1.)==0 &&
            a.CmpColor(1,2,"123457",1.)==1 &&
            a.CmpColor(1,2,"12345f",.9)==0 &&
            a.CmpColor(1,2,"123460",.9)==1 &&
            a.CmpColor(1,2,"223456-100000",1.)==0,"CmpColor result, multi-color, TS tolerance");
        a.GetCaptureStatus(&after);
        Require(after.cpuReadbacks-before.cpuReadbacks==(std::string(mode)=="cpu"?1u:0u),
            "GPU compare never reads full CPU frame");
        Require(a.CmpColor(64,0,"000000",1.)==1 && a.GetFindStatus(&find)==0 &&
            find.result==FS_CANSHU_CUOWU,"CmpColor bounds diagnostic");
        const std::string ten="ffffff|ffffff|ffffff|ffffff|ffffff|ffffff|ffffff|ffffff|ffffff|123456";
        Require(a.CmpColor(1,2,ten.c_str(),1.)==0 &&
            a.CmpColor(1,2,(ten+"|123456").c_str(),1.)==1 &&
            a.CmpColor(1,2,"123456|",1.)==1 &&
            a.CmpColor(1,2,"123456",NAN)==1 &&
            a.CmpColor(1,2,nullptr,1.)==1,"ten color limit and invalid input");
    }
    const auto pointer=a.GetScreenData(1,2,4,3);
    Require(pointer!=0,"GetScreenData nonzero address");
    const std::array<unsigned char,12> expected{0x56,0x34,0x12,0,0xef,0xcd,0xab,0,0x21,0x43,0x65,0};
    Require(std::equal(expected.begin(),expected.end(),reinterpret_cast<const unsigned char*>(pointer)),
        "GetScreenData tightly packed BGR0 odd width");
    // 其他接口和另一线程的原始数据调用不能破坏这个线程已取得的内存。
    Require(a.IsDisplayDead(0,0,4,4,0)==1,"zero seconds validates snapshot");
    bool threadOk{};
    std::thread reader([&] {
        const auto other=a.GetScreenData(2,2,3,3);
        threadOk=other && other!=pointer && *reinterpret_cast<const uint32_t*>(other)==0x00abcdef;
    });
    reader.join();
    Require(threadOk && std::equal(expected.begin(),expected.end(),reinterpret_cast<const unsigned char*>(pointer)),
        "screen pointer independent of other calls and threads");
    Require(!a.GetScreenData(0,0,65,48) && !a.GetScreenData(-1,0,2,2) &&
        !a.GetScreenData(2,2,2,3) && !a.IsDisplayDead(0,0,2,2,-1),"region and negative time checks");
    const auto start=GetTickCount64();
    Require(a.IsDisplayDead(0,0,4,4,1)==1 && GetTickCount64()-start>=990,"unchanged paused snapshot duration");
    // 等待首次实际读回，再由主线程更新/解绑，避免仅依赖线程调度先后。
    auto during=[&](auto action,int expectedResult) {
        FsCaijiZhuangtai before{};before.size=sizeof(before);a.GetCaptureStatus(&before);
        std::atomic_bool done{};int result=-1;
        std::jthread readerThread([&] {result=a.IsDisplayDead(0,0,4,4,2);done=true;});
        const auto deadline=GetTickCount64()+1500;
        for(;;) {
            FsCaijiZhuangtai now{};now.size=sizeof(now);a.GetCaptureStatus(&now);
            if(now.cpuReadbacks>before.cpuReadbacks || done.load()) break;
            Require(GetTickCount64()<deadline,"freeze first readback");Sleep(1);
        }
        Sleep(80);action();
        readerThread.join();Require(result==expectedResult,"freeze pixel/lifecycle result");
    };
    during([&] {for(int i=0;i<5;++i) {r.present();Sleep(30);}},1);
    during([&] {r.pixel(10,10,0xff00ff);r.present();},1);
    const auto changing=GetTickCount64();
    during([&] {r.pixel(1,2,0xffffff);r.present();},0);
    Require(GetTickCount64()-changing<1500,"changed region returns before timeout");
    during([&] {Require(a.UnBindWindow() && a.BindWindow(r.window.hwnd,"dx","normal","normal",0),"rebind during freeze");r.present();},0);
    during([&] {Require(SetWindowPos(r.window.hwnd,nullptr,0,0,128,96,SWP_NOMOVE|SWP_NOZORDER)!=0,"resize extended client");r.present();},0);
    const auto scaled=a.GetScreenData(2,4,4,6);
    Require(scaled && *reinterpret_cast<const uint32_t*>(scaled)==0x00ffffff,
        "client to render nearest mapping");
    during([&] {DestroyWindow(r.window.hwnd);},0);
    Require(!a.GetScreenData(0,0,1,1),"destroy invalidates raw pixel access");
    Require(a.UnBindWindow() && a.SetFindBackend(SearchMode()),"extended cleanup");
}
int wmain(int argc,wchar_t** argv) {
    try {
        hardwareTest=argc>1 && std::wstring_view(argv[1])==L"--gpu-hardware";
        gpuTest=hardwareTest || (argc>1 && std::wstring_view(argv[1])==L"--gpu");
        SetProcessDPIAware();
        Renderer renderer;
        Folder folder;
        Api a;
        if(argc>1 && std::wstring_view(argv[1])==L"--gpu-reuse") {
            CheckGpuReuse(a,renderer,folder);return 0;
        }
        int x=4,y=4;
        Require(std::string(a.GetColor(0,0)).empty() &&
            a.FindColor(0,0,64,48,"000000-000000",1.,0,&x,&y)==-1 && x==-1 && y==-1,"unbound search fails");
        Require(a.SetFindBackend(SearchMode())==1 && a.SetFindBackend(nullptr)==0 &&
            a.SetFindBackend("unknown")==0,"backend selection and invalid names");
        Require(a.SetPath(folder.utf8().c_str())==1,"set path before preload");
        Bitmap(folder.path/L"unbound_preload.bmp",2,2,{1,2,3,4});
        auto unboundBytes=BitmapBytes(folder.path/L"unbound_preload.bmp");
        Require(a.LoadPic("unbound_preload.bmp")==1 &&
            a.LoadPicByte(reinterpret_cast<uintptr_t>(unboundBytes.data()),static_cast<int>(unboundBytes.size()),"unbound_mem.bmp")==1 &&
            a.FreePic("unbound_*.bmp")==1,"file and byte preload need no binding or capture device");
        FsCaijiZhuangtai unloaded{};unloaded.size=sizeof(unloaded);
        Require(a.GetCaptureStatus(&unloaded)==FS_CHENGGONG && unloaded.gpuSlots==0 &&
            unloaded.cpuReadbacks==0,"preload never starts capture or pixel readback");
        Require(a.SetPath(folder.utf8().c_str())==1 &&
            a.BindWindow(renderer.window.hwnd,"dx","normal","normal",0)==1,"setup capture");
        Require(std::string(a.GetColor(0,0)).empty(),"no first frame");
        for(const auto& p:std::array<std::array<int,2>,5>{{{1,2},{1,40},{60,2},{60,40},{32,24}}})
            renderer.pixel(p[0],p[1],0x123456);
        renderer.present();
        FsCaijiZhuangtai before{},after{}; before.size=sizeof(before); after.size=sizeof(after);
        Require(a.GetCaptureStatus(&before)==FS_CHENGGONG && before.cpuReadbacks==0,"GPU-only before search");
        Require(a.SetFindBackend(SearchMode())==1 &&
            a.GetCaptureStatus(&after)==FS_CHENGGONG && after.cpuReadbacks==0,"selecting find backend never reads pixels");
        Require(std::string(a.GetColor(1,2))=="123456" &&
            a.GetCaptureStatus(&after)==FS_CHENGGONG && after.cpuReadbacks==uint64_t(!gpuTest),"GetColor readback policy");
        CheckColors(a,renderer);
        CheckExtendedColors(a,renderer);
        CheckPictures(a,renderer,folder);
        if(argc>1 && std::wstring_view(argv[1])==L"--opencv") {
            std::puts("PASS: OpenCV file and memory picture search.");
            return 0;
        }
        Require(a.SetFindBackend("unknown")==0 &&
            std::string(a.GetColor(8,10))=="abcdef","failed switch preserves selected backend");
        CheckWorkbench(a,renderer,folder);
        CheckPreload(a,folder);
        CheckLimit(a,renderer,folder);
        Require(a.UnBindWindow()==1 && std::string(a.GetColor(0,0)).empty() &&
            std::string(a.FindPicS(0,0,64,48,"other.bmp","000000",1.,0,&x,&y)).empty() &&
            x==-1 && y==-1,"unbind invalidates search");
        CheckLarge(a,folder);
        CheckTuSe(a,folder);
        std::puts("PASS: TSPlug channel tolerance, multi-color/anti-color, Ex limits, nine functions, safe boundaries and full-size search.");
        return 0;
    } catch(...) { return 1; }
}
