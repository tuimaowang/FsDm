// PP-OCRv5 mobile 与 PP-OCRv3：复用 ONNX 会话、输入内存和 DirectML 设备。
// 预处理及输出整理在 CPU；DirectML 的 Run 返回包含模型执行等待和结果传回，不是提交耗时。
#include "WenziMoxing.h"
#include "Haoshi.h"
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <numeric>
#include <queue>
#include <sstream>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace fsdm::wenzi {
namespace {
std::atomic<uint64_t> sessionsCreated{}, devicesCreated{};
std::atomic<uint32_t> loadedSessions{};
std::string Trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    if(first==std::string::npos) return {};
    return s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
int Hex(char c) {
    if(c>='0'&&c<='9') return c-'0';
    if(c>='a'&&c<='f') return c-'a'+10;
    if(c>='A'&&c<='F') return c-'A'+10;
    return -1;
}
// 仅解析当前 PP-OCR 配置中的 YAML 标量；不把反斜杠、单引号转义误算成新的类别。
std::string Scalar(std::string s) {
    s=Trim(s);
    if(s.size()<2) return s;
    if(s.front()=='\''&&s.back()=='\'') {
        s=s.substr(1,s.size()-2);
        for(size_t p=0;(p=s.find("''",p))!=std::string::npos;++p) s.replace(p,2,"'");
        return s;
    }
    if(s.front()!='"'||s.back()!='"') return s;
    std::string out;
    for(size_t i=1;i+1<s.size();++i) {
        char c=s[i];
        if(c!='\\') {out+=c;continue;}
        if(++i+1>=s.size()) throw std::runtime_error("字符表转义不完整");
        c=s[i];
        if(c=='n') out+='\n';
        else if(c=='r') out+='\r';
        else if(c=='t') out+='\t';
        else if(c=='"'||c=='\\'||c=='/') out+=c;
        else if(c=='u'||c=='U'||c=='x') {
            const int digits=c=='U'?8:c=='u'?4:2;
            uint32_t cp=0;
            for(int j=0;j<digits;++j) {
                if(++i+1>=s.size()||Hex(s[i])<0) throw std::runtime_error("字符表 Unicode 转义无效");
                cp=cp*16+Hex(s[i]);
            }
            if(cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) throw std::runtime_error("字符表码点无效");
            std::wstring w;
            if(cp>=0x10000) {cp-=0x10000;w+=wchar_t(0xd800+(cp>>10));w+=wchar_t(0xdc00+(cp&1023));}
            else w+=wchar_t(cp);
            out+=Utf8(w);
        } else if(c==' ') out+=' ';
        else throw std::runtime_error("不支持的字符表转义");
    }
    return out;
}
std::string ReadText(const std::filesystem::path& p) {
    std::ifstream in(p,std::ios::binary);
    if(!in) throw std::runtime_error("无法读取模型配置文件");
    in.seekg(0,std::ios::end);
    const auto n=in.tellg();
    if(n<=0||n>4*1024*1024) throw std::runtime_error("模型配置文件大小无效");
    std::string s(static_cast<size_t>(n),0);in.seekg(0);in.read(s.data(),n);
    if(!in) throw std::runtime_error("模型配置读取不完整");
    return s;
}
struct ModelConfig {
    std::filesystem::path path;
    std::filesystem::path detectorPath, recognizerPath, classifierPath;
    std::wstring identity;
    std::vector<std::string> chars;
};
std::vector<std::string> ReadCharacterFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法读取 OCR 字符表");
    std::vector<std::string> chars;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (chars.empty() && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf)
            line.erase(0, 3);
        if (line.empty() || Wide(line).empty())
            throw std::runtime_error("OCR 字符表包含空行或无效 UTF-8");
        chars.push_back(std::move(line));
        if (chars.size() > 100000) throw std::runtime_error("OCR 字符表类别数量超过限制");
    }
    if (!input.eof() || chars.empty()) throw std::runtime_error("OCR 字符表为空或读取失败");
    return chars;
}
ModelConfig Validate(const std::filesystem::path& path) {
    if(path.empty()) throw std::runtime_error("模型目录为空");
    ModelConfig cfg;
    cfg.path=std::filesystem::weakly_canonical(path);
    cfg.identity=cfg.path.native();
    const auto v5Det=path/L"PP-OCRv5_mobile_det/model.onnx";
    const auto v5Rec=path/L"PP-OCRv5_mobile_rec/model.onnx";
    if(std::filesystem::is_regular_file(v5Det)&&std::filesystem::is_regular_file(v5Rec)) {
        const auto det=ReadText(path/L"PP-OCRv5_mobile_det/inference.yml");
        const auto rec=ReadText(path/L"PP-OCRv5_mobile_rec/inference.yml");
        if(det.find("PP-OCRv5_mobile_det")==std::string::npos||det.find("DBPostProcess")==std::string::npos||
           rec.find("PP-OCRv5_mobile_rec")==std::string::npos||rec.find("CTCLabelDecode")==std::string::npos||
           rec.find("img_mode: BGR")==std::string::npos||det.find("img_mode: BGR")==std::string::npos)
            throw std::runtime_error("PP-OCRv5 配置不兼容：需要 BGR 检测和 CTC 识别");
        cfg.detectorPath=v5Det;
        cfg.recognizerPath=v5Rec;
        std::istringstream input(rec);std::string line;bool reading=false;
        while(std::getline(input,line)) {
            auto s=Trim(line);
            if(!reading) {if(s=="character_dict:") reading=true;continue;}
            if(s.empty()||s.front()!='-') break;
            auto ch=Scalar(s.substr(1));
            if(ch.empty()||Wide(ch).empty()) throw std::runtime_error("字符表包含空项或无效 UTF-8");
            cfg.chars.push_back(std::move(ch));
        }
    } else {
        // 用户常见 PP-OCRv3 原始文件名与 RapidOCR 重命名文件名共用同一套推理流程。
        const auto detA=path/L"ch_PP-OCRv3_det_infer.onnx";
        const auto recA=path/L"ch_PP-OCRv3_rec_chs.onnx";
        const auto clsA=path/L"ch_ppocr_mobile_v2.0_cls_infer.onnx";
        const auto keysA=path/L"ppocr_keys_chs.txt";
        const auto detB=path/L"detCN.onnx";
        const auto recB=path/L"RecCN.onnx";
        const auto clsB=path/L"cls.onnx";
        const auto keysB=path/L"KeysCN.txt";
        if(std::filesystem::is_regular_file(detA)&&std::filesystem::is_regular_file(recA)&&
           std::filesystem::is_regular_file(clsA)&&std::filesystem::is_regular_file(keysA)) {
            cfg.detectorPath=detA;cfg.recognizerPath=recA;cfg.classifierPath=clsA;
            cfg.chars=ReadCharacterFile(keysA);
        } else if(std::filesystem::is_regular_file(detB)&&std::filesystem::is_regular_file(recB)&&
            std::filesystem::is_regular_file(clsB)&&std::filesystem::is_regular_file(keysB)) {
            cfg.detectorPath=detB;cfg.recognizerPath=recB;cfg.classifierPath=clsB;
            cfg.chars=ReadCharacterFile(keysB);
        } else {
            throw std::runtime_error("模型目录不符合 PP-OCRv5 或支持的 PP-OCRv3 中文模型布局");
        }
    }
    if(cfg.chars.empty()||cfg.chars.size()>100000) throw std::runtime_error("字符表类别数量无效");
    // PP-OCR CTC 字典的序号从 1 开始；序号 0 留给 blank，末尾空格对应训练字典约定。
    cfg.chars.emplace_back(" ");
    return cfg;
}
// 按调用方给出的文件路径登记 PP-OCRv3 兼容组件；推理仍使用本项目实现的固定前后处理。
ModelConfig ValidateFiles(const std::filesystem::path& classifier,
    const std::filesystem::path& detector,const std::filesystem::path& recognizer,
    const std::filesystem::path& dictionary) {
    ModelConfig cfg;
    cfg.detectorPath=std::filesystem::canonical(detector);
    cfg.recognizerPath=std::filesystem::canonical(recognizer);
    cfg.classifierPath=classifier.empty()?std::filesystem::path{}:std::filesystem::canonical(classifier);
    const auto dictPath=std::filesystem::canonical(dictionary);
    if(!std::filesystem::is_regular_file(cfg.detectorPath)||
       !std::filesystem::is_regular_file(cfg.recognizerPath)||
       (!cfg.classifierPath.empty()&&!std::filesystem::is_regular_file(cfg.classifierPath))||
       !std::filesystem::is_regular_file(dictPath))
        throw std::runtime_error("OCR 模型或字符字典路径不是有效文件");
    cfg.chars=ReadCharacterFile(dictPath);
    if(cfg.chars.size()>100000) throw std::runtime_error("字符表类别数量超过限制");
    cfg.chars.emplace_back(" ");
    // 文件组合共同构成模型身份，路径相同的重复登记可以复用现有会话。
    cfg.identity=cfg.detectorPath.native()+L"\n"+cfg.recognizerPath.native()+L"\n"+
        cfg.classifierPath.native()+L"\n"+dictPath.native();
    return cfg;
}
struct Runtime {
    std::mutex lock;
    HMODULE module{}, dmlModule{};
    std::unique_ptr<Ort::Env> env;
    ComPtr<ID3D12Device> device;
    ComPtr<IDMLDevice> dmlDevice;
    ComPtr<ID3D12CommandQueue> queue;
    const OrtDmlApi* dmlApi{};
    // 进程固定加载：不在 DllMain 下调用 ORT 析构或等待 GPU，模型由 FreeOcrModel 显式释放。
    void Initialize(bool directml) {
        std::lock_guard guard(lock);
        if(!env) {
            const auto path=ModuleDirectory()/L"onnxruntime.dll";
            if(!module) module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module) throw std::runtime_error("加载 onnxruntime.dll 失败，Win32="+std::to_string(GetLastError()));
            auto get=reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(GetProcAddress(module,"OrtGetApiBase"));
            const auto* api=get?get()->GetApi(ORT_API_VERSION):nullptr;
            if(!api) throw std::runtime_error("ONNX Runtime C API 版本不匹配");
            Ort::InitApi(api);
            env=std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR,"FsDmOCR");
        }
        if(!directml||dmlDevice) return;
        if(!dmlModule) dmlModule=LoadLibraryExW((ModuleDirectory()/L"DirectML.dll").c_str(),nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!dmlModule) throw std::runtime_error("加载 DirectML.dll 失败，Win32="+std::to_string(GetLastError()));
        auto create=reinterpret_cast<HRESULT(WINAPI*)(ID3D12Device*,DML_CREATE_DEVICE_FLAGS,REFIID,void**)>(
            GetProcAddress(dmlModule,"DMLCreateDevice"));
        if(!create) throw std::runtime_error("DirectML 缺少 DMLCreateDevice");
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if(FAILED(hr)) throw std::runtime_error("创建 OCR DXGI 工厂失败");
        ComPtr<ID3D12Device> newDevice;
        for(UINT i=0;factory->EnumAdapters1(i,&adapter)!=DXGI_ERROR_NOT_FOUND;++i) {
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
            if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&newDevice));
            if(SUCCEEDED(hr)) break;
        }
        if(!newDevice) throw std::runtime_error("没有支持 DirectML 的硬件 D3D12 适配器");
        ComPtr<IDMLDevice> newDml;
        hr=create(newDevice.Get(),DML_CREATE_DEVICE_FLAG_NONE,IID_PPV_ARGS(&newDml));
        if(FAILED(hr)) throw std::runtime_error("创建 DirectML 设备失败，HRESULT="+std::to_string(hr));
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> newQueue;
        hr=newDevice->CreateCommandQueue(&desc,IID_PPV_ARGS(&newQueue));
        if(FAILED(hr)) throw std::runtime_error("创建 OCR GPU 队列失败");
        Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML",ORT_API_VERSION,reinterpret_cast<const void**>(&dmlApi)));
        device=std::move(newDevice);dmlDevice=std::move(newDml);queue=std::move(newQueue);++devicesCreated;
    }
};
Runtime& RunTime() {static auto* value=new Runtime;return *value;}
struct Sessions {
    Ort::Session det{nullptr}, rec{nullptr}, cls{nullptr};
    Ort::MemoryInfo memory=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    std::string detIn,detOut,recIn,recOut,clsIn,clsOut;
    std::vector<float> tensor;
    uint32_t sessionCount{};
    int recognizerHeight{};
    bool counted{};
    ~Sessions() {if(counted) loadedSessions.fetch_sub(sessionCount);}
};
struct Model {
    ModelConfig config;
    std::mutex lock;
    std::unique_ptr<Sessions> sessions[2];
};
struct Models {
    std::mutex lock;
    std::shared_ptr<Model> slots[100];
    bool defaultRegistered{};
};
Models& Registry() {static auto* value=new Models;return *value;}
std::shared_ptr<Model> GetModel(int index) {
    if(index<0||index>=100) throw std::runtime_error("模型索引必须是 0～99");
    auto& r=Registry();std::lock_guard guard(r.lock);
    if(index==0&&!r.slots[0]&&!r.defaultRegistered) {
        haoshi::Timer timer(haoshi::Phase::initialize);
        auto model=std::make_shared<Model>();
        model->config=Validate(ModuleDirectory()/L"ocr_models");
        r.slots[0]=model;r.defaultRegistered=true;
    }
    if(!r.slots[index]) throw std::runtime_error("该模型索引尚未设置或已经释放");
    return r.slots[index];
}
std::vector<int64_t> InputShape(Ort::Session& s) {
    if(s.GetInputCount()!=1||s.GetOutputCount()!=1) throw std::runtime_error("模型输入输出数量不兼容");
    const auto type=s.GetInputTypeInfo(0);
    auto info=type.GetTensorTypeAndShapeInfo();
    if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) throw std::runtime_error("模型输入必须为 float32");
    return info.GetShape();
}
std::unique_ptr<Sessions> CreateSessions(Model& model,int backend) {
    RunTime().Initialize(backend!=0);
    auto& rt=RunTime();
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    // 避免多个会话各自占满 CPU 核心；模型内串行执行，也符合 DirectML 的会话要求。
    options.SetIntraOpNumThreads(std::clamp<int>(static_cast<int>(std::thread::hardware_concurrency()/2),1,4));
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    if(backend) {
        options.DisableMemPattern();
        Ort::ThrowOnError(rt.dmlApi->SessionOptionsAppendExecutionProvider_DML1(options,rt.dmlDevice.Get(),rt.queue.Get()));
    }
    auto s=std::make_unique<Sessions>();
    s->det=Ort::Session(*rt.env,model.config.detectorPath.c_str(),options);
    s->rec=Ort::Session(*rt.env,model.config.recognizerPath.c_str(),options);
    const auto ds=InputShape(s->det),rs=InputShape(s->rec);
    if(ds.size()!=4||rs.size()!=4||ds[1]!=3||rs[1]!=3||
       (rs[2]>0&&rs[2]!=32&&rs[2]!=48)||
       ds[2]>0||ds[3]>0||rs[3]>0)
        throw std::runtime_error("需要动态宽高检测模型及高度32或48、动态宽度的识别模型");
    s->recognizerHeight=rs[2]>0?static_cast<int>(rs[2]):48;
    const auto out=s->rec.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if(out.size()!=3||(out[2]>0&&out[2]!=static_cast<int64_t>(model.config.chars.size()+1)))
        throw std::runtime_error("识别模型输出类别与字符表不一致");
    Ort::AllocatorWithDefaultOptions alloc;
    s->detIn=s->det.GetInputNameAllocated(0,alloc).get();s->detOut=s->det.GetOutputNameAllocated(0,alloc).get();
    s->recIn=s->rec.GetInputNameAllocated(0,alloc).get();s->recOut=s->rec.GetOutputNameAllocated(0,alloc).get();
    s->sessionCount=2;
    if(!model.config.classifierPath.empty()) {
        s->cls=Ort::Session(*rt.env,model.config.classifierPath.c_str(),options);
        const auto cs=InputShape(s->cls);
        if(cs.size()!=4||cs[1]!=3||(cs[2]>0&&cs[2]!=48)||(cs[3]>0&&cs[3]!=192))
            throw std::runtime_error("PP-OCRv3 方向分类模型输入必须为 3x48x192");
        const auto clsOut=s->cls.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if(clsOut.size()!=2||(clsOut[0]>0&&clsOut[0]!=1)||clsOut[1]!=2)
            throw std::runtime_error("PP-OCRv3 方向分类模型输出必须为 [N,2]");
        s->clsIn=s->cls.GetInputNameAllocated(0,alloc).get();
        s->clsOut=s->cls.GetOutputNameAllocated(0,alloc).get();
        ++s->sessionCount;
    }
    sessionsCreated+=s->sessionCount;loadedSessions+=s->sessionCount;s->counted=true;
    return s;
}
enum class ModelPart {detector,recognizer,classifier};
Ort::Value Run(Sessions& s,ModelPart part,const std::array<int64_t,4>& shape) {
    auto input=Ort::Value::CreateTensor<float>(s.memory,s.tensor.data(),s.tensor.size(),shape.data(),shape.size());
    Ort::Session* session=part==ModelPart::detector?&s.det:
        part==ModelPart::recognizer?&s.rec:&s.cls;
    const char* in=part==ModelPart::detector?s.detIn.c_str():
        part==ModelPart::recognizer?s.recIn.c_str():s.clsIn.c_str();
    const char* out=part==ModelPart::detector?s.detOut.c_str():
        part==ModelPart::recognizer?s.recOut.c_str():s.clsOut.c_str();
    Ort::Value output{nullptr};
    session->Run(Ort::RunOptions{nullptr},&in,&input,1,&out,&output,1);
    if(!output.IsTensor()||output.GetTensorTypeAndShapeInfo().GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error("模型输出必须为 float32 张量");
    return output;
}
std::array<float,3> Sample(const Image& img,float x,float y) {
    x=std::clamp(x,0.f,float(img.width-1));y=std::clamp(y,0.f,float(img.height-1));
    const int x0=int(x),y0=int(y),x1=std::min(x0+1,img.width-1),y1=std::min(y0+1,img.height-1);
    const float dx=x-x0,dy=y-y0;
    std::array<float,3> c{};
    for(int k=0;k<3;++k) {
        const float a=img.bgra[(size_t(y0)*img.width+x0)*4+k],b=img.bgra[(size_t(y0)*img.width+x1)*4+k];
        const float d=img.bgra[(size_t(y1)*img.width+x0)*4+k],e=img.bgra[(size_t(y1)*img.width+x1)*4+k];
        c[k]=(a+(b-a)*dx)*(1-dy)+(d+(e-d)*dx)*dy;
    }
    return c;
}
struct Box {int left,top,right,bottom;};
// 参考 DB 概率图处理：阈值、八邻域连通区域、轴对齐补边。
// PP-OCRv3 的方向分类另行纠正 180 度倒置文字；斜切框仍按轴对齐区域识别。
std::vector<Box> Boxes(const Ort::Value& value,int width,int height) {
    const auto info=value.GetTensorTypeAndShapeInfo();const auto shape=info.GetShape();
    if(shape.size()!=4||shape[0]!=1||shape[1]!=1||shape[2]<=0||shape[3]<=0||shape[2]*shape[3]>4096*4096)
        throw std::runtime_error("检测模型输出尺寸不兼容");
    const int h=int(shape[2]),w=int(shape[3]);
    const float* p=value.GetTensorData<float>();
    std::vector<unsigned char> visited(size_t(w)*h);
    std::vector<int> pending;std::vector<Box> boxes;
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        const int seed=y*w+x;
        if(visited[seed]||!std::isfinite(p[seed])||p[seed]<.3f) continue;
        pending.clear();pending.push_back(seed);visited[seed]=1;
        int left=x,right=x,top=y,bottom=y;double sum=0;
        for(size_t i=0;i<pending.size();++i) {
            const int at=pending[i],cx=at%w,cy=at/w;
            left=std::min(left,cx);right=std::max(right,cx);top=std::min(top,cy);bottom=std::max(bottom,cy);sum+=p[at];
            for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                const int nx=cx+dx,ny=cy+dy;
                if(nx<0||ny<0||nx>=w||ny>=h) continue;
                const int n=ny*w+nx;
                if(!visited[n]&&std::isfinite(p[n])&&p[n]>=.3f) {visited[n]=1;pending.push_back(n);}
            }
        }
        if(pending.size()<3||sum/pending.size()<.6) continue;
        int l=left*width/w,t=top*height/h,r=((right+1)*width+w-1)/w,b=((bottom+1)*height+h-1)/h;
        const int margin=std::max(2,int(std::ceil((b-t)*.18)));
        boxes.push_back({std::max(0,l-std::max(2,margin/2)),std::max(0,t-margin),
            std::min(width,r+std::max(2,margin/2)),std::min(height,b+margin)});
        if(boxes.size()>=1000) return boxes;
    }
    return boxes;
}
Line Decode(const Ort::Value& output,const ModelConfig& cfg,const Box& b,int padded,int content,double sim) {
    const auto shape=output.GetTensorTypeAndShapeInfo().GetShape();
    if(shape.size()!=3||shape[0]!=1||shape[1]<=0||shape[2]!=static_cast<int64_t>(cfg.chars.size()+1))
        throw std::runtime_error("CTC 输出或字符表类别不匹配");
    const int steps=int(shape[1]),classes=int(shape[2]);
    const float* p=output.GetTensorData<float>();
    struct Token {int id,start,end;float score;};
    std::vector<Token> tokens;int previous=-1;
    for(int t=0;t<steps;++t) {
        const float* row=p+size_t(t)*classes;
        const int best=int(std::max_element(row,row+classes)-row);
        if(!std::isfinite(row[best])) throw std::runtime_error("识别模型输出包含非有限值");
        if(best&&best!=previous) tokens.push_back({best,t,t+1,row[best]});
        else if(best&&best==previous&&!tokens.empty()) tokens.back().end=t+1;
        previous=best;
    }
    Line line;double confidence=0;
    for(const auto& t:tokens) confidence+=t.score;
    if(tokens.empty()||confidence/tokens.size()<sim) return line;
    // CTC 发射区间之间的中点作为字符边界，保留实际预测位置，避免均分导致长短字符错位。
    const double scale=double(b.right-b.left)*padded/(steps*content);
    for(size_t i=0;i<tokens.size();++i) {
        const auto& t=tokens[i];
        const double l=i?(tokens[i-1].end+t.start)*.5:std::max(0.,t.start-.5);
        const double r=i+1<tokens.size()?(t.end+tokens[i+1].start)*.5:std::min(double(steps),t.end+.5);
        line.chars.push_back({cfg.chars[t.id-1],std::clamp(b.left+int(l*scale),b.left,b.right-1),b.top,
            std::clamp(b.left+int(std::ceil(r*scale)),b.left+1,b.right),b.bottom,t.score});
    }
    return line;
}
bool ClassifyUpsideDown(Sessions& s,const Image& image,const Box& box) {
    constexpr int width=192,height=48;
    const int content=std::clamp(int(std::ceil(double(box.right-box.left)*height/
        (box.bottom-box.top))),8,width);
    const size_t plane=size_t(width)*height;
    s.tensor.assign(plane*3,0.0f);
    for(int y=0;y<height;++y) for(int x=0;x<content;++x) {
        const auto color=Sample(image,box.left+(x+.5f)*(box.right-box.left)/content-.5f,
            box.top+(y+.5f)*(box.bottom-box.top)/height-.5f);
        const size_t at=size_t(y)*width+x;
        for(int channel=0;channel<3;++channel)
            s.tensor[size_t(channel)*plane+at]=color[channel]/127.5f-1.0f;
    }
    const auto output=Run(s,ModelPart::classifier,{1,3,height,width});
    const auto info=output.GetTensorTypeAndShapeInfo();
    const auto shape=info.GetShape();
    if(shape.size()!=2||shape[0]!=1||shape[1]!=2)
        throw std::runtime_error("PP-OCRv3 方向分类模型运行时输出不是 [1,2]");
    const auto* scores=output.GetTensorData<float>();
    if(!std::isfinite(scores[0])||!std::isfinite(scores[1]))
        throw std::runtime_error("PP-OCRv3 方向分类结果包含非有限值");
    const int angle=scores[1]>scores[0]?1:0;
    return angle==1&&scores[1]>=.9f;
}
}
bool SetModel(int index,const std::filesystem::path& path) {
    try {
        if(index<0||index>=100) throw std::runtime_error("模型索引必须是 0～99");
        auto config=Validate(path);auto& r=Registry();std::lock_guard guard(r.lock);
        if(!r.slots[index]||r.slots[index]->config.identity!=config.identity) {
            auto m=std::make_shared<Model>();m->config=std::move(config);r.slots[index]=std::move(m);
        }
        if(index==0) r.defaultRegistered=true;
        Success();return true;
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
bool SetModelFiles(int index,const std::filesystem::path& classifier,
    const std::filesystem::path& detector,const std::filesystem::path& recognizer,
    const std::filesystem::path& dictionary) {
    try {
        if(index<0||index>=100) throw std::runtime_error("模型索引必须是 0～99");
        auto config=ValidateFiles(classifier,detector,recognizer,dictionary);
        auto& r=Registry();std::lock_guard guard(r.lock);
        if(!r.slots[index]||r.slots[index]->config.identity!=config.identity) {
            auto model=std::make_shared<Model>();model->config=std::move(config);
            r.slots[index]=std::move(model);
        }
        if(index==0) r.defaultRegistered=true;
        Success();return true;
    } catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
bool UseModel(int index) {
    try {GetModel(index);Success();return true;}
    catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
bool FreeModel(int index) {
    if(index<0||index>=100) {Error(FS_CANSHU_CUOWU,"模型索引必须是 0～99");return false;}
    auto& r=Registry();std::shared_ptr<Model> old;
    {std::lock_guard guard(r.lock);old=std::move(r.slots[index]);if(index==0) r.defaultRegistered=true;}
    // 并发识别持有 shared_ptr，释放索引不会销毁正在运行的会话。
    old.reset();Success();return true;
}
bool RecognizeModel(const Image& image,int index,int backend,double sim,Lines& lines) {
    try {
        auto model=GetModel(index);
        std::unique_lock guard(model->lock); // 同模型请求串行，保护输入内存和 ORT 会话。
        if(!model->sessions[backend]) {
            haoshi::Timer timer(haoshi::Phase::initialize);
            model->sessions[backend]=CreateSessions(*model,backend);
        }
        auto& s=*model->sessions[backend];int w,h;
        {
            haoshi::Timer timer(haoshi::Phase::preprocess);
            const int longest=std::max(image.width,image.height);
            const double scale=longest<640?std::min(2.,960./longest):std::min(1.,960./longest);
            w=std::max(32,int(std::ceil(image.width*scale/32))*32);
            h=std::max(32,int(std::ceil(image.height*scale/32))*32);
            const size_t plane=size_t(w)*h;s.tensor.resize(plane*3);
            constexpr float mean[3]{.485f,.456f,.406f},dev[3]{.229f,.224f,.225f};
            for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
                auto c=Sample(image,(x+.5f)*image.width/w-.5f,(y+.5f)*image.height/h-.5f);
                for(int k=0;k<3;++k) s.tensor[k*plane+size_t(y)*w+x]=(c[k]/255-mean[k])/dev[k];
            }
        }
        std::vector<Box> boxes;
        {haoshi::Timer timer(haoshi::Phase::detect);auto out=Run(s,ModelPart::detector,{1,3,h,w});boxes=Boxes(out,image.width,image.height);}
        for(const auto& b:boxes) {
            bool rotate=false;
            if(s.cls) {
                haoshi::Timer timer(haoshi::Phase::recognize);
                rotate=ClassifyUpsideDown(s,image,b);
            }
            int content,padded;
            {
                haoshi::Timer timer(haoshi::Phase::preprocess);
                const int recHeight=s.recognizerHeight;
                content=std::clamp(int(std::ceil(double(b.right-b.left)*recHeight/(b.bottom-b.top))),8,3200);
                padded=std::max(320,((content+31)/32)*32);
                const size_t plane=size_t(padded)*recHeight;s.tensor.assign(plane*3,0);
                for(int y=0;y<recHeight;++y) for(int x=0;x<content;++x) {
                    const float sourceX=rotate?
                        float(b.right-(x+.5)*(b.right-b.left)/content-.5):
                        float(b.left+(x+.5)*(b.right-b.left)/content-.5);
                    const float sourceY=rotate?
                        float(b.bottom-(y+.5)*(b.bottom-b.top)/48-.5):
                        float(b.top+(y+.5)*(b.bottom-b.top)/48-.5);
                    auto c=Sample(image,sourceX,sourceY);
                    for(int k=0;k<3;++k) s.tensor[k*plane+size_t(y)*padded+x]=c[k]/127.5f-1;
                }
            }
            haoshi::Timer timer(haoshi::Phase::recognize);
            auto out=Run(s,ModelPart::recognizer,{1,3,s.recognizerHeight,padded});auto line=Decode(out,model->config,b,padded,content,sim);
            if(!line.chars.empty()) lines.push_back(std::move(line));
        }
        return true;
    } catch(const Ort::Exception& e) {Error(FS_CAIJI_SHIBAI,std::string("ONNX 推理失败：")+e.what());return false;}
      catch(const std::exception& e) {Error(FS_JIAZAI_SHIBAI,e.what());return false;}
}
void ModelStatistics(uint64_t& sessions,uint64_t& devices,uint32_t& loaded) {
    sessions=sessionsCreated.load();devices=devicesCreated.load();loaded=loadedSessions.load();
}
}
