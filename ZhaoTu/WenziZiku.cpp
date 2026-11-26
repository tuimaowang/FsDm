// 未加密 TSPlug 字库：十六进制位流按列排列，每列固定 11 位，最高位对应顶部。
// 参考源码的 comparaStr/GetStringInfo 是空实现；这里实现有边界检查的完整二值字形匹配。
#include "WenziZiku.h"
#include "Haoshi.h"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <windows.h>

namespace fsdm::wenzi {
struct Dictionary {
    struct Glyph {std::string text;int width{},height{},sourceHeight{},points{};std::vector<uint16_t> columns;};
    std::vector<Glyph> glyphs;
};
namespace {
struct Registry {std::mutex lock;std::shared_ptr<const Dictionary> slots[100];};
Registry& Store() {static auto* r=new Registry;return *r;}
bool Valid(int index) {
    if(index>=0&&index<100) return true;
    Error(FS_CANSHU_CUOWU,"字库索引必须是 0～99");return false;
}
int Hex(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='A'&&c<='F') return c-'A'+10;
    if(c>='a'&&c<='f') return c-'a'+10;
    return -1;
}
std::string DecodeText(const std::string& text) {
    if(!text.empty()&&!Wide(text).empty()) return text;
    // 旧 TSPlug 字库通常是本地中文编码；固定 GB18030 而不是跟随运行机器的 ANSI 代码页。
    const int n=MultiByteToWideChar(54936,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if(n<=0) throw std::runtime_error("字库文字既不是 UTF-8，也不是 GB18030");
    std::wstring w(n,0);MultiByteToWideChar(54936,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),w.data(),n);
    return Utf8(w);
}
}
namespace {
bool SetDictionaryData(int index,const char* data,size_t size) {
    if(!Valid(index)) return false;
    try {
        if(!data||size==0||size>16u*1024u*1024u)
            throw std::runtime_error("字库数据必须在 1 字节～16 MiB 之间");
        std::istringstream input(std::string(data,size));
        auto dict=std::make_shared<Dictionary>();std::string line;int lineNumber=0;
        while(std::getline(input,line)) {
            ++lineNumber;
            if(lineNumber==1&&line.starts_with("\xef\xbb\xbf")) line.erase(0,3);
            if(!line.empty()&&line.back()=='\r') line.pop_back();
            if(line.empty()) continue;
            if(line.size()>32768) throw std::runtime_error("字库单行过长");
            const auto first=line.find('$'),second=first==line.npos?line.npos:line.find('$',first+1),last=line.rfind('$');
            if(first==line.npos||second==line.npos||first==0||second==first+1)
                throw std::runtime_error("字库第 "+std::to_string(lineNumber)+" 行格式无效；只支持未加密十六进制点阵文本");
            Dictionary::Glyph g;g.text=DecodeText(line.substr(first+1,second-first-1));
            const auto parsed=std::from_chars(line.data()+last+1,line.data()+line.size(),g.sourceHeight);
            if(parsed.ec!=std::errc{}||parsed.ptr!=line.data()+line.size()||g.sourceHeight<1||g.sourceHeight>255)
                throw std::runtime_error("字库高度字段必须是1～255之间的整数");
            // TSPlug 的高度字段与点阵列宽度是两套信息：每列始终只有11个位。
            // 高度超过11时保留源字段用于兼容加载，但识别几何按可表达的11行处理。
            g.height=std::min(g.sourceHeight,11);
            g.width=int(first*4/11);
            if(g.width<1||g.width>256||first!=(size_t(g.width)*11+3)/4)
                throw std::runtime_error("字库点阵宽度或填充位不兼容");
            std::vector<unsigned char> bits;bits.reserve(first*4);
            for(size_t i=0;i<first;++i) {
                const int n=Hex(line[i]);if(n<0) throw std::runtime_error("字库位流包含非十六进制字符");
                for(int k=3;k>=0;--k) bits.push_back((n>>k)&1);
            }
            for(size_t i=size_t(g.width)*11;i<bits.size();++i)
                if(bits[i]) throw std::runtime_error("字库末尾填充位必须为零");
            g.columns.resize(g.width);
            for(int x=0;x<g.width;++x) for(int y=0;y<11;++y) {
                if(y>=g.sourceHeight&&bits[size_t(x)*11+y])
                    throw std::runtime_error("字库实际高度与点阵中有效笔画不一致");
                if(bits[size_t(x)*11+y]) {g.columns[x]|=uint16_t(1u<<(10-y));++g.points;}
            }
            if(!g.points) throw std::runtime_error("字库不允许全空白字形");
            dict->glyphs.push_back(std::move(g));
            if(dict->glyphs.size()>10000) throw std::runtime_error("单字库最多支持10000个字形");
        }
        if(dict->glyphs.empty()) throw std::runtime_error("字库为空");
        auto& r=Store();{std::lock_guard guard(r.lock);r.slots[index]=std::move(dict);}
        Success();return true;
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
}
bool SetDictionary(int index,const std::filesystem::path& path) {
    if(!Valid(index)) return false;
    try {
        std::ifstream input(path,std::ios::binary);
        if(!input) throw std::runtime_error("无法打开字库文件");
        input.seekg(0,std::ios::end);
        const auto length=input.tellg();
        if(length<=0||length>16*1024*1024)
            throw std::runtime_error("字库文件必须在 1 字节～16 MiB 之间");
        input.seekg(0);
        std::string data(static_cast<size_t>(length),'\0');
        if(!input.read(data.data(),static_cast<std::streamsize>(data.size())))
            throw std::runtime_error("读取字库文件失败");
        return SetDictionaryData(index,data.data(),data.size());
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
bool SetDictionaryMemory(int index,uintptr_t address,int size) {
    if(!Valid(index)) return false;
    if(!address||size<=0||size>16*1024*1024||
        static_cast<uintptr_t>(size)>UINTPTR_MAX-address) {
        Error(FS_CANSHU_CUOWU,"字库内存地址或长度无效，数据长度须为 1 字节～16 MiB");
        return false;
    }
    try {
        std::string data(static_cast<size_t>(size),'\0');
        SIZE_T copied{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),
            data.data(),data.size(),&copied)||copied!=data.size()) {
            Error(FS_CANSHU_CUOWU,"无法读取指定地址的完整字库数据");
            return false;
        }
        return SetDictionaryData(index,data.data(),data.size());
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
bool HasDictionary(int index) {
    if(!Valid(index)) return false;
    auto& r=Store();std::lock_guard guard(r.lock);
    if(!r.slots[index]) {Error(FS_JIAZAI_SHIBAI,"该字库尚未设置或已清空");return false;}
    Success();return true;
}
int DictionaryCount(int index) {
    if(!Valid(index)) return 0;
    auto& r=Store();std::lock_guard guard(r.lock);Success();
    return r.slots[index]?int(r.slots[index]->glyphs.size()):0;
}
bool ClearDictionary(int index) {
    if(!Valid(index)) return false;
    auto& r=Store();{std::lock_guard guard(r.lock);r.slots[index].reset();}
    Success();return true;
}
bool RecognizeDictionary(const Image& image,int index,double sim,Lines& lines,const std::vector<std::string>* targets) {
    if(!Valid(index)) return false;
    std::shared_ptr<const Dictionary> dict;
    {auto& r=Store();std::lock_guard guard(r.lock);dict=r.slots[index];}
    if(!dict) {Error(FS_JIAZAI_SHIBAI,"请先 SetDict 加载字库");return false;}
    haoshi::Timer timer(haoshi::Phase::recognize);
    const int w=image.width,h=image.height;
    // 每个调用线程复用自己的临时内存，不共享图像内容，也不增加采集设备或模型会话。
    // 首列索引每帧重建，不能把上一张画面的候选位置当作本次的结果。
    struct Workspace {
        std::vector<uint16_t> columns;
        std::vector<uint32_t> next;
        std::vector<unsigned char> occupied;
    };
    thread_local Workspace workspace;
    auto& columns=workspace.columns;columns.resize(size_t(w)*h);
    // 输入由公共预处理统一成白底黑字。滑动列位值，避免每个模板重复读取整张图。
    for(int x=0;x<w;++x) {
        uint16_t v=0;
        for(int y=h-1;y>=0;--y) {
            const auto pos=size_t(y)*w+x;
            v=uint16_t((v>>1)|(image.bgra[pos*4]<128?1024:0));columns[pos]=v;
        }
    }
    struct Hit {Character c;int points;size_t glyphOrder;};
    std::vector<Hit> hits;
    auto match=[&](size_t order,int x,int y,uint16_t mask,int allowed) {
        const auto& glyph=dict->glyphs[order];int mismatch=0;
        for(int dx=0;dx<glyph.width;++dx) {
            mismatch+=std::popcount(unsigned((columns[size_t(y)*w+x+dx]&mask)^glyph.columns[dx]));
            if(mismatch>allowed) return true;
        }
        const float score=1.f-float(mismatch)/glyph.points;
        hits.push_back({{glyph.text,x,y,x+glyph.width,y+glyph.height,score},glyph.points,order});
        if(hits.size()>100000) {Error(FS_ZIYUAN_MANG,"字库候选过多，请缩小区域或提高相似度");return false;}
        return true;
    };
    auto scan=[&](size_t order) {
        const auto& glyph=dict->glyphs[order];
        const uint16_t mask=uint16_t((2047u<<(11-glyph.height))&2047u);
        const int allowed=int(std::floor((1-sim)*glyph.points+1e-9));
        for(int y=0;y<=h-glyph.height;++y) for(int x=0;x<=w-glyph.width;++x)
            if(!match(order,x,y,mask,allowed)) return false;
        return true;
    };
    if(!targets) {
        // 普通识别继续遍历全部字形，保留已有 FindStr/Ocr 的字库竞争与识别规则。
        for(size_t i=0;i<dict->glyphs.size();++i) {
            const auto& g=dict->glyphs[i];
            if(g.width<=w&&g.height<=h&&!scan(i)) return false;
        }
    } else {
        std::array<std::vector<size_t>,12> selected;
        for(size_t i=0;i<dict->glyphs.size();++i) {
            const auto& g=dict->glyphs[i];
            // 每个备选目标独立判断，不能拼接 "张|三" 后错误地选中复合字形 "张三"。
            if(g.width<=w&&g.height<=h&&std::any_of(targets->begin(),targets->end(),
                [&](const auto& word){return word.find(g.text)!=std::string::npos;}))
                selected[g.height].push_back(i);
        }
        constexpr uint32_t none=UINT32_MAX;
        for(int height=1;height<=11;++height) {
            if(selected[height].empty()) continue;
            if(sim<1) {
                // 模糊匹配允许首列也有误差，不能套用首列完全相等的索引而漏掉候选。
                // 仍只扫描目标字形，并在累计误差超过上限时立即放弃当前位置。
                for(const auto i:selected[height]) if(!scan(i)) return false;
                continue;
            }
            // 按实际高度截取首列；低字形下方的其他笔画不应影响该字形的匹配。
            // 同一高度只建一次 2048 个桶，每个桶用数组链表保存从上到下、从左到右的位置。
            const uint16_t mask=uint16_t((2047u<<(11-height))&2047u);
            std::array<uint32_t,2048> heads,tails;heads.fill(none);tails.fill(none);
            auto& next=workspace.next;next.resize(size_t(w)*h);
            for(int y=0;y<=h-height;++y) for(int x=0;x<w;++x) {
                const auto pos=uint32_t(size_t(y)*w+x);const auto key=columns[pos]&mask;
                next[pos]=none;
                if(heads[key]==none) heads[key]=pos;
                else next[tails[key]]=pos;
                tails[key]=pos;
            }
            for(const auto i:selected[height]) {
                const auto& g=dict->glyphs[i];
                for(auto pos=heads[g.columns[0]];pos!=none;pos=next[pos]) {
                    const int x=int(pos%w),y=int(pos/w);
                    if(x<=w-g.width&&!match(i,x,y,mask,0)) return false;
                }
            }
        }
    }
    // 同一区域保留高分、笔画多的字形。高度分组会改变遍历顺序，因此显式保留原字库优先级。
    std::stable_sort(hits.begin(),hits.end(),[](const Hit& a,const Hit& b){
        if(a.c.confidence!=b.c.confidence) return a.c.confidence>b.c.confidence;
        if(a.points!=b.points) return a.points>b.points;
        return a.glyphOrder<b.glyphOrder;
    });
    auto& occupied=workspace.occupied;occupied.assign(size_t(w)*h,0);Line accepted;
    for(auto& hit:hits) {
        const auto& c=hit.c;bool overlap=false;
        for(int y=c.y;y<c.bottom&&!overlap;++y) for(int x=c.x;x<c.right;++x)
            if(occupied[size_t(y)*w+x]) {overlap=true;break;}
        if(overlap) continue;
        for(int y=c.y;y<c.bottom;++y) for(int x=c.x;x<c.right;++x) occupied[size_t(y)*w+x]=1;
        accepted.chars.push_back(std::move(hit.c));
        if(accepted.chars.size()>=10000) break;
    }
    // 将独立字形按纵向重叠归到行，公共排序随后负责行内从左到右。
    std::sort(accepted.chars.begin(),accepted.chars.end(),[](const Character& a,const Character& b){return a.y<b.y;});
    for(auto& c:accepted.chars) {
        auto found=std::find_if(lines.begin(),lines.end(),[&](const Line& l) {
            const auto& anchor=l.chars.front();
            return std::min(c.bottom,anchor.bottom)-std::max(c.y,anchor.y)>=std::min(c.bottom-c.y,anchor.bottom-anchor.y)/2+1;
        });
        if(found==lines.end()) lines.push_back(Line{{std::move(c)}});
        else found->chars.push_back(std::move(c));
    }
    return true;
}
}
