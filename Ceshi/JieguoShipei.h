// 仅供旧算法回归断言使用：先验证真实结构体，再把字段整理为旧断言的比较值。
// 生产 DLL 和工作台不包含此文件，也不把坐标序列化后再解析。
// 这样保留已有方向、透明色、容差和字库测试，同时单独覆盖新的 C 结构体契约。
#pragma once
#include "../ZhaoTu/ZhaoTu.h"
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace fsdm::ceshi {
enum class Kind { color, compare, index, multi, pictureS, indexEx, fileEx,
    fullText, textS, textEx, ocrEx, ocrExOne };
inline const char* Retain(std::string value) {
    // deque 插入不会使其他条目的 c_str 失效；只用于保留旧测试的比较值。
    thread_local std::deque<std::string> values;
    values.push_back(std::move(value));return values.back().c_str();
}
inline void Validate(const FsTongyongJieguo* r) {
    if(!r || r->size!=sizeof(*r) || r->version!=1 || !r->fullText ||
        (r->count==0)!=(r->items==nullptr) ||
        (r->status!=FS_CHENGGONG && (r->count || *r->fullText)))
        throw std::runtime_error("通用结果布局、空结果或错误契约不正确");
    for(uint32_t i=0;i<r->count;++i) {
        const auto& v=r->items[i];
        if(!v.text || !v.file || (v.fields&~31u) ||
            ((v.fields&FS_JG_XY) && (v.x<0 || v.y<0)) ||
            ((v.fields&FS_JG_COLOR) && v.color>0xffffffu))
            throw std::runtime_error("通用结果条目字段不正确");
    }
}
template<class Function,Kind kind,bool coordinates=false,bool nullable=false>
struct Adapter {
    Function native{};
    decltype(&::FreeResult) release{};
    Adapter()=default;
    Adapter(Function f,decltype(release) free):native(f),release(free) {}
    void Load(FARPROC f,FARPROC free) {
        native=reinterpret_cast<Function>(f);
        release=reinterpret_cast<decltype(release)>(free);
        if(!native || !release) throw std::runtime_error("通用结果导出不存在");
    }
    explicit operator bool() const {return native && release;}
    template<class Tuple,size_t... I>
    const FsTongyongJieguo* Invoke(Tuple& args,std::index_sequence<I...>) const {
        return native(std::get<I>(args)...);
    }
    template<class... Args> auto operator()(Args... args) const {
        constexpr bool integer=kind==Kind::index || kind==Kind::multi || kind==Kind::compare;
        auto input=std::make_tuple(args...);
        int* x=nullptr;int* y=nullptr;
        if constexpr(coordinates) {
            x=std::get<sizeof...(Args)-2>(input);y=std::get<sizeof...(Args)-1>(input);
            if(x) *x=-1;if(y) *y=-1;
            if constexpr(!nullable) {
                // 旧断言对输出指针的检查仅在测试适配层保留；新接口没有这两个参数。
                if(!x || !y) {
                    if constexpr(integer) return kind==Kind::multi?0:-1;
                    else return Retain("");
                }
            }
        }
        const auto* r=Invoke(input,std::make_index_sequence<sizeof...(Args)-(coordinates?2:0)>{});
        struct Owner {
            const FsTongyongJieguo* r;
            decltype(release) free;
            ~Owner() {if(r) free(r);}
        } owner{r,release};
        Validate(r);
        const bool found=r->status==FS_CHENGGONG && r->count;
        if constexpr(coordinates) {
            if(found) {if(x) *x=r->items[0].x;if(y) *y=r->items[0].y;}
        }
        if constexpr(integer) {
            if constexpr(kind==Kind::compare) return found?0:1;
            else if constexpr(kind==Kind::multi) return found?1:0;
            else return found?r->items[0].id:-1;
        } else {
            std::string value;
            if(r->status==FS_CHENGGONG) {
                if constexpr(kind==Kind::color) {
                    if(found) {char hex[7];sprintf_s(hex,"%06x",r->items[0].color);value=hex;}
                } else if constexpr(kind==Kind::fullText || kind==Kind::ocrExOne) value=r->fullText;
                if constexpr(kind==Kind::textS) {if(found) value=r->items[0].text;}
                if constexpr(kind==Kind::pictureS || kind==Kind::indexEx || kind==Kind::fileEx ||
                    kind==Kind::textEx || kind==Kind::ocrEx || kind==Kind::ocrExOne) {
                    for(uint32_t i=0;i<r->count;++i) {
                        const auto& v=r->items[i];
                        if(i || kind==Kind::ocrExOne) value+='|';
                        if constexpr(kind==Kind::ocrEx) value+=std::string(v.text)+"$";
                        else if constexpr(kind==Kind::pictureS || kind==Kind::fileEx) value+=std::string(v.file)+",";
                        else if constexpr(kind==Kind::textEx) value+=std::string(v.text)+",";
                        else if constexpr(kind==Kind::indexEx) value+=std::to_string(v.id)+",";
                        value+=std::to_string(v.x)+(kind==Kind::ocrEx?"$":",")+std::to_string(v.y);
                    }
                }
            }
            return Retain(std::move(value));
        }
    }
};
}
