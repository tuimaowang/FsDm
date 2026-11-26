// YOLO v5-7.0 的 ONNX 推理、检测框整理及大漠 AI 接口名兼容入口。
// 模型使用 ONNX Runtime CPU 后端；输入预处理、非极大值抑制和 BMP 标注在 CPU 完成。
#include "ZhaoTu.h"
#include "WenziShibie.h"
#include "ZifuchuanNeicun.h"
#include "ZhenhuanCun.h"
#include "TongyongJieguo.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <list>
#include <sstream>
#include <string>
#include <thread>

namespace {
constexpr int kInputSize = 640;
constexpr size_t kMaxModelBytes = 512ull * 1024 * 1024;
constexpr size_t kMaxDetections = 1000;

struct Runtime {
    std::mutex lock;
    HMODULE module{};
    std::unique_ptr<Ort::Env> env;

    void Initialize() {
        std::lock_guard guard(lock);
        if (env) return;
        const auto path = fsdm::wenzi::ModuleDirectory() / L"onnxruntime.dll";
        module = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("加载 onnxruntime.dll 失败，Win32=" +
            std::to_string(GetLastError()));
        const auto get = reinterpret_cast<const OrtApiBase*(ORT_API_CALL*)()>(
            GetProcAddress(module, "OrtGetApiBase"));
        const auto* api = get ? get()->GetApi(ORT_API_VERSION) : nullptr;
        if (!api) throw std::runtime_error("ONNX Runtime C API 版本不匹配");
        Ort::InitApi(api);
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "FsDmYOLO");
    }
};
Runtime& OrtRuntime() {
    static auto* runtime = new Runtime;
    return *runtime;
}

// 每次重新加载都分配新编号，避免模型释放后地址被复用而命中旧模型结果。
std::atomic<uint64_t> nextModelIdentity{1};
struct Model {
    const uint64_t identity{nextModelIdentity.fetch_add(1,std::memory_order_relaxed)};
    std::mutex lock;
    std::vector<unsigned char> bytes;
    std::vector<std::string> labels;
    std::unique_ptr<Ort::Session> session;
    std::string inputName, outputName;
    int classCount{};
};
struct Models {
    std::mutex lock;
    std::shared_ptr<Model> slots[20];
    int selected{};
    std::string version;
};
Models& Registry() {
    static auto* models = new Models;
    return *models;
}

std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> ReadLabels(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("找不到模型同名 .class 类别文件");
    std::vector<std::string> labels;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (!line.empty()) labels.push_back(std::move(line));
        if (labels.size() > 10000) throw std::runtime_error(".class 类别数量超过限制");
    }
    if (labels.empty()) throw std::runtime_error(".class 类别文件为空");
    return labels;
}
void CreateSession(Model& model) {
    OrtRuntime().Initialize();
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetIntraOpNumThreads(std::clamp<int>(
        static_cast<int>(std::thread::hardware_concurrency() / 2), 1, 4));
    model.session = std::make_unique<Ort::Session>(*OrtRuntime().env,
        model.bytes.data(), model.bytes.size(), options);
    if (model.session->GetInputCount() != 1 || model.session->GetOutputCount() != 1)
        throw std::runtime_error("仅支持单输入、单输出 YOLOv5 ONNX 模型");
    auto inputInfo = model.session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
    const auto inputShape = inputInfo.GetShape();
    if (inputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        inputShape.size() != 4 || (inputShape[1] > 0 && inputShape[1] != 3) ||
        (inputShape[2] > 0 && inputShape[2] != kInputSize) ||
        (inputShape[3] > 0 && inputShape[3] != kInputSize)) {
        std::string detail;
        for (const auto dim : inputShape)
            detail += (detail.empty() ? "" : "x") + (dim > 0 ? std::to_string(dim) : "?");
        throw std::runtime_error("输入必须为 float32 NCHW、3通道、640x640；模型实际形状为 " +
            (detail.empty() ? std::string("未知") : detail) + "，元素类型编号=" +
            std::to_string(static_cast<int>(inputInfo.GetElementType())));
    }
    auto outputInfo = model.session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo();
    const auto outputShape = outputInfo.GetShape();
    if (outputInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        outputShape.size() != 3 || (outputShape[0] > 0 && outputShape[0] != 1) ||
        outputShape[2] < 6)
        throw std::runtime_error("输出必须为 [1,候选框,5+类别数] float32 YOLOv5 张量");
    model.classCount = static_cast<int>(outputShape[2] - 5);
    if (!model.labels.empty() && static_cast<int>(model.labels.size()) != model.classCount)
        throw std::runtime_error(".class 类别数量与 ONNX 输出不一致");
    if (model.labels.empty()) {
        model.labels.reserve(model.classCount);
        for (int i = 0; i < model.classCount; ++i) model.labels.push_back(std::to_string(i));
    }
    Ort::AllocatorWithDefaultOptions allocator;
    model.inputName = model.session->GetInputNameAllocated(0, allocator).get();
    model.outputName = model.session->GetOutputNameAllocated(0, allocator).get();
}
std::shared_ptr<Model> GetSelectedModel() {
    auto& registry = Registry();
    std::lock_guard guard(registry.lock);
    if (registry.version != "v5-7.0")
        throw std::runtime_error("请先调用 AiYoloSetVersion(\"v5-7.0\")");
    const int index = registry.selected;
    if (index < 0 || index >= 20 || !registry.slots[index])
        throw std::runtime_error("当前 YOLO 模型尚未加载");
    return registry.slots[index];
}

struct CapturedImage {
    FsZhen frame{};
    cv::Mat bgr;
    int clientX{}, clientY{}, clientWidth{}, clientHeight{};
};
bool CaptureRegion(int x1, int y1, int x2, int y2, CapturedImage& result) {
    if (x2 <= x1 || y2 <= y1) return false;
    fsdm::zhenhuancun::Snapshot snapshot;
    if (fsdm::zhenhuancun::Read(snapshot)!=FS_CHENGGONG || !snapshot.pixels) return false;
    const FsZhen& info=snapshot.info;
    const auto& pixels=*snapshot.pixels;
    if (!info.width || !info.height || !info.clientWidth || !info.clientHeight) return false;
    x1 = std::max(0, x1); y1 = std::max(0, y1);
    x2 = std::min<int>(x2, info.clientWidth); y2 = std::min<int>(y2, info.clientHeight);
    if (x2 <= x1 || y2 <= y1) return false;
    const int left = static_cast<int>(uint64_t(x1) * info.width / info.clientWidth);
    const int top = static_cast<int>(uint64_t(y1) * info.height / info.clientHeight);
    const int right = static_cast<int>(uint64_t(x2) * info.width / info.clientWidth);
    const int bottom = static_cast<int>(uint64_t(y2) * info.height / info.clientHeight);
    if (right <= left || bottom <= top) return false;
    cv::Mat bgra(static_cast<int>(info.height), static_cast<int>(info.width),
        CV_8UC4, const_cast<unsigned char*>(pixels.data()), info.stride);
    cv::cvtColor(bgra(cv::Rect(left, top, right - left, bottom - top)),
        result.bgr, cv::COLOR_BGRA2BGR);
    result.frame = info;
    result.clientX = x1; result.clientY = y1;
    result.clientWidth = x2 - x1; result.clientHeight = y2 - y1;
    return true;
}

struct Detection {
    int id{}, x{}, y{}, width{}, height{};
    float confidence{};
};
float IoU(const Detection& a, const Detection& b) {
    const int left = std::max(a.x, b.x), top = std::max(a.y, b.y);
    const int right = std::min(a.x + a.width, b.x + b.width);
    const int bottom = std::min(a.y + a.height, b.y + b.height);
    const int intersection = std::max(0, right - left) * std::max(0, bottom - top);
    const int areaA = a.width * a.height, areaB = b.width * b.height;
    const int unionArea = areaA + areaB - intersection;
    return unionArea > 0 ? static_cast<float>(intersection) / unionArea : 0.0f;
}
std::vector<Detection> Detect(Model& model, const CapturedImage& image, double probability, double iou) {
    std::lock_guard guard(model.lock);
    if (!model.session) CreateSession(model);
    const int originalWidth = image.bgr.cols, originalHeight = image.bgr.rows;
    const double scale = std::min(double(kInputSize) / originalWidth,
        double(kInputSize) / originalHeight);
    const int resizedWidth = std::max(1, int(std::round(originalWidth * scale)));
    const int resizedHeight = std::max(1, int(std::round(originalHeight * scale)));
    const int padX = (kInputSize - resizedWidth) / 2;
    const int padY = (kInputSize - resizedHeight) / 2;
    cv::Mat resized, inputImage(kInputSize, kInputSize, CV_8UC3, cv::Scalar(114, 114, 114));
    cv::resize(image.bgr, resized, cv::Size(resizedWidth, resizedHeight));
    resized.copyTo(inputImage(cv::Rect(padX, padY, resizedWidth, resizedHeight)));
    constexpr size_t plane = size_t(kInputSize) * kInputSize;
    std::vector<float> input(plane * 3);
    for (int y = 0; y < kInputSize; ++y) {
        const auto* row = inputImage.ptr<cv::Vec3b>(y);
        for (int x = 0; x < kInputSize; ++x) {
            const auto bgr = row[x];
            const size_t offset = size_t(y) * kInputSize + x;
            input[offset] = bgr[2] / 255.0f;
            input[plane + offset] = bgr[1] / 255.0f;
            input[plane * 2 + offset] = bgr[0] / 255.0f;
        }
    }
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::array<int64_t, 4> shape{1, 3, kInputSize, kInputSize};
    auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(),
        shape.data(), shape.size());
    const char* inputNames[]{model.inputName.c_str()};
    const char* outputNames[]{model.outputName.c_str()};
    auto outputs = model.session->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1,
        outputNames, 1);
    if (outputs.empty() || !outputs[0].IsTensor())
        throw std::runtime_error("YOLO 模型没有返回张量");
    const auto outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
    const auto outputShape = outputInfo.GetShape();
    if (outputShape.size() != 3 || outputShape[0] != 1 ||
        outputShape[2] != model.classCount + 5)
        throw std::runtime_error("YOLO 模型输出维度在运行时发生变化");
    const float* values = outputs[0].GetTensorData<float>();
    const size_t candidates = static_cast<size_t>(outputShape[1]);
    const size_t stride = static_cast<size_t>(outputShape[2]);
    std::vector<Detection> candidatesFound;
    candidatesFound.reserve((std::min)(candidates, size_t(4096)));
    const double invScaleX = double(image.clientWidth) / originalWidth;
    const double invScaleY = double(image.clientHeight) / originalHeight;
    for (size_t i = 0; i < candidates; ++i) {
        const float* item = values + i * stride;
        const float objectness = item[4];
        if (!std::isfinite(objectness) || objectness <= 0) continue;
        int classId = 0;
        float classScore = 0;
        for (int c = 0; c < model.classCount; ++c) {
            const float score = item[5 + c];
            if (std::isfinite(score) && score > classScore) {
                classScore = score; classId = c;
            }
        }
        const float confidence = objectness * classScore;
        if (confidence < probability) continue;
        const double centerX = (item[0] - padX) / scale;
        const double centerY = (item[1] - padY) / scale;
        const double width = item[2] / scale, height = item[3] / scale;
        if (!std::isfinite(centerX) || !std::isfinite(centerY) ||
            !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) continue;
        const int left = std::clamp(int(std::floor((centerX - width / 2) * invScaleX)), 0, image.clientWidth);
        const int top = std::clamp(int(std::floor((centerY - height / 2) * invScaleY)), 0, image.clientHeight);
        const int right = std::clamp(int(std::ceil((centerX + width / 2) * invScaleX)), 0, image.clientWidth);
        const int bottom = std::clamp(int(std::ceil((centerY + height / 2) * invScaleY)), 0, image.clientHeight);
        if (right <= left || bottom <= top) continue;
        candidatesFound.push_back({classId, image.clientX + left, image.clientY + top,
            right - left, bottom - top, confidence});
    }
    std::stable_sort(candidatesFound.begin(), candidatesFound.end(),
        [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
    std::vector<Detection> selected;
    for (const auto& candidate : candidatesFound) {
        bool suppressed = false;
        for (const auto& prior : selected) {
            if (candidate.id == prior.id && IoU(candidate, prior) > iou) {
                suppressed = true; break;
            }
        }
        if (!suppressed) selected.push_back(candidate);
        if (selected.size() >= kMaxDetections) break;
    }
    return selected;
}
struct DetectionKey {
    uint64_t model{};
    int x1{},y1{},x2{},y2{};
    double probability{},iou{};
    bool operator==(const DetectionKey&) const = default;
};
struct CachedDetection {
    DetectionKey key;
    CapturedImage image;
    std::vector<Detection> detections;
    size_t bytes() const {
        return sizeof(CachedDetection)+image.bgr.total()*image.bgr.elemSize()+
            detections.capacity()*sizeof(Detection);
    }
};
std::mutex detectionCacheLock;
std::list<CachedDetection> detectionCache;
size_t detectionCacheBytes{};
constexpr size_t kDetectionCacheEntries=16, kDetectionCacheBytes=32u*1024*1024;
bool SameDetectionFrame(const FsZhen& a,const FsZhen& b) {
    return a.bindingVersion&&a.bindingVersion==b.bindingVersion&&
        a.frameNumber==b.frameNumber&&a.width==b.width&&a.height==b.height&&
        a.stride==b.stride&&a.clientWidth==b.clientWidth&&a.clientHeight==b.clientHeight;
}
void ValidateDetectionBinding(const FsZhen& frame) {
    FsZhuangtai state{};state.size=sizeof(state);
    if(GetStatus(&state)!=FS_CHENGGONG||!state.bound||state.bindingVersion!=frame.bindingVersion)
        throw std::runtime_error("YOLO 检测期间窗口已解绑或重新绑定");
}
bool ReuseDetection(const DetectionKey& key,CapturedImage& image,std::vector<Detection>& detections) {
    if(!fsdm::zhenhuancun::ResultReuseEnabled()) return false;
    FsZhuangtai state{};state.size=sizeof(state);
    if(GetStatus(&state)!=FS_CHENGGONG||state.result!=FS_CHENGGONG||
        !state.bound||!state.hasFrame) return false;
    std::lock_guard guard(detectionCacheLock);
    const auto entry=std::find_if(detectionCache.begin(),detectionCache.end(),[&](const auto& cached) {
        return cached.key==key&&SameDetectionFrame(cached.image.frame,state.frame);
    });
    if(entry==detectionCache.end()) return false;
    image=entry->image;detections=entry->detections;
    detectionCache.splice(detectionCache.begin(),detectionCache,entry);
    return true;
}
void RememberDetection(const DetectionKey& key,const CapturedImage& image,
    const std::vector<Detection>& detections) noexcept {
    if(!fsdm::zhenhuancun::ResultReuseEnabled()) return;
    try {
        // 同时保留该帧的原始区域，输出标注图片时能使用与检测框对应的像素。
        // cv::Mat 共享只读像素，绘制入口先 clone，防止污染后续命中和并发调用。
        CachedDetection fresh{key,image,detections};
        const size_t bytes=fresh.bytes();if(bytes>kDetectionCacheBytes) return;
        std::lock_guard guard(detectionCacheLock);
        const auto old=std::find_if(detectionCache.begin(),detectionCache.end(),[&](const auto& cached) {
            return cached.key==key&&SameDetectionFrame(cached.image.frame,image.frame);
        });
        detectionCache.push_front(std::move(fresh));detectionCacheBytes+=bytes;
        if(old!=detectionCache.end()) {detectionCacheBytes-=old->bytes();detectionCache.erase(old);}
        while(detectionCache.size()>kDetectionCacheEntries||detectionCacheBytes>kDetectionCacheBytes) {
            detectionCacheBytes-=detectionCache.back().bytes();detectionCache.pop_back();
        }
    } catch(...) {
        // 缓存空间不足只影响复用，此次已经完成的检测仍正常返回。
    }
}
// 三个公开检测入口共用同一份完成结果。命中时只查询帧信息，跳过取图、预处理和推理。
std::vector<Detection> DetectRegion(int x1,int y1,int x2,int y2,double probability,double iou,
    CapturedImage& image) {
    if(x2<=x1||y2<=y1) throw std::runtime_error("搜索区域无效");
    const auto model=GetSelectedModel();
    const DetectionKey key{model->identity,x1,y1,x2,y2,probability,iou};
    std::vector<Detection> detections;
    if(!ReuseDetection(key,image,detections)) {
        if(!CaptureRegion(x1,y1,x2,y2,image)) throw std::runtime_error("截图失败或搜索区域无效");
        detections=Detect(*model,image,probability,iou);
        ValidateDetectionBinding(image.frame);
        RememberDetection(key,image,detections);
    }
    ValidateDetectionBinding(image.frame);
    fsdm::wenzi::Success();
    return detections;
}
std::string EncodeDetections(const std::vector<Detection>& detections) {
    std::ostringstream output;
    output.setf(std::ios::fixed);
    output.precision(6);
    for (size_t i = 0; i < detections.size(); ++i) {
        if (i) output << '|';
        const auto& d = detections[i];
        output << d.id << ',' << d.confidence << ',' << d.x << ',' << d.y << ','
            << d.width << ',' << d.height;
    }
    return output.str();
}
std::string BmpBytes(cv::Mat& image) {
    const uint32_t rowBytes = (static_cast<uint32_t>(image.cols) * 3 + 3) & ~3u;
    const uint64_t pixelBytes = uint64_t(rowBytes) * image.rows;
    if (pixelBytes > UINT32_MAX - 54) throw std::runtime_error("BMP 图像超过格式大小上限");
    std::string bytes(54 + static_cast<size_t>(pixelBytes), '\0');
    auto put16 = [&](size_t at, uint16_t value) {
        bytes[at] = char(value); bytes[at + 1] = char(value >> 8);
    };
    auto put32 = [&](size_t at, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = char(value >> (8 * i));
    };
    bytes[0] = 'B'; bytes[1] = 'M';
    put32(2, static_cast<uint32_t>(bytes.size())); put32(10, 54);
    put32(14, 40); put32(18, static_cast<uint32_t>(image.cols));
    put32(22, static_cast<uint32_t>(image.rows)); put16(26, 1); put16(28, 24);
    put32(34, static_cast<uint32_t>(pixelBytes));
    for (int y = 0; y < image.rows; ++y) {
        const auto* row = image.ptr<unsigned char>(image.rows - y - 1);
        std::memcpy(bytes.data() + 54 + size_t(y) * rowBytes, row, size_t(image.cols) * 3);
    }
    return bytes;
}
bool ValidateThresholds(double probability, double iou) {
    return std::isfinite(probability) && probability >= 0 && probability <= 1 &&
        std::isfinite(iou) && iou >= 0 && iou <= 1;
}
std::filesystem::path ResolveYoloPath(const char* file) {
    if (!file || !*file) throw std::runtime_error("模型文件路径为空");
    const auto path = FsResolvePath(file);
    if (path.empty() || path.extension() != L".onnx")
        throw std::runtime_error("首版仅支持 .onnx 模型文件");
    return path;
}
bool RegisterModel(int index, std::shared_ptr<Model> model) {
    if (index < 0 || index >= 20) throw std::runtime_error("模型索引必须为 0～19");
    auto& registry = Registry();
    std::lock_guard guard(registry.lock);
    registry.slots[index] = std::move(model);
    return true;
}
const char* SaveText(fsdm::zifuchuan::Function function, const std::string& text) {
    return fsdm::zifuchuan::Save(function, text);
}
} // namespace

int WINAPI AiYoloSetVersion(const char* version) {
    if (!version || strcmp(version, "v5-7.0")) return 0;
    auto& registry = Registry();
    std::lock_guard guard(registry.lock);
    registry.version = version;
    return 1;
}
int WINAPI AiYoloSetModel(int index, const char* file, const char* password) {
    try {
        if (index < 0 || index >= 20 || !password || *password)
            throw std::runtime_error("索引须为0～19；ONNX 模型密码必须为空");
        const auto path = ResolveYoloPath(file);
        auto model = std::make_shared<Model>();
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("无法打开 ONNX 模型文件");
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        if (length <= 0 || static_cast<uint64_t>(length) > kMaxModelBytes)
            throw std::runtime_error("ONNX 模型文件大小无效或超过 512 MiB");
        model->bytes.resize(static_cast<size_t>(length));
        input.seekg(0); input.read(reinterpret_cast<char*>(model->bytes.data()), length);
        if (!input) throw std::runtime_error("ONNX 模型读取不完整");
        auto labelsPath = path; labelsPath.replace_extension(L".class");
        model->labels = ReadLabels(labelsPath);
        CreateSession(*model);
        RegisterModel(index, std::move(model));
        fsdm::wenzi::Success();
        return 1;
    } catch (const Ort::Exception& e) {
        fsdm::wenzi::Error(FS_JIAZAI_SHIBAI, std::string("加载 YOLO ONNX 失败：") + e.what());
    } catch (const std::exception& e) {
        fsdm::wenzi::Error(FS_JIAZAI_SHIBAI, e.what());
    }
    return 0;
}
int WINAPI AiYoloSetModelMemory(int index, uintptr_t data, int size, const char* password) {
    try {
        if (index < 0 || index >= 20 || !data || size <= 0 ||
            static_cast<size_t>(size) > kMaxModelBytes || !password || *password)
            throw std::runtime_error("内存模型参数无效；仅支持不超过512 MiB且密码为空的 ONNX 数据");
        auto model = std::make_shared<Model>();
        const auto* first = reinterpret_cast<const unsigned char*>(data);
        model->bytes.assign(first, first + size);
        CreateSession(*model);
        RegisterModel(index, std::move(model));
        fsdm::wenzi::Success();
        return 1;
    } catch (const Ort::Exception& e) {
        fsdm::wenzi::Error(FS_JIAZAI_SHIBAI, std::string("加载内存 YOLO ONNX 失败：") + e.what());
    } catch (const std::exception& e) {
        fsdm::wenzi::Error(FS_JIAZAI_SHIBAI, e.what());
    }
    return 0;
}
int WINAPI AiYoloUseModel(int index) {
    if (index < 0 || index >= 20) return 0;
    auto& registry = Registry();
    std::lock_guard guard(registry.lock);
    if (!registry.slots[index]) return 0;
    registry.selected = index;
    return 1;
}
int WINAPI AiYoloFreeModel(int index) {
    if (index < 0 || index >= 20) return 0;
    auto& registry = Registry();
    std::lock_guard guard(registry.lock);
    registry.slots[index].reset();
    if (registry.selected == index) registry.selected = 0;
    return 1;
}
const char* WINAPI AiYoloDetectObjects(int x1, int y1, int x2, int y2, double prob, double iou) {
    try {
        if (!ValidateThresholds(prob, iou)) throw std::runtime_error("prob 与 iou 必须在 0～1 范围内");
        CapturedImage image;
        return SaveText(fsdm::zifuchuan::Function::AiYoloDetectObjects,
            EncodeDetections(DetectRegion(x1,y1,x2,y2,prob,iou,image)));
    } catch (const Ort::Exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, std::string("YOLO 推理失败：") + e.what());
    } catch (const std::exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, e.what());
    }
    return SaveText(fsdm::zifuchuan::Function::AiYoloDetectObjects, {});
}
int WINAPI AiYoloDetectObjectsToFile(int x1, int y1, int x2, int y2, double prob,
    double iou, const char* file, int mode) {
    try {
        if (!ValidateThresholds(prob, iou) || mode < 0 || mode > 1 || !file || !*file)
            throw std::runtime_error("参数无效：mode 仅支持0或1，文件路径不能为空");
        CapturedImage image;
        const auto detections = DetectRegion(x1,y1,x2,y2,prob,iou,image);
        image.bgr=image.bgr.clone();
        for (const auto& d : detections) {
            const int x = d.x - image.clientX, y = d.y - image.clientY;
            cv::rectangle(image.bgr, cv::Rect(x, y, d.width, d.height), cv::Scalar(0, 0, 255), 2);
            std::string label = std::to_string(d.id);
            if (mode == 0) {
                char confidence[24];
                sprintf_s(confidence, " %.2f", d.confidence);
                label += confidence;
            }
            cv::putText(image.bgr, label, cv::Point(x, std::max(12, y - 3)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
        }
        const auto bytes = BmpBytes(image.bgr);
        const auto path = FsResolvePath(file);
        if (path.empty()) throw std::runtime_error("输出路径无效");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) throw std::runtime_error("BMP 文件写入失败");
        fsdm::wenzi::Success();
        return 1;
    } catch (const Ort::Exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, std::string("YOLO 推理失败：") + e.what());
    } catch (const std::exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, e.what());
    }
    return 0;
}
int WINAPI AiYoloDetectObjectsToDataBmp(int x1, int y1, int x2, int y2, double prob,
    double iou, void* data, uint32_t* size, int mode) {
    try {
        if (!size || !ValidateThresholds(prob, iou) || mode < 0 || mode > 1)
            throw std::runtime_error("参数无效：size 不能为空且 mode 仅支持0或1");
        const uint32_t capacity = *size;
        CapturedImage image;
        const auto detections=DetectRegion(x1,y1,x2,y2,prob,iou,image);
        image.bgr=image.bgr.clone();
        for (const auto& d : detections) {
            const int x = d.x - image.clientX, y = d.y - image.clientY;
            cv::rectangle(image.bgr, cv::Rect(x, y, d.width, d.height), cv::Scalar(0, 0, 255), 2);
            std::string label = std::to_string(d.id);
            if (mode == 0) {
                char confidence[24];
                sprintf_s(confidence, " %.2f", d.confidence);
                label += confidence;
            }
            cv::putText(image.bgr, label, cv::Point(x, std::max(12, y - 3)),
                cv::FONT_HERSHEY_SIMPLEX, 0.45, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
        }
        const auto bytes = BmpBytes(image.bgr);
        if (bytes.size() > UINT32_MAX) throw std::runtime_error("BMP 数据超过32位长度范围");
        *size = static_cast<uint32_t>(bytes.size());
        if (!data || capacity < bytes.size()) return 0;
        std::memcpy(data, bytes.data(), bytes.size());
        fsdm::wenzi::Success();
        return 1;
    } catch (const Ort::Exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, std::string("YOLO 推理失败：") + e.what());
    } catch (const std::exception& e) {
        fsdm::wenzi::Error(FS_CAIJI_SHIBAI, e.what());
    }
    return 0;
}
const char* WINAPI AiYoloSortsObjects(const char* objects, int height) {
    try {
        if (!objects || height < 0) throw std::runtime_error("objects 不能为空且 height 不能小于0");
        struct Parsed { std::string source; int id{}, x{}, y{}, w{}, h{}; float score{}; };
        std::vector<Parsed> parsed;
        std::string_view text(objects);
        while (!text.empty()) {
            const auto end = text.find('|');
            const auto token = text.substr(0, end);
            std::array<std::string_view, 6> fields{};
            size_t start = 0;
            for (size_t i = 0; i < fields.size(); ++i) {
                const auto comma = token.find(',', start);
                if (i < fields.size() - 1 && comma == token.npos)
                    throw std::runtime_error("YOLO 对象必须是 class,prob,x,y,w,h");
                fields[i] = token.substr(start, comma == token.npos ? comma : comma - start);
                start = comma == token.npos ? token.size() : comma + 1;
            }
            Parsed item; item.source = std::string(token);
            auto parseInt = [](std::string_view s, int& v) {
                const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
                return r.ec == std::errc{} && r.ptr == s.data() + s.size();
            };
            const auto r = std::from_chars(fields[1].data(), fields[1].data() + fields[1].size(), item.score);
            if (!parseInt(fields[0], item.id) || r.ec != std::errc{} ||
                r.ptr != fields[1].data() + fields[1].size() || !parseInt(fields[2], item.x) ||
                !parseInt(fields[3], item.y) || !parseInt(fields[4], item.w) ||
                !parseInt(fields[5], item.h) || item.id < 0 || item.w < 0 || item.h < 0 ||
                !std::isfinite(item.score))
                throw std::runtime_error("YOLO 对象字段格式无效");
            parsed.push_back(std::move(item));
            if (end == text.npos) break;
            text.remove_prefix(end + 1);
            if (text.empty()) throw std::runtime_error("YOLO 对象列表末尾不能有分隔符");
        }
        std::stable_sort(parsed.begin(), parsed.end(), [](const Parsed& a, const Parsed& b) {
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });
        std::string sorted;
        std::vector<Parsed> row;
        int rowTop = parsed.empty() ? 0 : parsed.front().y;
        auto appendRow = [&] {
            std::stable_sort(row.begin(), row.end(),
                [](const Parsed& a, const Parsed& b) { return a.x < b.x; });
            for (const auto& item : row) {
                if (!sorted.empty()) sorted += '|';
                sorted += item.source;
            }
            row.clear();
        };
        for (const auto& item : parsed) {
            if (height > 0 && !row.empty() && item.y - rowTop >= height) {
                appendRow();
                rowTop = item.y;
            }
            row.push_back(item);
        }
        appendRow();
        return SaveText(fsdm::zifuchuan::Function::AiYoloSortsObjects, sorted);
    } catch (...) {
        return SaveText(fsdm::zifuchuan::Function::AiYoloSortsObjects, {});
    }
}
const char* WINAPI AiYoloObjectsToString(const char* objects) {
    try {
        if (!objects) throw std::runtime_error("objects 不能为空");
        const auto model = GetSelectedModel();
        std::lock_guard guard(model->lock);
        std::string output;
        std::string_view text(objects);
        while (!text.empty()) {
            const auto end = text.find('|');
            const auto token = text.substr(0, end);
            const auto comma = token.find(',');
            int id{};
            if (comma == token.npos) throw std::runtime_error("YOLO 对象格式无效");
            const auto parsed = std::from_chars(token.data(), token.data() + comma, id);
            if (parsed.ec != std::errc{} || parsed.ptr != token.data() + comma ||
                id < 0 || static_cast<size_t>(id) >= model->labels.size())
                throw std::runtime_error("YOLO 类别编号超出 .class 类别表");
            if (!output.empty()) output += '|';
            output += model->labels[id];
            if (end == text.npos) break;
            text.remove_prefix(end + 1);
        }
        return SaveText(fsdm::zifuchuan::Function::AiYoloObjectsToString, output);
    } catch (...) {
        return SaveText(fsdm::zifuchuan::Function::AiYoloObjectsToString, {});
    }
}

const FsTongyongJieguo* WINAPI AiFindPic(int x1, int y1, int x2, int y2,
    const char* pic_name, double sim, int dir) {
    return CvFindPic(x1, y1, x2, y2, pic_name, sim, dir);
}
const FsTongyongJieguo* WINAPI AiFindPicEx(int x1, int y1, int x2, int y2,
    const char* pic_name, double sim, int dir) {
    return CvFindPicEx(x1, y1, x2, y2, pic_name, sim, dir);
}
namespace {
std::atomic<uint64_t> memoryAliasNumber{};
bool ParseMemoryList(const char* text, std::vector<std::string>& aliases) {
    if (!text || !*text || strlen(text) > 65536) return false;
    auto fail = [&] {
        for (const auto& alias : aliases) FreePic(alias.c_str());
        aliases.clear();
        return false;
    };
    std::string_view remaining(text);
    while (!remaining.empty()) {
        const auto end = remaining.find('|');
        const auto item = remaining.substr(0, end);
        const auto comma = item.find(',');
        if (comma == item.npos) return fail();
        uint64_t address{}; uint64_t size{};
        const auto first = std::from_chars(item.data(), item.data() + comma, address);
        const auto second = std::from_chars(item.data() + comma + 1, item.data() + item.size(), size);
        if (first.ec != std::errc{} || first.ptr != item.data() + comma || !address ||
            second.ec != std::errc{} || second.ptr != item.data() + item.size() ||
            size < 54 || size > INT_MAX) return fail();
        const auto alias = "ai_mem_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(memoryAliasNumber.fetch_add(1)) + ".bmp";
        if (!LoadPicByte(static_cast<uintptr_t>(address), static_cast<int>(size), alias.c_str())) {
            for (const auto& old : aliases) FreePic(old.c_str());
            aliases.clear();
            return fail();
        }
        aliases.push_back(alias);
        if (end == remaining.npos) break;
        remaining.remove_prefix(end + 1);
        if (remaining.empty()) return fail();
    }
    return !aliases.empty();
}
const FsTongyongJieguo* FindPicMemory(bool all, int x1, int y1, int x2, int y2,
    const char* pic_info, double sim, int dir) {
    std::vector<std::string> aliases;
    if (!ParseMemoryList(pic_info, aliases))
        return fsdm::jieguo::Save(all ? "AiFindPicMemEx" : "AiFindPicMem",
            FS_CANSHU_CUOWU, 0);
    std::string names;
    for (const auto& alias : aliases) {
        if (!names.empty()) names += '|';
        names += alias;
    }
    const auto* result = all ? CvFindPicMemEx(x1, y1, x2, y2, names.c_str(), sim, dir) :
        CvFindPicMem(x1, y1, x2, y2, names.c_str(), sim, dir);
    for (const auto& alias : aliases) FreePic(alias.c_str());
    return result;
}
}
const FsTongyongJieguo* WINAPI AiFindPicMem(int x1, int y1, int x2, int y2,
    const char* pic_info, double sim, int dir) {
    return FindPicMemory(false, x1, y1, x2, y2, pic_info, sim, dir);
}
const FsTongyongJieguo* WINAPI AiFindPicMemEx(int x1, int y1, int x2, int y2,
    const char* pic_info, double sim, int dir) {
    return FindPicMemory(true, x1, y1, x2, y2, pic_info, sim, dir);
}
