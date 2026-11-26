// OCR 对外入口：获取一次完整画面、颜色预处理、调用模型/点阵字库，再直接填入通用结果数组。
// 模型与字库不依赖采集后端；解绑定只使本次画面失效，不重置已加载的识别资源。
#include "WenziShibie.h"
#include "WenziMoxing.h"
#include "WenziZiku.h"
#include "Haoshi.h"
#include "ZifuchuanNeicun.h"
#include "TongyongJieguo.h"
#include "ZhenhuanCun.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <sstream>
#include <string_view>
#include <atomic>
#include <shared_mutex>

using Microsoft::WRL::ComPtr;
namespace fsdm::wenzi {
// 工作台在 OCR 调用结束后读取此标记，用来区分缓存复用与新帧读回。
thread_local bool ocrFrameReused{};
thread_local bool ocrResultReused{};
namespace {
struct Settings {std::mutex lock;Selection selection;};
Settings& Config() {static auto* s=new Settings;return *s;}
// 识别期间共享读取配置，修改模型、字库或后端时独占修改。
// 保证缓存键与实际参与识别的资源来自同一组设置，避免并发切换后误存旧结果。
std::shared_mutex ocrConfigurationLock;
std::atomic<uint64_t> ocrConfigRevision{1};
void BumpOcrConfig() noexcept { ocrConfigRevision.fetch_add(1,std::memory_order_relaxed); }
thread_local FsOcrZhuangtai diagnostic{};
// 导出返回前保存分段，随后工作台调用 GetOcrError/status 不会把最近一次识别计时清掉。
struct SaveTiming {
    ~SaveTiming() {
        const auto& t=haoshi::current;
        if(t.operation!=haoshi::Operation::ocr) return;
        diagnostic.initializationMs=t.initialize.ms;diagnostic.acquireMs=t.acquire.ms;
        diagnostic.preprocessMs=t.preprocess.ms;diagnostic.detectionMs=t.detect.ms;
        diagnostic.recognitionMs=t.recognize.ms;diagnostic.resultMs=t.result.ms;
    }
};
struct Request {
    int left{},top{},right{},bottom{};
    const char* color{};
    double sim{};
    const char* file{};
};
struct Color {
    enum Type {rgb,hsv,gray} type{};
    std::array<double,3> value{},delta{};
};
struct Filter {std::vector<Color> colors;bool background{};std::string separator;};
// 保存内部条目和全文，公开返回时仍由 Save 独立分配，调用者释放结果不会破坏缓存。
// 空结果同样登记；最多保留 32 组、8 MiB，避免连续切换帧或参数时无限增长。
struct CachedOcrResult {
    std::string key;
    FsZhen frame{};
    std::vector<fsdm::jieguo::Item> items;
    std::string fullText;
    size_t bytes() const {
        size_t total=sizeof(CachedOcrResult)+key.capacity()+fullText.capacity()+
            items.capacity()*sizeof(fsdm::jieguo::Item);
        for(const auto& item:items) total+=item.text.capacity()+item.file.capacity()+2;
        return total;
    }
};
std::mutex ocrResultCacheLock;
std::list<CachedOcrResult> ocrResultCache;
size_t ocrResultCacheBytes{};
constexpr size_t kOcrResultCacheEntries=32, kOcrResultCacheBytes=8u*1024*1024;
bool SameOcrFrame(const FsZhen& a,const FsZhen& b) {
    return a.bindingVersion && a.bindingVersion==b.bindingVersion &&
        a.frameNumber==b.frameNumber && a.width==b.width && a.height==b.height &&
        a.stride==b.stride && a.clientWidth==b.clientWidth && a.clientHeight==b.clientHeight;
}
template<class T> void AppendOcrKey(std::string& key,const T& value) {
    key.append(reinterpret_cast<const char*>(&value),sizeof(value));
}
void AppendOcrText(std::string& key,const char* text) {
    const bool present=text!=nullptr;AppendOcrKey(key,present);
    const size_t size=text?strlen(text):0;AppendOcrKey(key,size);
    if(size) key.append(text,size);
}
std::string MakeOcrKey(const char* name,const Request& request,const Selection& selection) {
    std::string key;AppendOcrText(key,name);
    AppendOcrKey(key,request.left);AppendOcrKey(key,request.top);
    AppendOcrKey(key,request.right);AppendOcrKey(key,request.bottom);
    AppendOcrText(key,request.color);AppendOcrKey(key,request.sim);
    AppendOcrKey(key,selection.model);AppendOcrKey(key,selection.dict);
    AppendOcrKey(key,selection.backend);AppendOcrKey(key,selection.engine);
    const auto revision=ocrConfigRevision.load(std::memory_order_relaxed);
    AppendOcrKey(key,revision);
    return key;
}
bool ReadCurrentOcrFrame(FsZhen& frame) {
    FsZhuangtai state{};state.size=sizeof(state);
    if(GetStatus(&state)!=FS_CHENGGONG||state.result!=FS_CHENGGONG||
        !state.bound||!state.hasFrame) return false;
    frame=state.frame;return frame.bindingVersion!=0;
}
bool ReuseOcrResult(const std::string& key,const FsZhen& frame,
    std::vector<fsdm::jieguo::Item>& items,std::string& fullText) {
    if(!zhenhuancun::ResultReuseEnabled()) return false;
    std::lock_guard guard(ocrResultCacheLock);
    const auto entry=std::find_if(ocrResultCache.begin(),ocrResultCache.end(),[&](const auto& cached) {
        return cached.key==key&&SameOcrFrame(cached.frame,frame);
    });
    if(entry==ocrResultCache.end()) return false;
    items=entry->items;fullText=entry->fullText;
    diagnostic={};diagnostic.result=FS_CHENGGONG;
    diagnostic.frameNumber=frame.frameNumber;diagnostic.bindingVersion=frame.bindingVersion;
    ocrFrameReused=true;
    ocrResultReused=true;
    haoshi::Reset(haoshi::Operation::ocr);
    ocrResultCache.splice(ocrResultCache.begin(),ocrResultCache,entry);
    return true;
}
void RememberOcrResult(const std::string& key,const FsZhen& frame,
    const std::vector<fsdm::jieguo::Item>& items,const std::string& fullText) noexcept {
    if(!zhenhuancun::ResultReuseEnabled()||!frame.bindingVersion) return;
    try {
        CachedOcrResult fresh{key,frame,items,fullText};
        const size_t bytes=fresh.bytes();if(bytes>kOcrResultCacheBytes) return;
        std::lock_guard guard(ocrResultCacheLock);
        const auto old=std::find_if(ocrResultCache.begin(),ocrResultCache.end(),[&](const auto& cached) {
            return cached.key==key&&SameOcrFrame(cached.frame,frame);
        });
        ocrResultCache.push_front(std::move(fresh));ocrResultCacheBytes+=bytes;
        if(old!=ocrResultCache.end()) {ocrResultCacheBytes-=old->bytes();ocrResultCache.erase(old);}
        while(ocrResultCache.size()>kOcrResultCacheEntries||ocrResultCacheBytes>kOcrResultCacheBytes) {
            ocrResultCacheBytes-=ocrResultCache.back().bytes();ocrResultCache.pop_back();
        }
    } catch(...) {}
}
std::string Trim(std::string value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
std::vector<std::string> Split(const std::string& value,char separator) {
    std::vector<std::string> out;
    size_t begin=0;
    for(;;) {
        const auto end=value.find(separator,begin);
        out.push_back(value.substr(begin,end-begin));
        if(end==std::string::npos) break;
        begin=end+1;
    }
    return out;
}
double Number(const std::string& s) {
    if(s.empty()) throw std::runtime_error("颜色数值为空");
    double value{};
    const auto parsed=std::from_chars(s.data(),s.data()+s.size(),value);
    if(parsed.ec!=std::errc{}||parsed.ptr!=s.data()+s.size()||!std::isfinite(value)||value<0)
        throw std::runtime_error("颜色数值格式无效");
    return value;
}
std::array<double,3> Hex(const std::string& s) {
    if(s.size()!=6) throw std::runtime_error("RGB 颜色与偏色必须是六位十六进制");
    std::array<double,3> values{};
    for(size_t i=0;i<3;++i) {
        unsigned v{};const auto parsed=std::from_chars(s.data()+i*2,s.data()+i*2+2,v,16);
        if(parsed.ec!=std::errc{}||parsed.ptr!=s.data()+i*2+2) throw std::runtime_error("RGB 颜色包含非法字符");
        values[i]=v;
    }
    return values;
}
std::array<double,3> Hsv(const std::string& s) {
    const auto parts=Split(s,'.');
    if(parts.size()!=3) throw std::runtime_error("HSV 格式必须为 H.S.V");
    std::array<double,3> out{Number(parts[0]),Number(parts[1]),Number(parts[2])};
    if(out[0]>360||out[1]>100||out[2]>100) throw std::runtime_error("HSV 范围为 H=0～360、S/V=0～100");
    return out;
}
Filter ParseFilter(const char* text) {
    if(!text) throw std::runtime_error("color_format 不能为空指针");
    std::string value(text);Filter out;
    if(value.size()>4096||(!value.empty()&&Wide(value).empty())) throw std::runtime_error("颜色格式长度或 UTF-8 无效");
    const auto comma=value.find(',');
    if(comma!=value.npos) {out.separator=value.substr(comma+1);value.resize(comma);}
    value=Trim(value);
    if(value.starts_with("b@")) {out.background=true;value.erase(0,2);}
    if(value.empty()) {
        if(out.background) throw std::runtime_error("b@ 后必须指定背景颜色");
        return out;
    }
    auto pieces=Split(value,'|');
    if(pieces.size()>10) throw std::runtime_error("OCR 最多支持10种颜色");
    for(auto piece:pieces) {
        piece=Trim(piece);const auto dash=piece.find('-');
        const auto base=Trim(piece.substr(0,dash)),delta=dash==piece.npos?std::string{}:Trim(piece.substr(dash+1));
        Color c;
        if(base.empty()) throw std::runtime_error("OCR 颜色列表包含空项");
        if(base.front()=='#') {
            c.type=Color::gray;c.value[0]=Number(base.substr(1));c.delta[0]=delta.empty()?0:Number(delta);
            if(c.value[0]>255||c.delta[0]>255) throw std::runtime_error("灰度范围为0～255");
        } else if(base.find('.')!=base.npos) {
            c.type=Color::hsv;c.value=Hsv(base);c.delta=delta.empty()?std::array<double,3>{}:Hsv(delta);
        } else {c.type=Color::rgb;c.value=Hex(base);c.delta=Hex(delta.empty()?"000000":delta);}
        out.colors.push_back(c);
    }
    return out;
}
bool Matches(const unsigned char* pixel,const Color& c) {
    const double r=pixel[2],g=pixel[1],b=pixel[0];
    std::array<double,3> values{r,g,b};
    if(c.type==Color::gray) return std::abs((r*299+g*587+b*114)/1000-c.value[0])<=c.delta[0];
    if(c.type==Color::hsv) {
        const double high=std::max({r,g,b}),low=std::min({r,g,b}),range=high-low;
        double hue=0;
        if(range>0) {
            hue=60*(high==r?(g-b)/range:high==g?(b-r)/range+2:(r-g)/range+4);
            if(hue<0) hue+=360;
        }
        values={hue,high?range/high*100:0,high/255*100};
    }
    for(size_t i=0;i<3;++i) {
        double distance=std::abs(values[i]-c.value[i]);
        if(c.type==Color::hsv&&i==0) distance=std::min(distance,360-distance);
        if(distance>c.delta[i]+1e-8) return false;
    }
    return true;
}
void Preprocess(Image& image,const Filter& f,bool dictionary) {
    haoshi::Timer timer(haoshi::Phase::preprocess);
    if(!f.colors.empty()) {
        for(size_t i=0;i<image.bgra.size();i+=4) {
            bool selected=false;
            for(const auto& c:f.colors) if(Matches(image.bgra.data()+i,c)) {selected=true;break;}
            if(f.background) selected=!selected;
            const auto value=selected?0:255;
            image.bgra[i]=image.bgra[i+1]=image.bgra[i+2]=static_cast<unsigned char>(value);
        }
    } else if(dictionary) {
        // 无颜色条件的点阵识别：用 Otsu 阈值分割，再由边缘多数颜色推断背景明暗。
        std::array<uint64_t,256> histogram{};uint64_t total=uint64_t(image.width)*image.height;
        for(size_t i=0;i<image.bgra.size();i+=4)
            ++histogram[(image.bgra[i+2]*299+image.bgra[i+1]*587+image.bgra[i]*114)/1000];
        double sum=0;for(int i=0;i<256;++i) sum+=double(i)*histogram[i];
        uint64_t lowCount=0;double lowSum=0,best=-1;int threshold=127;
        for(int t=0;t<255;++t) {
            lowCount+=histogram[t];lowSum+=double(t)*histogram[t];
            if(!lowCount||lowCount==total) continue;
            const double d=lowSum/lowCount-(sum-lowSum)/(total-lowCount);
            const double variance=double(lowCount)*(total-lowCount)*d*d;
            if(variance>best) {best=variance;threshold=t;}
        }
        uint64_t light=0,border=0;
        for(int y=0;y<image.height;++y) for(int x=0;x<image.width;++x)
            if(x==0||y==0||x==image.width-1||y==image.height-1) {
                auto p=image.bgra.data()+(size_t(y)*image.width+x)*4;
                light+=(p[2]*299+p[1]*587+p[0]*114)/1000>threshold;++border;
            }
        const bool brightBackground=light*2>=border;
        for(size_t i=0;i<image.bgra.size();i+=4) {
            const bool bright=(image.bgra[i+2]*299+image.bgra[i+1]*587+image.bgra[i]*114)/1000>threshold;
            const unsigned char value=bright==brightBackground?255:0;
            image.bgra[i]=image.bgra[i+1]=image.bgra[i+2]=value;
        }
    }
}
Image ReadFile(const char* filename) {
    const auto path=FsResolvePath(filename);
    if(path.empty()) throw std::runtime_error("图片路径无效");
    struct Apartment {
        HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ~Apartment(){if(SUCCEEDED(hr)) CoUninitialize();}
    } apartment;
    if(FAILED(apartment.hr)&&apartment.hr!=RPC_E_CHANGED_MODE) throw std::runtime_error("图片解码 COM 初始化失败");
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if(SUCCEEDED(hr)) hr=factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder);
    if(SUCCEEDED(hr)) hr=decoder->GetFrame(0,&frame);
    UINT w{},h{};
    if(SUCCEEDED(hr)) hr=frame->GetSize(&w,&h);
    if(FAILED(hr)||!w||!h||uint64_t(w)*h>FSDM_ZUIDA_ZIJIE/4) throw std::runtime_error("图片解码失败或超过16777216像素");
    Image out;out.width=int(w);out.height=int(h);out.bgra.resize(size_t(w)*h*4);
    hr=factory->CreateFormatConverter(&converter);
    if(SUCCEEDED(hr)) hr=converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
    if(SUCCEEDED(hr)) hr=converter->CopyPixels(nullptr,w*4,UINT(out.bgra.size()),out.bgra.data());
    if(FAILED(hr)) throw std::runtime_error("转换图片到 BGRA 失败");
    return out;
}
bool Acquire(const Request& r,Image& out,FsZhen& frame) {
    haoshi::Timer timer(haoshi::Phase::acquire);
    ocrFrameReused=false;
    if(r.file) {
        auto all=ReadFile(r.file);
        if(r.right>all.width||r.bottom>all.height) {Error(FS_CANSHU_CUOWU,"OCR 区域超过文件图像范围");return false;}
        out.width=r.right-r.left;out.height=r.bottom-r.top;out.bgra.resize(size_t(out.width)*out.height*4);
        for(int y=0;y<out.height;++y) std::copy_n(all.bgra.data()+(size_t(y+r.top)*all.width+r.left)*4,
            size_t(out.width)*4,out.bgra.data()+size_t(y)*out.width*4);
        return true;
    }
    FsZhuangtai state{};state.size=sizeof(state);
    if(GetStatus(&state)!=FS_CHENGGONG||!state.bound) {Error(FS_WEIBANGDING,"窗口 OCR 需要先绑定窗口");return false;}
    const uint64_t binding=state.bindingVersion;
    zhenhuancun::Snapshot snapshot;
    const int code=zhenhuancun::Read(snapshot);
    if(code!=FS_CHENGGONG) {Error(code,"读取 BGRA 画面失败");return false;}
    ocrFrameReused=snapshot.reused;
    frame=snapshot.info;
    if(!snapshot.pixels) {Error(FS_CAIJI_SHIBAI,"读取 BGRA 画面失败");return false;}
    if(frame.bindingVersion!=binding) {Error(FS_WEIBANGDING,"取帧期间绑定已经改变");return false;}
    if(!frame.width||!frame.height||!frame.clientWidth||!frame.clientHeight||
       r.right>int(frame.clientWidth)||r.bottom>int(frame.clientHeight)) {
        Error(FS_CANSHU_CUOWU,"OCR 区域超过窗口客户区");return false;
    }
    out.width=r.right-r.left;out.height=r.bottom-r.top;
    if(uint64_t(out.width)*out.height>FSDM_ZUIDA_ZIJIE/4) {Error(FS_CANSHU_CUOWU,"客户区 OCR 区域过大");return false;}
    out.bgra.resize(size_t(out.width)*out.height*4);
    // 统一为客户区物理像素，避免渲染分辨率与客户区不同导致找字返回错位。
    for(int y=0;y<out.height;++y) {
        const uint32_t sy=uint32_t(uint64_t(y+r.top)*frame.height/frame.clientHeight);
        for(int x=0;x<out.width;++x) {
            const uint32_t sx=uint32_t(uint64_t(x+r.left)*frame.width/frame.clientWidth);
            std::copy_n(snapshot.pixels->data()+size_t(sy)*frame.stride+size_t(sx)*4,4,
                out.bgra.data()+(size_t(y)*out.width+x)*4);
        }
    }
    diagnostic.frameNumber=frame.frameNumber;diagnostic.bindingVersion=frame.bindingVersion;
    return true;
}
bool SameBinding(const FsZhen& frame) {
    if(!frame.bindingVersion) return true; // 文件 OCR 不属于窗口绑定。
    FsZhuangtai state{};state.size=sizeof(state);
    if(GetStatus(&state)==FS_CHENGGONG&&state.bound&&state.bindingVersion==frame.bindingVersion) return true;
    Error(FS_WEIBANGDING,"识别期间窗口已解绑、销毁或重新绑定，已丢弃本次结果");return false;
}
struct Recognition {Lines lines;std::string separator;FsZhen frame{};};
bool Recognize(const Request& r,Recognition& out,const std::vector<std::string>* targets=nullptr) {
    haoshi::Reset(haoshi::Operation::ocr);diagnostic={};
    if(r.left<0||r.top<0||r.right<=r.left||r.bottom<=r.top||
       !std::isfinite(r.sim)||r.sim<.1||r.sim>1) {Error(FS_CANSHU_CUOWU,"区域无效，或相似度不在0.1～1.0");return false;}
    const auto selected=Selected();
    try {
        Filter filter;
        {haoshi::Timer timer(haoshi::Phase::preprocess);filter=ParseFilter(r.color);}
        Image image;
        if(!Acquire(r,image,out.frame)) return false;
        Preprocess(image,filter,selected.engine!=0);
        const bool ok=selected.engine?RecognizeDictionary(image,selected.dict,r.sim,out.lines,targets):
            RecognizeModel(image,selected.model,selected.backend,r.sim,out.lines);
        if(!ok) return false;
        {
            haoshi::Timer timer(haoshi::Phase::result);
            SortLines(out.lines);
            if(targets&&!selected.engine) {
                // 神经模型仍识别整个区域，只在每条原始文字行内部移除无关字符。
                // 不重新合并过滤后的行，避免把上下两行的文字拼成一次命中。
                for(auto& line:out.lines) std::erase_if(line.chars,[&](const Character& c) {
                    return std::none_of(targets->begin(),targets->end(),[&](const auto& word) {
                        return word.find(c.text)!=std::string::npos;
                    });
                });
            }
            for(auto& line:out.lines) for(auto& c:line.chars) {c.x+=r.left;c.right+=r.left;c.y+=r.top;c.bottom+=r.top;}
            out.separator=std::move(filter.separator);
        }
        if(!r.file&&!SameBinding(out.frame)) {out.lines.clear();return false;}
        Success();return true;
    } catch(const std::exception& e) {Error(FS_CANSHU_CUOWU,e.what());return false;}
}
std::string Text(const Lines& lines,const std::string& separator) {
    std::string text;bool first=true;
    for(const auto& line:lines) {
        if(!first) text+=separator;first=false;
        for(const auto& c:line.chars) text+=c.text;
    }
    return text;
}
// 逐字符坐标直接来自识别结果，不生成“文字$x$y”之类的中间字符串。
const FsTongyongJieguo* OcrResult(const char* name,const Request& request) {
    SaveTiming saved;std::vector<fsdm::jieguo::Item> items;std::string text;
    haoshi::Reset(haoshi::Operation::ocr);diagnostic={};ocrFrameReused=false;ocrResultReused=false;
    try {
        std::shared_lock configurationGuard(ocrConfigurationLock);
        const std::string cacheKey=MakeOcrKey(name,request,Selected());
        FsZhen currentFrame{};
        if(!request.file&&ReadCurrentOcrFrame(currentFrame)&&
            ReuseOcrResult(cacheKey,currentFrame,items,text)) {
            if(!SameBinding(currentFrame)) {items.clear();text.clear();}
            return fsdm::jieguo::Save(name,diagnostic.result,0,items,text);
        }
        Recognition recognition;
        if(Recognize(request,recognition)) {
            haoshi::Timer timer(haoshi::Phase::result);
            text=Text(recognition.lines,recognition.separator);
            for(const auto& line:recognition.lines) for(const auto& c:line.chars) {
                fsdm::jieguo::Item item;item.fields=FS_JG_ID|FS_JG_XY|FS_JG_TEXT;
                item.id=int(items.size());item.x=c.x;item.y=c.y;item.text=c.text;
                items.push_back(std::move(item));
            }
            if(!request.file&&!SameBinding(recognition.frame)) {items.clear();text.clear();}
            if(!request.file&&diagnostic.result==FS_CHENGGONG)
                RememberOcrResult(cacheKey,recognition.frame,items,text);
        }
    } catch(const std::exception& e) {Error(FS_CAIJI_SHIBAI,e.what());}
    catch(...) {Error(FS_CAIJI_SHIBAI,"OCR结果整理失败");}
    return fsdm::jieguo::Save(name,diagnostic.result,0,items,text);
}
struct Match {int id,x,y;std::string text;};
std::vector<Match> Find(const Request& request,const char* targets,bool all,bool fast,FsZhen& source) {
    if(!targets||!*targets||std::string_view(targets).size()>65536||Wide(targets).empty()) {
        haoshi::Reset(haoshi::Operation::ocr);diagnostic={};
        Error(FS_CANSHU_CUOWU,"待查文字为空、过长或 UTF-8 无效");return {};
    }
    const auto words=Split(targets,'|');
    if(std::any_of(words.begin(),words.end(),[](const auto& word){return word.empty();})) {
        haoshi::Reset(haoshi::Operation::ocr);diagnostic={};
        Error(FS_CANSHU_CUOWU,"待查文字列表含空项");return {};
    }
    Recognition recognition;if(!Recognize(request,recognition,fast?&words:nullptr)) return {};
    source=recognition.frame;
    haoshi::Timer timer(haoshi::Phase::result);std::vector<Match> matches;
    // 按目标列表优先，每行独立查找；字节位置只用于 UTF-8 子串定位，坐标来自对应字符框。
    for(size_t id=0;id<words.size();++id) for(const auto& line:recognition.lines) {
        std::string text;std::vector<size_t> offsets;
        for(const auto& c:line.chars) {offsets.push_back(text.size());text+=c.text;}
        for(size_t begin=0;(begin=text.find(words[id],begin))!=text.npos;++begin) {
            const auto at=std::lower_bound(offsets.begin(),offsets.end(),begin);
            if(at==offsets.end()||*at!=begin) continue;
            const auto end=begin+words[id].size();
            if(end!=text.size()&&!std::binary_search(offsets.begin(),offsets.end(),end)) continue;
            const auto& c=line.chars[size_t(at-offsets.begin())];
            matches.push_back({int(id),c.x,c.y,words[id]});
            if(!all||matches.size()>=1500) {
                if(!SameBinding(recognition.frame)) return {};
                return matches;
            }
        }
    }
    if(!SameBinding(recognition.frame)) return {};
    return matches;
}
const FsTongyongJieguo* FindResult(const char* name,const Request& request,const char* targets,bool all,bool fast) {
    SaveTiming saved;std::vector<fsdm::jieguo::Item> items;FsZhen source{};
    haoshi::Reset(haoshi::Operation::ocr);diagnostic={};ocrFrameReused=false;ocrResultReused=false;
    try {
        std::shared_lock configurationGuard(ocrConfigurationLock);
        std::string cacheKey=MakeOcrKey(name,request,Selected());
        AppendOcrKey(cacheKey,all);AppendOcrKey(cacheKey,fast);AppendOcrText(cacheKey,targets);
        FsZhen currentFrame{};std::string unusedText;
        if(ReadCurrentOcrFrame(currentFrame)&&
            ReuseOcrResult(cacheKey,currentFrame,items,unusedText)) {
            if(!SameBinding(currentFrame)) items.clear();
            return fsdm::jieguo::Save(name,diagnostic.result,0,items);
        }
        const auto matches=Find(request,targets,all,fast,source);
        haoshi::Timer timer(haoshi::Phase::result);
        for(const auto& hit:matches) {
            fsdm::jieguo::Item item;item.fields=FS_JG_ID|FS_JG_XY|FS_JG_TEXT;
            item.id=hit.id;item.x=hit.x;item.y=hit.y;item.text=hit.text;
            items.push_back(std::move(item));
        }
        // 数组整理期间也可能发生解绑，发布前再次检查产生这些坐标的绑定版本。
        if(diagnostic.result==FS_CHENGGONG) {
            if(SameBinding(source)) RememberOcrResult(cacheKey,source,items,{});
            else items.clear();
        }
    } catch(const std::exception& e) {Error(FS_CAIJI_SHIBAI,e.what());}
    catch(...) {Error(FS_CAIJI_SHIBAI,"找字结果整理失败");}
    return fsdm::jieguo::Save(name,diagnostic.result,0,items);
}

}
std::wstring Wide(const std::string& text) {
    if(text.empty()) return {};
    const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(n<=0) return {};
    std::wstring out(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n);return out;
}
std::string Utf8(const std::wstring& text) {
    if(text.empty()) return {};
    const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if(n<=0) return {};
    std::string out(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),out.data(),n,nullptr,nullptr);return out;
}
std::filesystem::path ModuleDirectory() {
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDirectory),&module)) throw std::runtime_error("获取 OCR 模块地址失败");
    std::wstring filename(32768,0);
    const auto n=GetModuleFileNameW(module,filename.data(),DWORD(filename.size()));
    if(!n||n>=filename.size()) throw std::runtime_error("获取 OCR 模块目录失败");
    filename.resize(n);return std::filesystem::path(filename).parent_path();
}
void Error(int code,const std::string& message) {
    diagnostic.result=code;
    // 在完整 UTF-8 字符边界截断诊断，避免工作台转换失败。
    size_t n=std::min(message.size(),sizeof(diagnostic.message)-1);
    while(n<message.size()&&n>0&&(static_cast<unsigned char>(message[n])&0xc0)==0x80) --n;
    std::copy_n(message.data(),n,diagnostic.message);diagnostic.message[n]=0;
}
void Success() {diagnostic.result=FS_CHENGGONG;diagnostic.message[0]=0;}
Selection Selected() {auto& c=Config();std::lock_guard guard(c.lock);return c.selection;}
void SortLines(Lines& lines) {
    lines.erase(std::remove_if(lines.begin(),lines.end(),[](const auto& l){return l.chars.empty();}),lines.end());
    for(auto& line:lines) std::stable_sort(line.chars.begin(),line.chars.end(),[](const auto& a,const auto& b){return a.x<b.x;});
    std::stable_sort(lines.begin(),lines.end(),[](const auto& a,const auto& b){
        return a.chars.front().y!=b.chars.front().y?a.chars.front().y<b.chars.front().y:a.chars.front().x<b.chars.front().x;
    });
    Lines merged;
    for(auto& line:lines) {
        auto found=std::find_if(merged.begin(),merged.end(),[&](const Line& row) {
            const auto& a=row.chars.front();const auto& b=line.chars.front();
            const int overlap=std::min(a.bottom,b.bottom)-std::max(a.y,b.y);
            const int height=std::min(a.bottom-a.y,b.bottom-b.y);
            const int gap=b.x-row.chars.back().right;
            return overlap*2>=height&&gap<=std::max(a.bottom-a.y,b.bottom-b.y)*3;
        });
        if(found==merged.end()) merged.push_back(std::move(line));
        else found->chars.insert(found->chars.end(),std::make_move_iterator(line.chars.begin()),std::make_move_iterator(line.chars.end()));
    }
    for(auto& line:merged) std::stable_sort(line.chars.begin(),line.chars.end(),[](const auto& a,const auto& b){return a.x<b.x;});
    lines=std::move(merged);
}
}
namespace fsdm::wenzi {
bool OcrFrameReused() noexcept { return ocrFrameReused; }
bool OcrResultReused() noexcept { return ocrResultReused; }
}
using namespace fsdm::wenzi;
int WINAPI SetOcrBackend(const char* backend) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!backend||(strcmp(backend,"cpu")&&strcmp(backend,"directml"))) {Error(FS_CANSHU_CUOWU,"OCR 后端仅支持 cpu/directml");return 0;}
    auto& c=Config();{std::lock_guard guard(c.lock);c.selection.backend=strcmp(backend,"cpu")?1:0;}
    BumpOcrConfig();
    Success();return 1;
}
int WINAPI SetOcrEngine(const char* engine) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!engine||(strcmp(engine,"model")&&strcmp(engine,"dict"))) {Error(FS_CANSHU_CUOWU,"OCR 引擎仅支持 model/dict");return 0;}
    auto& c=Config();{std::lock_guard guard(c.lock);c.selection.engine=strcmp(engine,"model")?1:0;}
    BumpOcrConfig();
    Success();return 1;
}
int WINAPI SetOcrModel(int index,const char* directory) {
    try {
        std::unique_lock configurationGuard(ocrConfigurationLock);
        if(!directory) {Error(FS_CANSHU_CUOWU,"模型目录为空");return 0;}
        if(!SetModel(index,FsResolvePath(directory))) return 0;
        BumpOcrConfig();return 1;
    }
    catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return 0;}
}
int WINAPI SetOcrModelFiles(int index,const char* classifier,const char* detector,
    const char* recognizer,const char* dictionary) {
    try {
        std::unique_lock configurationGuard(ocrConfigurationLock);
        if(!detector||!recognizer||!dictionary) {
            Error(FS_CANSHU_CUOWU,"检测模型、识别模型和字符字典路径不能为空");return 0;
        }
        const auto classifierPath=classifier&&*classifier?FsResolvePath(classifier):std::filesystem::path{};
        if(!SetModelFiles(index,classifierPath,FsResolvePath(detector),
            FsResolvePath(recognizer),FsResolvePath(dictionary))) return 0;
        BumpOcrConfig();return 1;
    }
    catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return 0;}
}
int WINAPI UseOcrModel(int index) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!UseModel(index)) return 0;
    auto& c=Config();{std::lock_guard guard(c.lock);c.selection.model=index;}BumpOcrConfig();return 1;
}
int WINAPI FreeOcrModel(int index) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!FreeModel(index)) return 0;BumpOcrConfig();return 1;
}
int WINAPI SetDict(int index,const char* file) {
    try {
        std::unique_lock configurationGuard(ocrConfigurationLock);
        if(!file) {Error(FS_CANSHU_CUOWU,"字库路径为空");return 0;}
        if(!SetDictionary(index,FsResolvePath(file))) return 0;
        auto& c=Config();{std::lock_guard guard(c.lock);c.selection.dict=index;}BumpOcrConfig();return 1;
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return 0;}
}
int WINAPI SetDictMemory(int index,uintptr_t address,int size) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!SetDictionaryMemory(index,address,size)) return 0;
    auto& c=Config();{std::lock_guard guard(c.lock);c.selection.dict=index;}BumpOcrConfig();return 1;
}
int WINAPI UseDict(int index) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!HasDictionary(index)) return 0;
    auto& c=Config();{std::lock_guard guard(c.lock);c.selection.dict=index;}BumpOcrConfig();return 1;
}
int WINAPI GetNowDict(void) {Success();return Selected().dict;}
int WINAPI GetDictCount(int index) {return DictionaryCount(index);}
int WINAPI ClearDict(int index) {
    std::unique_lock configurationGuard(ocrConfigurationLock);
    if(!ClearDictionary(index)) return 0;BumpOcrConfig();return 1;
}
int WINAPI GetOcrStatus(FsOcrZhuangtai* status) {
    if(!status||status->size!=sizeof(*status)) return FS_CANSHU_CUOWU;
    *status=diagnostic;status->size=sizeof(*status);status->version=1;const auto selection=Selected();
    status->backend=selection.backend;status->engine=selection.engine;
    status->modelIndex=selection.model;status->dictIndex=selection.dict;
    ModelStatistics(status->sessionsCreated,status->devicesCreated,status->loadedSessions);
    const auto& t=fsdm::haoshi::current;
    if(t.operation==fsdm::haoshi::Operation::ocr) {
        status->initializationMs=t.initialize.ms;status->acquireMs=t.acquire.ms;status->preprocessMs=t.preprocess.ms;
        status->detectionMs=t.detect.ms;status->recognitionMs=t.recognize.ms;status->resultMs=t.result.ms;
    }
    return FS_CHENGGONG;
}
const char* WINAPI GetOcrError(void) {return fsdm::zifuchuan::Save(fsdm::zifuchuan::Function::GetOcrError,diagnostic.message);}
const FsTongyongJieguo* WINAPI Ocr(int x1,int y1,int x2,int y2,const char* color,double sim) {
    return OcrResult("Ocr",{x1,y1,x2,y2,color,sim});
}
const FsTongyongJieguo* WINAPI OcrEx(int x1,int y1,int x2,int y2,const char* color,double sim) {
    return OcrResult("OcrEx",{x1,y1,x2,y2,color,sim});
}
const FsTongyongJieguo* WINAPI OcrExOne(int x1,int y1,int x2,int y2,const char* color,double sim) {
    return OcrResult("OcrExOne",{x1,y1,x2,y2,color,sim});
}
const FsTongyongJieguo* WINAPI OcrInFile(int x1,int y1,int x2,int y2,const char* file,const char* color,double sim) {
    if(!file||!*file) {
        fsdm::haoshi::Reset(fsdm::haoshi::Operation::ocr);diagnostic={};
        Error(FS_CANSHU_CUOWU,"文件路径为空");
        return fsdm::jieguo::Save("OcrInFile",diagnostic.result,0);
    }
    return OcrResult("OcrInFile",{x1,y1,x2,y2,color,sim,file});
}
const FsTongyongJieguo* WINAPI FindStr(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStr",{x1,y1,x2,y2,color,sim},text,false,false);
}
const FsTongyongJieguo* WINAPI FindStrS(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrS",{x1,y1,x2,y2,color,sim},text,false,false);
}
const FsTongyongJieguo* WINAPI FindStrEx(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrEx",{x1,y1,x2,y2,color,sim},text,true,false);
}
const FsTongyongJieguo* WINAPI FindStrExS(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrExS",{x1,y1,x2,y2,color,sim},text,true,false);
}
const FsTongyongJieguo* WINAPI FindStrFast(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrFast",{x1,y1,x2,y2,color,sim},text,false,true);
}
const FsTongyongJieguo* WINAPI FindStrFastS(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrFastS",{x1,y1,x2,y2,color,sim},text,false,true);
}
const FsTongyongJieguo* WINAPI FindStrFastEx(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrFastEx",{x1,y1,x2,y2,color,sim},text,true,true);
}
const FsTongyongJieguo* WINAPI FindStrFastExS(int x1,int y1,int x2,int y2,const char* text,const char* color,double sim) {
    return FindResult("FindStrFastExS",{x1,y1,x2,y2,color,sim},text,true,true);
}
