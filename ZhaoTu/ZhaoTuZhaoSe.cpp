// CPU/GPU 找图找色：对照 TSPlug4.19 的 TSFindPicture.cpp 实现通道容差与方向遍历。
// GetColor 对应 DXBind::GetColor/processColor；FindColor 对应 processColor。
// 四个 FindPic 接口对应 findPicture 的普通、type=2、type=0、type=1 分支。
// 采集后端与查找后端分离，每次操作固定一张快照；GPU 分支不读取完整 BGRA。
#include "ZhaoTu.h"
#include "Haoshi.h"
#include "ZifuchuanNeicun.h"
#include "TongyongJieguo.h"
#include "XianKaChazhao.h"
#include "ZhenhuanCun.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

std::filesystem::path FsResolvePath(const char*);
using Microsoft::WRL::ComPtr;
namespace {
thread_local std::string answer;
thread_local bool useGpu{};
using fsdm::chazhao::Color;
using fsdm::chazhao::TemplateData;
using fsdm::chazhao::Point;
// 最近一次查找所用帧仍按线程记录，像素本身由跨 API 的共享缓存持有。
thread_local FsZhen searchedFrame{};
bool sameFrame(const FsZhen& a,const FsZhen& b) {
    return a.bindingVersion && a.bindingVersion==b.bindingVersion && a.frameNumber==b.frameNumber &&
        a.width==b.width && a.height==b.height && a.stride==b.stride &&
        a.clientWidth==b.clientWidth && a.clientHeight==b.clientHeight;
}
struct Frame {
    FsZhen info{};
    std::unique_ptr<fsdm::gpu::Session> gpu;
    std::shared_ptr<const std::vector<unsigned char>> bytes;
    bool read() {
        // GPU 查找继续直接使用共享纹理；CPU 路径与 OCR、YOLO 共用 BGRA 快照缓存。
        fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::acquire);
        if (useGpu) {
            gpu=std::make_unique<fsdm::gpu::Session>();
            if(!gpu->Read(info)) return false;
            searchedFrame=info;
            return true;
        }
        fsdm::zhenhuancun::Snapshot snapshot;
        const int rc=fsdm::zhenhuancun::Read(snapshot);
        if(rc!=FS_CHENGGONG) {fsdm::gpu::Fail(rc);return false;}
        info=snapshot.info;bytes=std::move(snapshot.pixels);
        fsdm::gpu::CpuFrame(info);searchedFrame=info;
        return true;
    }
    bool region(int& x1, int& y1, int& x2, int& y2, bool color) const {
        // TSPlug 的入口拒绝反向/空矩形和左上角超过客户区的请求；
        // 负的左上角归零，超过客户区的右下角截断。找色右下角包含，找图不包含。
        if (x2 <= x1 || y2 <= y1 || x1 >= int(info.clientWidth) || y1 >= int(info.clientHeight))
            { fsdm::gpu::Fail(FS_CANSHU_CUOWU); return false; }
        x1 = (std::max)(0, x1); y1 = (std::max)(0, y1);
        x2 = (std::min)(x2, int(info.clientWidth) - int(color));
        y2 = (std::min)(y2, int(info.clientHeight) - int(color));
        // 内部统一用不包含右下角的区间，找色只在这里加一，避免循环越界。
        if (color) { ++x2; ++y2; }
        if(x2<=x1 || y2<=y1) {fsdm::gpu::Fail(FS_CANSHU_CUOWU);return false;}
        return true;
    }
    const unsigned char* pixel(int x, int y) const {
        const uint64_t px = uint64_t(x) * info.width / info.clientWidth;
        const uint64_t py = uint64_t(y) * info.height / info.clientHeight;
        return renderPixel(static_cast<int>(px), static_cast<int>(py));
    }
    const unsigned char* renderPixel(int x, int y) const {
        return bytes->data() + size_t(y) * info.stride + size_t(x) * 4;
    }
};
int digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool hex(const std::string& s, Color& c) {
    if (s.size() != 6) return false;
    int v[6]{};
    for (int i = 0; i < 6; ++i) if ((v[i] = digit(s[i])) < 0) return false;
    c = {v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]};
    return true;
}
std::vector<std::string> split(const char* text) {
    std::vector<std::string> parts;
    if (!text) return parts;
    std::string s(text);
    size_t pos = 0;
    for (;;) {
        const size_t next = s.find('|', pos);
        parts.push_back(s.substr(pos, next == std::string::npos ? next : next - pos));
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return parts;
}
bool validSim(double sim) { return std::isfinite(sim) && sim >= 0.1 && sim <= 1.0; }
// 严格对应 findPicture::m_simColor / processColor::help_simi 的离散表。
// sim 不是“允许多少比例的像素错误”。未列出的值（含 0.95、0.4）额外容差为零，
// 不能插值或改用 255*(1-sim)，否则会改变参考源码的参数含义。
int extraTolerance(double sim) {
    if (sim == 0.5) return 49;
    if (sim == 0.6) return 38;
    if (sim == 0.7) return 29;
    if (sim == 0.8) return 18;
    if (sim == 0.9) return 9;
    return 0;
}
bool equal(const unsigned char* p, const Color& c, const Color& tolerance) {
    return std::abs(int(p[2]) - c.r) <= tolerance.r &&
        std::abs(int(p[1]) - c.g) <= tolerance.g && std::abs(int(p[0]) - c.b) <= tolerance.b;
}

struct ColorChoice { Color color, delta; };
// 新接口允许省略偏色（等价于 000000），但不接受多余分隔符或不完整十六进制。
// 容差叠加沿用 TSPlug 的离散 sim 表；反色也使用叠加后的同一范围。
bool parseChoice(std::string_view text, int extra, ColorChoice& choice) {
    if ((text.size()!=6 && text.size()!=13) || (text.size()==13 && text[6]!='-') ||
        !hex(std::string(text.substr(0,6)),choice.color)) return false;
    choice.delta={};
    if (text.size()==13 && !hex(std::string(text.substr(7)),choice.delta)) return false;
    choice.delta.r=(std::min)(255,choice.delta.r+extra);
    choice.delta.g=(std::min)(255,choice.delta.g+extra);
    choice.delta.b=(std::min)(255,choice.delta.b+extra);
    return true;
}
struct OffsetPoint {
    int x{}, y{};
    ColorChoice choice;
    bool inverted{};
};
bool decimal(std::string_view text, int& value) {
    if (text.empty()) return false;
    // from_chars 不接受前导加号；单独处理，仍禁止空值、空白、十六进制和溢出。
    if (text.front()=='+') {
        text.remove_prefix(1);
        if (text.empty() || text.front()=='-') return false;
    }
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value,10);
    return result.ec==std::errc{} && result.ptr==text.data()+text.size();
}
bool parseOffsets(const char* text, int extra, std::vector<OffsetPoint>& points) {
    if (!text || !*text) return false;
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const auto end=remaining.find(',');
        const auto item=remaining.substr(0,end);
        const auto first=item.find('|');
        const auto second=first==item.npos ? item.npos : item.find('|',first+1);
        if (first==item.npos || second==item.npos) return false;
        OffsetPoint point;
        if (!decimal(item.substr(0,first),point.x) ||
            !decimal(item.substr(first+1,second-first-1),point.y)) return false;
        auto color=item.substr(second+1);
        if (!color.empty() && color.front()=='-') {
            point.inverted=true;
            color.remove_prefix(1);
        }
        if (!parseChoice(color,extra,point.choice)) return false;
        points.push_back(point);
        if (end==remaining.npos) break;
        remaining.remove_prefix(end+1);
        if (remaining.empty()) return false;
    }
    return !points.empty();
}
// 回调返回 true 时提前停止。0～3 按行、5～8 按列，与 processColor 对应。
template<class F> void scan(int x1, int y1, int x2, int y2, int dir, F fn) {
    if (dir == 4) {
        // 对应源码 TurnUP→TurnRIGHT→TurnDOWN→TurnLEFT 的螺旋，
        // 步数依次为 1、1、2、2……。只访问区域内坐标；狭长区域将区域外
        // 的整段移动直接跳过，既保留访问顺序，也不因长宽比例大而空转平方次。
        int64_t x = x1 + (x2-x1-1)/2, y = y1 + (y2-y1-1)/2;
        uint64_t visited = 0;
        const uint64_t total = uint64_t(x2-x1) * uint64_t(y2-y1);
        const int dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
        for (int64_t step=1; visited<total; ++step) {
            for (int side=0; side<2 && visited<total; ++side) {
                const int direction = int((2*(step-1)+side)%4);
                int64_t first=0, last=step-1;
                if (dx[direction]) {
                    if (y<y1 || y>=y2) last=-1;
                    else if (dx[direction]>0) {
                        first=(std::max)(first,int64_t(x1)-x);
                        last=(std::min)(last,int64_t(x2)-1-x);
                    } else {
                        first=(std::max)(first,x-int64_t(x2)+1);
                        last=(std::min)(last,x-int64_t(x1));
                    }
                } else {
                    if (x<x1 || x>=x2) last=-1;
                    else if (dy[direction]>0) {
                        first=(std::max)(first,int64_t(y1)-y);
                        last=(std::min)(last,int64_t(y2)-1-y);
                    } else {
                        first=(std::max)(first,y-int64_t(y2)+1);
                        last=(std::min)(last,y-int64_t(y1));
                    }
                }
                for (int64_t i=first; i<=last; ++i) {
                    ++visited;
                    if (fn(int(x+i*dx[direction]),int(y+i*dy[direction]))) return;
                }
                x += step*dx[direction]; y += step*dy[direction];
            }
        }
        return;
    }
    const bool column = dir >= 5;
    const bool reverseX = dir == 2 || dir == 3 || dir == 6 || dir == 8;
    const bool reverseY = dir == 1 || dir == 3 || dir == 7 || dir == 8;
    const int w = x2-x1, h = y2-y1;
    for (int outer = 0; outer < (column ? w : h); ++outer)
        for (int inner = 0; inner < (column ? h : w); ++inner) {
            const int xi = column ? outer : inner, yi = column ? inner : outer;
            if (fn(reverseX ? x2-1-xi : x1+xi, reverseY ? y2-1-yi : y1+yi)) return;
        }
}
struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};
struct Template {
    // 返回文件名必须保留本次调用的写法，不能借用首次缓存时的相对路径。
    std::string name;
    std::shared_ptr<const TemplateData> data;
};
struct TemplatePixels {
    UINT width{}, height{};
    bool pngAlpha{};
    bool transparent{};
    std::vector<unsigned char> pixels;
};
struct FileStamp {
    uint64_t bytes{}, written{};
    bool operator==(const FileStamp&) const = default;
};
// 一次调用固定解析后的绝对路径，防止 SetPath 并发变化时把另一目录的结果记入缓存。
// 所有候选模板都记录状态，包括缺失项，避免前面的缺失模板后来出现仍命中旧结果。
struct TemplateDependency {
    std::string name;
    std::filesystem::path path;
    bool pinned{}, exists{};
    FileStamp stamp{};
    bool operator==(const TemplateDependency&) const = default;
};
struct TemplateDependencies {
    uint64_t revision{};
    std::vector<TemplateDependency> entries;
    bool operator==(const TemplateDependencies&) const = default;
};
thread_local const TemplateDependencies* activeTemplates{};
thread_local bool templatesReusable{true};
bool stamp(const std::filesystem::path& path, FileStamp& value) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    value = {(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
        (uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime};
    return true;
}
struct TemplateEntry {
    std::wstring path;
    // 显式加载的模板不检查源文件、不参与自动淘汰；文件和内存加载共用名称空间。
    bool pinned{};
    FileStamp stamp{};
    TemplatePixels source;
    UINT stride{};
    int direction{};
    std::shared_ptr<const TemplateData> prepared;
    size_t bytes() const {
        return source.pixels.capacity() + (prepared ? prepared->samples.capacity()*sizeof(TemplateData::Sample) : 0);
    }
};
// CPU 模板缓存按需增长，只受运行时字节预算约束；超出时只淘汰普通自动缓存。
// 显式加载占用同一预算；显式条目本身放不下时返回失败，不悄悄丢失已经预加载的图片。
// 正在匹配的只读样本由 shared_ptr
// 保活，淘汰或文件更新不会破坏其他线程。锁只保护模板准备，不覆盖像素扫描和 GPU 读回。
std::mutex templateLock;
std::list<TemplateEntry> templateCache;
size_t templateBytes{};
// 只在显式加载、替换和释放时递增。普通样本准备、方向变化及容量淘汰不改变模板内容。
// 同时核对调用前后版本，避免另一个线程替换模板时发布过时的查找缓存。
uint64_t templateRevision{};
// 清理放在模板锁解除之后，GPU 查找则通过 try_lock 跳过，避免相反的加锁顺序。
struct PruneOnExit { ~PruneOnExit() { fsdm::gpu::PruneTemplates(); } };
bool samePath(const std::wstring& a,const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
}
auto findTemplate(const std::wstring& key) {
    return std::find_if(templateCache.begin(),templateCache.end(),
        [&](const TemplateEntry& e) { return samePath(e.path,key); });
}
void refreshDependencies(TemplateDependencies& dependencies) {
    std::lock_guard lock(templateLock);
    dependencies.revision=templateRevision;
    for(auto& dependency:dependencies.entries) {
        dependency.pinned=false;
        dependency.exists=false;
        dependency.stamp={};
        if(dependency.path.empty()) continue;
        const auto entry=findTemplate(dependency.path.wstring());
        dependency.pinned=entry!=templateCache.end() && entry->pinned;
        dependency.exists=dependency.pinned || stamp(dependency.path,dependency.stamp);
    }
}
TemplateDependencies resolveDependencies(const char* names) {
    TemplateDependencies dependencies;
    for(const auto& name:split(names)) {
        TemplateDependency dependency;
        dependency.name=name;
        dependency.path=FsResolvePath(name.c_str()).lexically_normal();
        dependencies.entries.push_back(std::move(dependency));
    }
    refreshDependencies(dependencies);
    return dependencies;
}
struct TemplateScope {
    const TemplateDependencies* previous=activeTemplates;
    explicit TemplateScope(const TemplateDependencies& dependencies) { activeTemplates=&dependencies; }
    ~TemplateScope() { activeTemplates=previous; }
};
void retire(TemplateEntry& entry) {
    if(entry.prepared) entry.prepared->retired.store(true);
}
auto eraseTemplate(std::list<TemplateEntry>::iterator entry) {
    retire(*entry); templateBytes-=entry->bytes();
    return templateCache.erase(entry);
}
void trimTemplates() {
    while(templateBytes>fsdm::chazhao::templateCacheLimit.load(std::memory_order_relaxed)) {
        auto entry=std::find_if(templateCache.rbegin(),templateCache.rend(),
            [](const TemplateEntry& e) { return !e.pinned; });
        if(entry==templateCache.rend()) break;
        eraseTemplate(std::prev(entry.base()));
    }
}
bool bmp24(const unsigned char* header,size_t size) {
    // 不能只检查 WIC 输出格式：PNG/JPG 转成 BGRA 也不等于输入为 BMP24。
    return size>=30 && header[0]=='B' && header[1]=='M' && header[28]==24 && header[29]==0;
}
bool decodePixels(IWICImagingFactory* factory,IWICBitmapDecoder* decoder,TemplatePixels& image,bool pngAlpha) {
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&image.width, &image.height)) ||
        !image.width || !image.height || uint64_t(image.width)*image.height > FSDM_ZUIDA_ZIJIE/4 ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    auto& pixels = image.pixels;
    pixels.resize(size_t(image.width)*image.height*4);
    if (FAILED(converter->CopyPixels(nullptr, image.width*4, static_cast<UINT>(pixels.size()), pixels.data()))) return false;
    image.pngAlpha=pngAlpha;
    if(pngAlpha) return true;
    const auto p = [&](size_t i) { return pixels.data() + i*4; };
    const size_t corners[] = {0, image.width-1, size_t(image.height-1)*image.width, size_t(image.width)*image.height-1};
    bool transparent = !memcmp(p(corners[0]),p(corners[1]),3) &&
        !memcmp(p(corners[0]),p(corners[2]),3) && !memcmp(p(corners[0]),p(corners[3]),3);
    // 源码注释将全图同色归为非透明图；统一处理方形和非方形，
    // 避免原代码用高度与宽度混比后出现“没有比较任何像素却成功”的情况。
    if (transparent) {
        bool uniform=true;
        for (size_t i=1; i<size_t(image.width)*image.height; ++i)
            if (memcmp(p(i),p(0),3)) { uniform=false; break; }
        if (uniform) transparent=false;
    }
    image.transparent = transparent;
    return true;
}
bool decodeTemplate(const std::filesystem::path& path, TemplatePixels& image) {
    Apartment apartment;
    if(!apartment.valid()) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder)))
        return false;
    GUID container{};
    if(FAILED(decoder->GetContainerFormat(&container))) return false;
    const bool bmp=IsEqualGUID(container,GUID_ContainerFormatBmp)!=0;
    const bool png=IsEqualGUID(container,GUID_ContainerFormatPng)!=0;
    if(!bmp && !png && !IsEqualGUID(container,GUID_ContainerFormatJpeg)) return false;
    if(bmp) {
        std::ifstream file(path,std::ios::binary);
        unsigned char header[30]{};
        if(!file.read(reinterpret_cast<char*>(header),sizeof(header)) || !bmp24(header,sizeof(header))) return false;
    }
    return decodePixels(factory.Get(),decoder.Get(),image,png);
}
bool decodeTemplateBytes(std::vector<unsigned char>& bytes,TemplatePixels& image) {
    if(!bmp24(bytes.data(),bytes.size())) return false;
    uint32_t declared{};
    memcpy(&declared,bytes.data()+2,sizeof(declared));
    // WIC 可能容忍缺失行尾填充，但接口约定传入完整 BMP，不能接受被截断的文件字节。
    if(declared<30 || declared>bytes.size()) return false;
    Apartment apartment;
    if(!apartment.valid()) return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    // InitializeFromMemory 在解码期间借用 bytes；CopyPixels 完成后已不再依赖调用方内存。
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(bytes.data(),static_cast<DWORD>(bytes.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)) &&
        decodePixels(factory.Get(),decoder.Get(),image,false);
}
std::shared_ptr<const TemplateData> prepareTemplate(const TemplatePixels& source, UINT stride, int dir) {
    auto image = std::make_shared<TemplateData>();
    image->width = source.width; image->height = source.height;
    const auto* pixels = source.pixels.data();
    // PNG 仅把 alpha 为 0 的像素当作遮罩；半透明颜色保留 RGB，匹配时不猜测目标背景色。
    const auto ignored=[&](size_t i) {
        const auto* pixel=pixels+i*4;
        if(source.pngAlpha && pixel[3]==0) return true;
        return source.transparent && !source.pngAlpha && !memcmp(pixel,pixels,3);
    };
    // 尺寸、方向或帧行跨度改变时只重建比较样本，不重复解码文件。
    size_t count{};
    for (size_t i=0; i<size_t(source.width)*source.height; ++i)
        if (!ignored(i)) ++count;
    image->samples.reserve(count);
    scan(0,0,int(source.width),int(source.height),dir,[&](int x,int y) {
        const auto* pixel=pixels+(size_t(y)*source.width+x)*4;
        if (!ignored(size_t(y)*source.width+x))
            image->samples.push_back({size_t(y)*stride+size_t(x)*4,{pixel[2],pixel[1],pixel[0]}});
        return false;
    });
    return image;
}
bool loadTemplate(const std::string& name, Template& image, UINT stride, int dir) {
    // 命中缓存时仍计入路径解析、文件属性校验及样本选择，计时边界与原来一致。
    fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::prepare);
    std::filesystem::path path;
    if(activeTemplates) {
        const auto dependency=std::find_if(activeTemplates->entries.begin(),activeTemplates->entries.end(),
            [&](const auto& entry) { return entry.name==name; });
        if(dependency!=activeTemplates->entries.end()) path=dependency->path;
    } else path=FsResolvePath(name.c_str()).lexically_normal();
    if (path.empty()) return false;
    const auto key = path.wstring();
    PruneOnExit cleanup;
    std::lock_guard lock(templateLock);
    auto entry=findTemplate(key);
    FileStamp before{};
    // 显式加载优先于文件检查，故虚拟文件名和已删除的预加载文件仍然可以正常查找。
    const bool pinned=entry!=templateCache.end() && entry->pinned;
    const bool exists=pinned || stamp(path,before);
    if(entry!=templateCache.end() && !pinned && (!exists || !(entry->stamp==before))) {
        eraseTemplate(entry);
        entry=templateCache.end();
    }
    if (!exists) return false;
    if (entry==templateCache.end()) {
        TemplateEntry fresh;
        fresh.path=key; fresh.stamp=before;
        if (!decodeTemplate(path,fresh.source)) { templatesReusable=false;return false; }
        FileStamp after{};
        // 解码期间被覆盖的文件不发布到缓存，下次调用重新加载，避免混合文件版本。
        if (!stamp(path,after) || !(before==after)) { templatesReusable=false;return false; }
        fresh.prepared=prepareTemplate(fresh.source,stride,dir);
        fresh.stride=stride; fresh.direction=dir;
        image.data=fresh.prepared;
        if (fresh.bytes()<=fsdm::chazhao::templateCacheLimit.load(std::memory_order_relaxed)) {
            const size_t bytes=fresh.bytes();
            templateCache.push_front(std::move(fresh));
            templateBytes+=bytes;
        }
    } else {
        if (entry->stride!=stride || entry->direction!=dir) {
            auto prepared=prepareTemplate(entry->source,stride,dir);
            templateBytes-=entry->bytes();
            retire(*entry);
            entry->prepared=std::move(prepared); entry->stride=stride; entry->direction=dir;
            templateBytes+=entry->bytes();
        }
        image.data=entry->prepared;
        templateCache.splice(templateCache.begin(),templateCache,entry);
    }
    trimTemplates();
    image.name=name;
    return image.data && !image.data->samples.empty();
}
// 显式条目发布前先核算其他显式条目的预算；失败时旧模板保持原样。
bool pinTemplate(TemplateEntry fresh) {
    auto old=findTemplate(fresh.path);
    const size_t limit=fsdm::chazhao::templateCacheLimit.load(std::memory_order_relaxed);
    size_t pinnedBytes=fresh.bytes();
    if(pinnedBytes>limit) return false;
    for(auto it=templateCache.begin();it!=templateCache.end();++it) if(it!=old && it->pinned) {
        const size_t bytes=it->bytes();
        if(bytes>limit-pinnedBytes) return false;
        pinnedBytes+=bytes;
    }
    // 先完成可能分配内存的操作，再使旧条目失效，避免分配失败时丢失原模板。
    fresh.pinned=true;
    templateCache.push_front(std::move(fresh));
    templateBytes+=templateCache.front().bytes();
    if(old!=templateCache.end()) eraseTemplate(old);
    trimTemplates();
    ++templateRevision;
    return true;
}
bool pinFile(const std::filesystem::path& path) {
    const auto key=path.wstring();
    auto entry=findTemplate(key);
    if(entry!=templateCache.end() && entry->pinned) return true;
    FileStamp before{},after{};
    if(!stamp(path,before)) return false;
    if(entry!=templateCache.end() && entry->stamp==before) {
        // 普通缓存转为显式缓存不重复解码，也不破坏已经上传的 GPU 样本。
        entry->pinned=true;
        ++templateRevision;
        templateCache.splice(templateCache.begin(),templateCache,entry);
        return true;
    }
    TemplateEntry fresh;
    fresh.path=key; fresh.stamp=before;
    if(!decodeTemplate(path,fresh.source) || !stamp(path,after) || !(before==after)) return false;
    fresh.stride=fresh.source.width*4;
    fresh.prepared=prepareTemplate(fresh.source,fresh.stride,0);
    return pinTemplate(std::move(fresh));
}
bool wildcard(const std::wstring& text) { return text.find_first_of(L"*?")!=std::wstring::npos; }
bool templatePattern(const std::string& name,std::filesystem::path& path) {
    path=FsResolvePath(name.c_str()).lexically_normal();
    const auto file=path.filename().wstring();
    // 只允许文件名带通配符；目录仍使用 SetPath 或明确的绝对路径。
    return !path.empty() && !file.empty() && file!=L"." && file!=L".." && !wildcard(path.parent_path().wstring());
}
std::vector<std::filesystem::path> expandTemplates(const std::filesystem::path& pattern) {
    if(!wildcard(pattern.filename().wstring())) return {pattern};
    WIN32_FIND_DATAW data{};
    const HANDLE handle=FindFirstFileW(pattern.c_str(),&data);
    if(handle==INVALID_HANDLE_VALUE) return {};
    struct Close { HANDLE handle; ~Close() { FindClose(handle); } } close{handle};
    std::vector<std::filesystem::path> paths;
    do {
        if(!(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
            paths.push_back(pattern.parent_path()/data.cFileName);
    } while(FindNextFileW(handle,&data));
    const auto error=GetLastError();
    if(error!=ERROR_NO_MORE_FILES) return {};
    std::sort(paths.begin(),paths.end(),[](const auto& a,const auto& b) {
        return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;
    });
    return paths;
}
// FreePic 必须匹配缓存名称而非枚举磁盘，才能释放已删除的文件及 LoadPicByte 的虚拟名称。
bool wildcardMatch(const std::wstring& pattern,const std::wstring& name) {
    size_t p=0,n=0,star=std::wstring::npos,retry=0;
    while(n<name.size()) {
        if(p<pattern.size() && (pattern[p]==L'?' ||
            CompareStringOrdinal(pattern.data()+p,1,name.data()+n,1,TRUE)==CSTR_EQUAL)) { ++p; ++n; }
        else if(p<pattern.size() && pattern[p]==L'*') { star=p++; retry=n; }
        else if(star!=std::wstring::npos) { p=star+1; n=++retry; }
        else return false;
    }
    while(p<pattern.size() && pattern[p]==L'*') ++p;
    return p==pattern.size();
}
struct Hit { int index, x, y; };
bool match(const Frame& frame, const Template& t, int x, int y, const Color& tolerance) {
    const auto* origin = frame.renderPixel(x,y);
    // 对应 compara* 的核心：任意一个有效像素有任意通道超限，立即换下一个位置。
    // 所有有效像素必须通过；不累计成功比例，也不使用会漏报的抽样近似。
    for (const auto& sample : t.data->samples)
        if (!equal(origin+sample.offset,sample.color,tolerance)) return false;
    return true;
}
bool pictures(int x1,int y1,int x2,int y2,const char* names,const char* color,double sim,int dir,
              bool all,std::vector<Template>& images,std::vector<Hit>& hits) {
    Color delta;
    if (!color || !hex(color, delta) || !validSim(sim) || dir < 0 || dir > 3) return false;
    const int extra=extraTolerance(sim);
    delta.r+=extra; delta.g+=extra; delta.b+=extra;
    const auto files = split(names);
    if (files.empty()) return false;
    Frame frame;
    if (!frame.read() || !frame.region(x1,y1,x2,y2,false)) return false;
    const int left = static_cast<int>(uint64_t(x1)*frame.info.width/frame.info.clientWidth);
    const int top = static_cast<int>(uint64_t(y1)*frame.info.height/frame.info.clientHeight);
    const int right = static_cast<int>(uint64_t(x2)*frame.info.width/frame.info.clientWidth);
    const int bottom = static_cast<int>(uint64_t(y2)*frame.info.height/frame.info.clientHeight);
    if (right<=left || bottom<=top) {fsdm::gpu::Fail(FS_CANSHU_CUOWU);return false;}
    // 模板与 Capture 都保留原图像素。逐原图位置扫描，避免客户区小于原图时漏掉中间像素；
    // 仅在返回结果时换算回客户区，边界取整可能使不同原图位置得到同一客户区坐标。
    // 对应 findPicture 的多图外层循环：先完整搜索第一张，再搜索第二张。
    // 缺失/损坏模板相当于该项无命中；索引始终保留其在原始列表中的位置。
    images.resize(files.size());
    bool loaded=false;
    for (size_t i=0; i<files.size(); ++i) {
        auto& t=images[i];
        if (files[i].empty() || !loadTemplate(files[i],t,frame.info.stride,dir)) continue;
        loaded=true;
        if(t.data->width>UINT(right-left) || t.data->height>UINT(bottom-top)) continue;
        {
            // 累加各张模板的完整扫描时间；不把模板解码或结果字符串拼接算作匹配。
            fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
            const auto record=[&](int rx,int ry) {
                const int x=(std::max)(x1,static_cast<int>(uint64_t(rx)*frame.info.clientWidth/frame.info.width));
                const int y=(std::max)(y1,static_cast<int>(uint64_t(ry)*frame.info.clientHeight/frame.info.height));
                hits.push_back({static_cast<int>(i),x,y});
                return !all || hits.size()>=1500;
            };
            if(frame.gpu) {
                const auto found=frame.gpu->Picture(left,top,right-int(t.data->width)+1,
                    bottom-int(t.data->height)+1,dir,t.data,frame.info.stride,delta,all?1500-hits.size():1);
                for(const auto& [x,y]:found) record(x,y);
            } else scan(left,top,right-int(t.data->width)+1,bottom-int(t.data->height)+1,dir,[&](int rx,int ry) {
                return match(frame,t,rx,ry,delta) && record(rx,ry);
            });
        }
        if ((!all && !hits.empty()) || hits.size()>=1500) break;
    }
    if(!loaded) {fsdm::gpu::Fail(FS_JIAZAI_SHIBAI);return false;}
    return true;
}


using ResultItem=fsdm::jieguo::Item;
struct SearchBackend {bool gpu;};
const SearchBackend cpu{false},gpu{true};
std::atomic<const SearchBackend*> searchBackend{&gpu};
struct SearchParameters {
    std::array<int,5> coordinates{};
    const char* first{};
    const char* second{};
    double similarity{1.0};
    bool picture{}, cpuOnly{};
};
struct CachedSearch {
    std::string key;
    FsZhen frame{};
    FsChazhaoZhuangtai status{};
    std::vector<ResultItem> items;
    size_t bytes() const {
        size_t total=sizeof(CachedSearch)+2*sizeof(void*)+key.capacity()+1+
            items.capacity()*sizeof(ResultItem);
        for(const auto& item:items) total+=item.text.capacity()+item.file.capacity()+2;
        return total;
    }
};
// 所有线程共享有上限的结果副本，命中后仍由 Save 分配独立的公开结果。
// 缓存锁只覆盖复制和淘汰，模板检查、采集、CPU 扫描和 GPU 运算均在锁外执行。
constexpr size_t kResultCacheEntries=64, kResultCacheBytes=16u*1024*1024;
std::mutex resultCacheLock;
std::list<CachedSearch> resultCache;
size_t resultCacheBytes{};
template<class T> void appendValue(std::string& key,const T& value) {
    key.append(reinterpret_cast<const char*>(&value),sizeof(value));
}
void appendText(std::string& key,const char* text) {
    const bool present=text!=nullptr;
    appendValue(key,present);
    const size_t size=text?strlen(text):0;
    appendValue(key,size);
    if(size) key.append(text,size);
}
// 使用带长度的原始字段组成完整键，字符串中的分隔符和浮点舍入均不会造成误命中。
std::string searchKey(const char* name,const SearchParameters& parameters,
    const TemplateDependencies& dependencies) {
    std::string key;
    appendText(key,name);
    appendValue(key,useGpu);
    for(const auto value:parameters.coordinates) appendValue(key,value);
    appendText(key,parameters.first);
    appendText(key,parameters.second);
    appendValue(key,parameters.similarity);
    appendValue(key,dependencies.revision);
    for(const auto& dependency:dependencies.entries) {
        const auto path=dependency.path.u8string();
        appendValue(key,path.size());
        key.append(reinterpret_cast<const char*>(path.data()),path.size());
        appendValue(key,dependency.pinned);
        appendValue(key,dependency.exists);
        appendValue(key,dependency.stamp.bytes);
        appendValue(key,dependency.stamp.written);
    }
    return key;
}
bool reuseResult(const std::string& key,const FsZhen& frame,std::vector<ResultItem>& items) {
    std::lock_guard lock(resultCacheLock);
    const auto entry=std::find_if(resultCache.begin(),resultCache.end(),[&](const auto& cached) {
        return cached.key==key && sameFrame(cached.frame,frame);
    });
    if(entry==resultCache.end()) return false;
    items=entry->items;
    fsdm::gpu::ReuseResult(entry->status);
    resultCache.splice(resultCache.begin(),resultCache,entry);
    return true;
}
void rememberResult(const std::string& key,const FsZhen& frame,
    const FsChazhaoZhuangtai& status,const std::vector<ResultItem>& items) noexcept {
    // 结果缓存属于可选加速，内存不足时保留本次已完成的正常查找结果。
    try {
        CachedSearch fresh{key,frame,status,items};
        const size_t bytes=fresh.bytes();
        if(bytes>kResultCacheBytes) return;
        std::lock_guard lock(resultCacheLock);
        const auto old=std::find_if(resultCache.begin(),resultCache.end(),[&](const auto& cached) {
            return cached.key==key && sameFrame(cached.frame,frame);
        });
        // 先完成可能失败的节点分配，再移除旧项，保证并发完成的相同查询仅保留一份。
        resultCache.push_front(std::move(fresh));
        resultCacheBytes+=bytes;
        if(old!=resultCache.end()) {
            resultCacheBytes-=old->bytes();
            resultCache.erase(old);
        }
        while(resultCache.size()>kResultCacheEntries || resultCacheBytes>kResultCacheBytes) {
            resultCacheBytes-=resultCache.back().bytes();
            resultCache.pop_back();
        }
    } catch(...) {}
}
void Begin(fsdm::haoshi::Operation operation) {
    useGpu=searchBackend.load()->gpu;
    searchedFrame={};
    templatesReusable=true;
    fsdm::haoshi::Reset(operation);fsdm::haoshi::current.gpu=useGpu;
    fsdm::gpu::Reset();
}
// 在发布结果前再次核对绑定版本。即使旧画面已完成计算，解绑之后也不能发布旧坐标。
const FsTongyongJieguo* Finish(const char* function,const std::vector<ResultItem>& items) {
    FsChazhaoZhuangtai state{};fsdm::gpu::Status(state);
    if(state.result==FS_CHENGGONG && state.bindingVersion) {
        FsZhuangtai now{};now.size=sizeof(now);
        if(GetStatus(&now)!=FS_CHENGGONG || !now.bound || now.bindingVersion!=state.bindingVersion) {
            fsdm::gpu::Fail(FS_WEIBANGDING);fsdm::gpu::Status(state);
        }
    }
    return fsdm::jieguo::Save(function,state.result,state.detail,items);
}
template<class F> const FsTongyongJieguo* Execute(const char* name,fsdm::haoshi::Operation operation,
    const SearchParameters& parameters,F fn) {
    Begin(operation);std::vector<ResultItem> items;
    if(parameters.cpuOnly) { useGpu=false;fsdm::haoshi::current.gpu=false; }
    try {
        const auto dependencies=parameters.picture?resolveDependencies(parameters.first):TemplateDependencies{};
        TemplateScope scope(dependencies);
        const auto key=searchKey(name,parameters,dependencies);
        FsZhuangtai now{};now.size=sizeof(now);
        // 只检查快照元数据。命中时不读回整帧，也不申请 GPU 租约或重新提交比较。
        if(fsdm::zhenhuancun::ResultReuseEnabled() && GetStatus(&now)==FS_CHENGGONG &&
            now.result==FS_CHENGGONG && now.bound && now.hasFrame &&
            reuseResult(key,now.frame,items)) return Finish(name,items);
        fn(items);
        FsChazhaoZhuangtai state{};fsdm::gpu::Status(state);
        if(state.result==FS_CHENGGONG && searchedFrame.bindingVersion) {
            auto after=dependencies;
            if(parameters.picture) refreshDependencies(after);
            // 用实际参与计算的帧编号登记，容量查询后采集可能已经推进到下一张快照。
            // 模板在计算期间变化时照常返回此次结果，后续调用会重新查找。
            if(fsdm::zhenhuancun::ResultReuseEnabled() && templatesReusable && after==dependencies)
                rememberResult(key,searchedFrame,state,items);
        }
    } catch(...) {
        items.clear();FsChazhaoZhuangtai state{};fsdm::gpu::Status(state);
        if(state.result==FS_CHENGGONG) fsdm::gpu::Fail(FS_CAIJI_SHIBAI,E_FAIL);
    }
    return Finish(name,items);
}
ResultItem Coordinate(int id,int x,int y) {
    ResultItem item;item.fields=FS_JG_ID|FS_JG_XY;item.id=id;item.x=x;item.y=y;return item;
}
uint32_t ReadColor(Frame& frame,int x,int y) {
    if(frame.gpu) return frame.gpu->Color(x,y);
    const auto* p=frame.pixel(x,y);return uint32_t(p[2])<<16|uint32_t(p[1])<<8|p[0];
}
void PixelResult(int x,int y,const char* choices,double sim,bool compare,std::vector<ResultItem>& items) {
    std::vector<ColorChoice> colors;
    if(compare) {
        const auto parts=split(choices);
        if(!validSim(sim)||parts.empty()||parts.size()>10) return;
        for(const auto& part:parts) {
            ColorChoice c;if(!parseChoice(part,extraTolerance(sim),c)) return;colors.push_back(c);
        }
    }
    Frame frame;if(!frame.read()) return;
    if(x>=int(frame.info.clientWidth)||y>=int(frame.info.clientHeight)) {fsdm::gpu::Fail(FS_CANSHU_CUOWU);return;}
    x=(std::max)(0,x);y=(std::max)(0,y);
    fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
    const uint32_t rgb=ReadColor(frame,x,y);
    int id=-1;
    if(compare) {
        const unsigned char bytes[]{static_cast<unsigned char>(rgb),static_cast<unsigned char>(rgb>>8),static_cast<unsigned char>(rgb>>16),0};
        for(size_t i=0;i<colors.size();++i) if(equal(bytes,colors[i].color,colors[i].delta)) {id=int(i);break;}
        if(id<0) return;
    }
    auto item=Coordinate(id,x,y);item.fields=FS_JG_XY|FS_JG_COLOR|(compare?FS_JG_ID:0u);item.color=rgb;
    items.push_back(std::move(item));
}
// 保留TSPlug原有颜色优先级、方向和截断规则；只改变结果的保存形式。
void ColorResults(int x1,int y1,int x2,int y2,const char* color,double sim,int dir,bool all,
    std::vector<ResultItem>& items) {
    if(!validSim(sim)||dir<0||dir>8||(all&&dir==4)) return;
    const auto parts=split(color);if(parts.empty()) return;
    std::vector<ColorChoice> choices;
    for(const auto& part:parts) {
        if(!all&&part.empty()) continue;
        if(!all&&(part.size()!=13||part[6]!='-')) return;
        ColorChoice choice;if(!parseChoice(part,extraTolerance(sim),choice)) return;
        choices.push_back(choice);
    }
    if(choices.empty()) return;
    Frame frame;if(!frame.read()||!frame.region(x1,y1,x2,y2,true)) return;
    fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
    const size_t maxHitsPerColor=size_t(x2-x1)*size_t(y2-y1);
    for(size_t i=0;i<choices.size();++i) {
        const auto record=[&](int x,int y) {
            items.push_back(Coordinate(int(i),x,y));
            return !all;
        };
        const auto& c=choices[i];
        if(frame.gpu) {
            const Point point{0,0,fsdm::chazhao::Pack(c.color),fsdm::chazhao::Pack(c.delta)};
            const auto hits=frame.gpu->Find(x1,y1,x2,y2,dir,std::span(&point,1),
                all?maxHitsPerColor:1);
            for(const auto& [x,y]:hits) if(record(x,y)) break;
        } else scan(x1,y1,x2,y2,dir,[&](int x,int y){return equal(frame.pixel(x,y),c.color,c.delta)&&record(x,y);});
        if(!all&&!items.empty()) break;
    }
}
void MultiResults(int x1,int y1,int x2,int y2,const char* first,const char* offsets,double sim,int dir,
    bool all,std::vector<ResultItem>& items) {
    if(!first||!validSim(sim)||dir<0||dir>3) return;
    ColorChoice origin;std::vector<OffsetPoint> points;
    if(!parseChoice(first,extraTolerance(sim),origin)||!parseOffsets(offsets,extraTolerance(sim),points)) return;
    Frame frame;if(!frame.read()||!frame.region(x1,y1,x2,y2,true)) return;
    fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
    int64_t left=x1,top=y1,right=x2,bottom=y2;
    for(const auto& p:points) {
        left=(std::max)(left,int64_t(x1)-p.x);top=(std::max)(top,int64_t(y1)-p.y);
        right=(std::min)(right,int64_t(x2)-p.x);bottom=(std::min)(bottom,int64_t(y2)-p.y);
    }
    if(left>=right||top>=bottom) return;
    const auto record=[&](int x,int y){items.push_back(Coordinate(0,x,y));return !all||items.size()>=1500;};
    if(frame.gpu) {
        std::vector<Point> pattern{{0,0,fsdm::chazhao::Pack(origin.color),fsdm::chazhao::Pack(origin.delta)}};
        for(const auto& p:points) pattern.push_back({p.x,p.y,fsdm::chazhao::Pack(p.choice.color),
            fsdm::chazhao::Pack(p.choice.delta)|(p.inverted?0x1000000u:0u)});
        for(const auto& [x,y]:frame.gpu->Find(int(left),int(top),int(right),int(bottom),dir,pattern,all?1500:1)) record(x,y);
    } else scan(int(left),int(top),int(right),int(bottom),dir,[&](int x,int y) {
        if(!equal(frame.pixel(x,y),origin.color,origin.delta)) return false;
        for(const auto& p:points) if(equal(frame.pixel(int(int64_t(x)+p.x),int(int64_t(y)+p.y)),
            p.choice.color,p.choice.delta)==p.inverted) return false;
        return record(x,y);
    });
}
// OpenCV 使用现有模板样本重建掩码，沿用透明像素和缓存的处理方式。
// 将平方误差开方后归一化为均方根误差，避免平方值让相似度阈值显得过宽；分数为 1 表示完全一致。
const FsTongyongJieguo* CvPictureResults(const char* name,int x1,int y1,int x2,int y2,
    const char* names,double sim,int dir,bool all) {
    return Execute(name,fsdm::haoshi::Operation::findPicture,
        {{x1,y1,x2,y2,dir},names,nullptr,sim,true,true},[&](auto& items) {
        if(!validSim(sim) || dir<0 || dir>3 || !names || !*names) {
            fsdm::gpu::Fail(FS_CANSHU_CUOWU); return;
        }
        // OpenCV 入口已固定 CPU 路径，使用同一帧的 BGRA 像素进行模板匹配。
        Frame frame;
        if(!frame.read() || !frame.region(x1,y1,x2,y2,false)) return;
        const int left=int(uint64_t(x1)*frame.info.width/frame.info.clientWidth);
        const int top=int(uint64_t(y1)*frame.info.height/frame.info.clientHeight);
        const int right=int(uint64_t(x2)*frame.info.width/frame.info.clientWidth);
        const int bottom=int(uint64_t(y2)*frame.info.height/frame.info.clientHeight);
        if(right<=left || bottom<=top) { fsdm::gpu::Fail(FS_CANSHU_CUOWU); return; }
        cv::Mat bgra(int(frame.info.height),int(frame.info.width),CV_8UC4,
            const_cast<unsigned char*>(frame.bytes->data()),frame.info.stride);
        cv::Mat source;
        cv::cvtColor(bgra(cv::Rect(left,top,right-left,bottom-top)),source,cv::COLOR_BGRA2BGR);
        const auto files=split(names);
        bool loaded=false;
        for(size_t i=0;i<files.size();++i) {
            Template image;
            if(files[i].empty() || !loadTemplate(files[i],image,frame.info.stride,0)) continue;
            loaded=true;
            const int width=int(image.data->width), height=int(image.data->height);
            if(width>source.cols || height>source.rows) continue;
            cv::Mat target(height,width,CV_8UC3,cv::Scalar(0,0,0));
            cv::Mat mask(height,width,CV_8UC1,cv::Scalar(0));
            for(const auto& sample:image.data->samples) {
                const int y=int(sample.offset/frame.info.stride);
                const int x=int((sample.offset%frame.info.stride)/4);
                target.at<cv::Vec3b>(y,x)=cv::Vec3b(static_cast<unsigned char>(sample.color.b),
                    static_cast<unsigned char>(sample.color.g),static_cast<unsigned char>(sample.color.r));
                mask.at<unsigned char>(y,x)=255;
            }
            cv::Mat scores;
            {
                fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::search);
                cv::matchTemplate(source,target,scores,cv::TM_SQDIFF,mask);
            }
            const double maximum=double(image.data->samples.size())*3.0*255.0*255.0;
            scan(0,0,scores.cols,scores.rows,dir,[&](int x,int y) {
                const double difference=(std::max)(0.0,double(scores.at<float>(y,x)));
                const double similarity=1.0-std::sqrt(difference/maximum);
                if(!std::isfinite(similarity) || similarity+1e-7<sim) return false;
                const int rx=left+x, ry=top+y;
                auto item=Coordinate(int(i),
                    (std::max)(x1,int(uint64_t(rx)*frame.info.clientWidth/frame.info.width)),
                    (std::max)(y1,int(uint64_t(ry)*frame.info.clientHeight/frame.info.height)));
                item.fields|=FS_JG_FILE; item.file=files[i]; items.push_back(std::move(item));
                return !all || items.size()>=1500;
            });
            if((!all && !items.empty()) || items.size()>=1500) break;
        }
        if(!loaded) fsdm::gpu::Fail(FS_JIAZAI_SHIBAI);
    });
}
const FsTongyongJieguo* PictureResults(const char* name,int x1,int y1,int x2,int y2,const char* names,
    const char* delta,double sim,int dir,bool all) {
    return Execute(name,fsdm::haoshi::Operation::findPicture,
        {{x1,y1,x2,y2,dir},names,delta,sim,true,false},[&](auto& items) {
        std::vector<Template> images;std::vector<Hit> hits;
        if(!pictures(x1,y1,x2,y2,names,delta,sim,dir,all,images,hits)) return;
        for(const auto& h:hits) {
            auto item=Coordinate(h.index,h.x,h.y);item.fields|=FS_JG_FILE;item.file=images[h.index].name;
            items.push_back(std::move(item));
        }
    });
}
}

// 将截图的紧密排列 BGRA 像素登记为不透明模板；缓存键沿用找图的 SetPath 解析规则。
int FsPinTemplatePixels(const char* name, UINT width, UINT height, const unsigned char* bgra) {
    try {
        std::filesystem::path path;
        if (!name || !*name || !width || !height || !bgra ||
            uint64_t(width) * height > FSDM_ZUIDA_ZIJIE / 4 ||
            strchr(name, '|') || !templatePattern(name, path) || wildcard(path.filename().wstring()))
            return 0;
        TemplateEntry fresh;
        fresh.path = path.lexically_normal().wstring();
        fresh.source.width = width;
        fresh.source.height = height;
        fresh.source.pixels.assign(bgra, bgra + size_t(width) * height * 4);
        fresh.stride = width * 4;
        fresh.prepared = prepareTemplate(fresh.source, fresh.stride, 0);
        PruneOnExit cleanup;
        std::lock_guard lock(templateLock);
        return pinTemplate(std::move(fresh)) ? 1 : 0;
    } catch (...) { return 0; }
}

int WINAPI LoadPic(const char* pic_name) {
    try {
        const auto names=split(pic_name);
        if(names.empty()) return 0;
        PruneOnExit cleanup;
        std::lock_guard lock(templateLock);
        // 列表逐项处理；失败不会撤销此前已成功的项，调用方可用 FreePic 统一释放。
        for(const auto& name:names) {
            std::filesystem::path pattern;
            if(!templatePattern(name,pattern)) return 0;
            const auto paths=expandTemplates(pattern);
            if(paths.empty()) return 0;
            for(const auto& path:paths) if(!pinFile(path)) return 0;
        }
        return 1;
    } catch(...) { return 0; }
}
int WINAPI SetTemplateCacheLimit(uint64_t bytes) {
    if(!bytes || bytes>std::numeric_limits<size_t>::max()) return 0;
    PruneOnExit cleanup;
    std::lock_guard lock(templateLock);
    size_t pinnedBytes=0;
    for(const auto& entry:templateCache) if(entry.pinned) {
        const size_t used=entry.bytes();
        if(used>bytes-pinnedBytes) return 0;
        pinnedBytes+=used;
    }
    fsdm::chazhao::templateCacheLimit.store(static_cast<size_t>(bytes),std::memory_order_relaxed);
    trimTemplates();
    return 1;
}
int WINAPI FreePic(const char* pic_name) {
    try {
        const auto names=split(pic_name);
        if(names.empty()) return 0;
        std::vector<std::filesystem::path> patterns;
        for(const auto& name:names) {
            std::filesystem::path path;
            if(!templatePattern(name,path)) return 0;
            patterns.push_back(std::move(path));
        }
        PruneOnExit cleanup;
        std::lock_guard lock(templateLock);
        // 显式释放即撤销旧的结果版本，包含仅被结果缓存引用、已由容量策略淘汰的模板。
        ++templateRevision;
        for(auto it=templateCache.begin();it!=templateCache.end();) {
            const std::filesystem::path cached(it->path);
            const bool found=std::any_of(patterns.begin(),patterns.end(),[&](const auto& pattern) {
                return samePath(cached.parent_path().wstring(),pattern.parent_path().wstring()) &&
                    wildcardMatch(pattern.filename().wstring(),cached.filename().wstring());
            });
            if(found) it=eraseTemplate(it);
            else ++it;
        }
        return 1;
    } catch(...) { return 0; }
}
int WINAPI LoadPicByte(uintptr_t addr,int size,const char* pic_name) {
    try {
        std::filesystem::path path;
        if(!addr || size<30 || static_cast<size_t>(size)>fsdm::chazhao::templateCacheLimit.load(std::memory_order_relaxed) ||
            static_cast<size_t>(size)>UINTPTR_MAX-addr || !pic_name || strchr(pic_name,'|') ||
            !templatePattern(pic_name,path) || wildcard(path.filename().wstring())) return 0;
        // 不直接解引用任意调用方地址；先检查文件头，再复制完整数据，不会因无效地址触发访问异常。
        unsigned char header[30]{};
        SIZE_T copied{};
        const auto* memory=reinterpret_cast<const void*>(addr);
        if(!ReadProcessMemory(GetCurrentProcess(),memory,header,sizeof(header),&copied) ||
            copied!=sizeof(header) || !bmp24(header,sizeof(header))) return 0;
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        if(!ReadProcessMemory(GetCurrentProcess(),memory,bytes.data(),bytes.size(),&copied) ||
            copied!=bytes.size()) return 0;
        TemplateEntry fresh;
        fresh.path=path.wstring();
        if(!decodeTemplateBytes(bytes,fresh.source)) return 0;
        fresh.stride=fresh.source.width*4;
        fresh.prepared=prepareTemplate(fresh.source,fresh.stride,0);
        PruneOnExit cleanup;
        std::lock_guard lock(templateLock);
        return pinTemplate(std::move(fresh)) ? 1 : 0;
    } catch(...) { return 0; }
}

int WINAPI SetFindBackend(const char* backend) {
    if (!backend) return 0;
    if(!strcmp(backend,"cpu")) searchBackend.store(&cpu);
    else if(!strcmp(backend,"gpu")) searchBackend.store(&gpu);
    else return 0;
    return 1;
}
int WINAPI GetFindStatus(FsChazhaoZhuangtai* output) {
    if(!output || output->size!=sizeof(*output)) return FS_CANSHU_CUOWU;
    fsdm::gpu::Status(*output);output->backend=searchBackend.load()->gpu?1u:0u;
    return FS_CHENGGONG;
}
const FsTongyongJieguo* WINAPI GetColor(int x,int y) {
    return Execute("GetColor",fsdm::haoshi::Operation::getColor,
        {{x,y,0,0,0}},[&](auto& items){PixelResult(x,y,nullptr,1,false,items);});
}
const FsTongyongJieguo* WINAPI CmpColor(int x,int y,const char* color,double sim) {
    return Execute("CmpColor",fsdm::haoshi::Operation::compareColor,
        {{x,y,0,0,0},color,nullptr,sim},[&](auto& items){PixelResult(x,y,color,sim,true,items);});
}
// 内部枚举仍复用临时字符串，只有公开入口才分配并登记返回数组。
static const char* MatchPicNameTemporary(const char* pic_name) {
    Begin(fsdm::haoshi::Operation::matchNames);
    fsdm::haoshi::current.gpu=false; // 仅枚举磁盘，不获取采集/查找设备。
    answer.clear();
    try {
        fsdm::haoshi::Timer timer(fsdm::haoshi::Phase::prepare);
        const auto names=split(pic_name);
        if(names.empty()) return answer.c_str();
        std::vector<std::wstring> seen;
        for(const auto& name:names) {
            std::filesystem::path pattern;
            if(!templatePattern(name,pattern)) {answer.clear();return answer.c_str();}
            // 返回形式跟随调用方：相对路径保留相对目录，绝对路径保留完整路径。
            const std::filesystem::path input=std::u8string(
                reinterpret_cast<const char8_t*>(name.data()),name.size());
            for(const auto& path:expandTemplates(pattern)) {
                const auto extension=path.extension().wstring();
                constexpr const wchar_t* supportedExtensions[]{L".bmp",L".jpg",L".jpeg",L".png"};
                const bool supported=std::any_of(std::begin(supportedExtensions),std::end(supportedExtensions),
                    [&](const wchar_t* candidate) {
                        return CompareStringOrdinal(extension.c_str(),-1,candidate,-1,TRUE)==CSTR_EQUAL;
                    });
                if(!supported ||
                    !wildcardMatch(pattern.filename().wstring(),path.filename().wstring())) continue;
                const auto attributes=GetFileAttributesW(path.c_str());
                if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
                const auto key=path.wstring();
                if(std::any_of(seen.begin(),seen.end(),[&](const auto& old){return samePath(old,key);})) continue;
                seen.push_back(key);
                const auto returned=(input.parent_path()/path.filename()).u8string();
                if(!answer.empty()) answer+='|';
                answer.append(reinterpret_cast<const char*>(returned.data()),returned.size());
            }
        }
        fsdm::gpu::Fail(FS_CHENGGONG);
    } catch(...) {answer.clear();fsdm::gpu::Fail(FS_CAIJI_SHIBAI,E_FAIL);}
    return answer.c_str();
}
const char* WINAPI MatchPicName(const char* pic_name) {
    return fsdm::zifuchuan::Save(fsdm::zifuchuan::Function::MatchPicName,MatchPicNameTemporary(pic_name));
}
const FsTongyongJieguo* WINAPI FindColor(int x1,int y1,int x2,int y2,const char* color,double sim,int dir) {
    return Execute("FindColor",fsdm::haoshi::Operation::findColor,
        {{x1,y1,x2,y2,dir},color,nullptr,sim},[&](auto& items){ColorResults(x1,y1,x2,y2,color,sim,dir,false,items);});
}
const FsTongyongJieguo* WINAPI FindColorEx(int x1,int y1,int x2,int y2,const char* color,double sim,int dir) {
    return Execute("FindColorEx",fsdm::haoshi::Operation::findColor,
        {{x1,y1,x2,y2,dir},color,nullptr,sim},[&](auto& items){ColorResults(x1,y1,x2,y2,color,sim,dir,true,items);});
}
const FsTongyongJieguo* WINAPI FindMultiColor(int x1,int y1,int x2,int y2,const char* first,const char* offsets,double sim,int dir) {
    return Execute("FindMultiColor",fsdm::haoshi::Operation::findColor,
        {{x1,y1,x2,y2,dir},first,offsets,sim},[&](auto& items){MultiResults(x1,y1,x2,y2,first,offsets,sim,dir,false,items);});
}
const FsTongyongJieguo* WINAPI FindMultiColorEx(int x1,int y1,int x2,int y2,const char* first,const char* offsets,double sim,int dir) {
    return Execute("FindMultiColorEx",fsdm::haoshi::Operation::findColor,
        {{x1,y1,x2,y2,dir},first,offsets,sim},[&](auto& items){MultiResults(x1,y1,x2,y2,first,offsets,sim,dir,true,items);});
}
const FsTongyongJieguo* WINAPI FindPic(int x1,int y1,int x2,int y2,const char* names,const char* delta,double sim,int dir) {
    return PictureResults("FindPic",x1,y1,x2,y2,names,delta,sim,dir,false);
}
const FsTongyongJieguo* WINAPI FindPicS(int x1,int y1,int x2,int y2,const char* names,const char* delta,double sim,int dir) {
    return PictureResults("FindPicS",x1,y1,x2,y2,names,delta,sim,dir,false);
}
const FsTongyongJieguo* WINAPI FindPicEx(int x1,int y1,int x2,int y2,const char* names,const char* delta,double sim,int dir) {
    return PictureResults("FindPicEx",x1,y1,x2,y2,names,delta,sim,dir,true);
}
const FsTongyongJieguo* WINAPI FindPicExS(int x1,int y1,int x2,int y2,const char* names,const char* delta,double sim,int dir) {
    return PictureResults("FindPicExS",x1,y1,x2,y2,names,delta,sim,dir,true);
}

// 四个入口共享实现，内存图通过 LoadPicByte 的别名进入现有模板缓存。
const FsTongyongJieguo* WINAPI CvFindPic(int x1,int y1,int x2,int y2,const char* name,double sim,int dir) {
    return CvPictureResults("CvFindPic",x1,y1,x2,y2,name,sim,dir,false);
}
const FsTongyongJieguo* WINAPI CvFindPicEx(int x1,int y1,int x2,int y2,const char* name,double sim,int dir) {
    return CvPictureResults("CvFindPicEx",x1,y1,x2,y2,name,sim,dir,true);
}
const FsTongyongJieguo* WINAPI CvFindPicMem(int x1,int y1,int x2,int y2,const char* name,double sim,int dir) {
    return CvPictureResults("CvFindPicMem",x1,y1,x2,y2,name,sim,dir,false);
}
const FsTongyongJieguo* WINAPI CvFindPicMemEx(int x1,int y1,int x2,int y2,const char* name,double sim,int dir) {
    return CvPictureResults("CvFindPicMemEx",x1,y1,x2,y2,name,sim,dir,true);
}
