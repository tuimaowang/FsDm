// 找图 DLL 的按需保存：从采集 DLL 复制完整 BGRA 帧，在 CPU 裁剪后交给系统图片编码器。
// 文件先写到目标目录的临时文件，全部完成后才替换目标，避免失败时留下不完整结果。
#include "ZhaoTu.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
int FsPinTemplatePixels(const char* name, UINT width, UINT height, const unsigned char* bgra);
namespace {
std::mutex pathLock;
std::filesystem::path basePath;
struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

// 对外路径统一用 UTF-8，避免中文路径随系统 ANSI 代码页变化而损坏。
std::wstring Wide(const char* text) {
    if (!text || !*text) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (!n) return {};
    std::wstring value(static_cast<size_t>(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, value.data(), n)) return {};
    value.pop_back();
    return value;
}
std::filesystem::path Destination(const char* text) {
    auto value = Wide(text);
    if (value.empty()) return {};
    std::filesystem::path result(value);
    if (result.is_absolute()) return result;
    // 带盘符但不是绝对路径的形式具有进程相关语义，拒绝以免保存到意外位置。
    if (result.has_root_name() || result.has_root_directory()) return {};
    std::lock_guard lock(pathLock);
    return std::filesystem::absolute(basePath.empty() ? result : basePath / result);
}
struct Region { int x1, y1, x2, y2; };
struct Picture {
    FsZhen frame{};
    UINT width{}, height{}, stride{};
    std::vector<BYTE> pixels;
};
// 按线程保留完整帧内存，后续保存和 GIF 的各帧直接复用容量，不保留可复用的旧画面。
thread_local std::vector<BYTE> frameBytes;

// 客户区和渲染尺寸不同时，用完整客户区对应完整帧的比例映射边界。
// 先读完整帧，再根据同一帧附带的尺寸裁剪，避免调整窗口时混用两份元数据。
bool ReadRegion(const Region& r, Picture& out) {
    if (r.x1 < 0 || r.y1 < 0 || r.x2 <= r.x1 || r.y2 <= r.y1) return false;
    FsZhen frame{}; frame.size = sizeof(frame);
    int result = CopyFrame(nullptr, 0, &frame);
    if (result != FS_HUANCHONG_BUZU || !frame.requiredBytes || frame.requiredBytes > FSDM_ZUIDA_ZIJIE)
        return false;
    auto& data = frameBytes;
    data.resize(static_cast<size_t>(frame.requiredBytes));
    // 尺寸可能在查询容量和读回之间变化；最多重试有限次数。
    for (int attempt = 0; attempt < 3; ++attempt) {
        frame.size = sizeof(frame);
        result = CopyFrame(data.data(), data.size(), &frame);
        if (result != FS_HUANCHONG_BUZU) break;
        if (!frame.requiredBytes || frame.requiredBytes > FSDM_ZUIDA_ZIJIE) return false;
        data.resize(static_cast<size_t>(frame.requiredBytes));
    }
    if (result != FS_CHENGGONG || !frame.clientWidth || !frame.clientHeight ||
        static_cast<uint32_t>(r.x2) > frame.clientWidth || static_cast<uint32_t>(r.y2) > frame.clientHeight)
        return false;
    const auto scale = [](uint32_t p, uint32_t from, uint32_t to) {
        return static_cast<uint32_t>(uint64_t{p} * to / from);
    };
    const UINT left = scale(r.x1, frame.clientWidth, frame.width);
    const UINT top = scale(r.y1, frame.clientHeight, frame.height);
    const UINT right = scale(r.x2, frame.clientWidth, frame.width);
    const UINT bottom = scale(r.y2, frame.clientHeight, frame.height);
    if (right <= left || bottom <= top || right > frame.width || bottom > frame.height) return false;
    out.frame = frame; out.width = right - left; out.height = bottom - top; out.stride = out.width * 4;
    out.pixels.resize(static_cast<size_t>(out.stride) * out.height);
    for (UINT y = 0; y < out.height; ++y)
        std::copy_n(data.data() + size_t{top + y} * frame.stride + size_t{left} * 4,
                    out.stride, out.pixels.data() + size_t{y} * out.stride);
    return true;
}

// 每帧单独写入；GIF 的延迟以百分之一秒存储，使用最近的可表示值。
HRESULT WriteFrame(IWICImagingFactory* factory, IWICBitmapEncoder* encoder,
                   const Picture& image, const GUID& format, bool gif, int delay) {
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(image.width, image.height);
    WICPixelFormatGUID pixelFormat = format;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
    if (SUCCEEDED(hr) && pixelFormat != format) hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    ComPtr<IWICBitmap> bitmap;
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromMemory(image.width, image.height,
        GUID_WICPixelFormat32bppBGRA, image.stride, static_cast<UINT>(image.pixels.size()),
        const_cast<BYTE*>(image.pixels.data()), &bitmap);
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(bitmap.Get(), pixelFormat, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeMedianCut);
    if (SUCCEEDED(hr) && gif) {
        // GIF 是索引色图像；调色板须随帧写出，不能只设置像素格式。
        ComPtr<IWICPalette> palette;
        hr = factory->CreatePalette(&palette);
        if (SUCCEEDED(hr)) hr = converter->CopyPalette(palette.Get());
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
    }
    if (SUCCEEDED(hr) && gif) {
        ComPtr<IWICMetadataQueryWriter> metadata;
        hr = frame->GetMetadataQueryWriter(&metadata);
        PROPVARIANT value{}; value.vt = VT_UI2;
        value.uiVal = static_cast<USHORT>(std::clamp((int64_t{delay} + 5) / 10, int64_t{1}, int64_t{65535}));
        if (SUCCEEDED(hr)) hr = metadata->SetMetadataByName(L"/grctlext/Delay", &value);
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(converter.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    return hr;
}

int Save(const Region& region, const char* name, int mode, const GUID& container, const GUID& pixelFormat,
         int delay, int duration, bool gif) {
    try {
        if (!gif && (mode < 1 || mode > 3)) return 0;
        if (gif && (delay < 0 || duration < 0 || (delay > 0 && duration <= 0))) return 0;
        const bool saveFile = gif || mode == 2 || mode == 3;
        const bool saveTemplate = !gif && (mode == 1 || mode == 3);
        const auto destination = saveFile ? Destination(name) : std::filesystem::path{};
        if ((saveFile && destination.empty()) || (saveTemplate && (!name || !*name))) return 0;
        Apartment apartment;
        if (saveFile && !apartment.valid()) return 0;
        // 裁剪后的缓冲区也按线程复用；每次 ReadRegion 都重新填入本次完整帧的区域。
        thread_local Picture image;
        if (!ReadRegion(region, image)) return 0;
        if (!saveFile)
            return FsPinTemplatePixels(name, image.width, image.height, image.pixels.data());
        const auto version = image.frame.bindingVersion;
        const UINT width = image.width, height = image.height;
        const UINT clientWidth = image.frame.clientWidth, clientHeight = image.frame.clientHeight;
        const UINT sourceWidth = image.frame.width, sourceHeight = image.frame.height;
        ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return 0;
        wchar_t temporary[MAX_PATH]{};
        const auto parent = destination.parent_path();
        if (!GetTempFileNameW(parent.c_str(), L"fsd", 0, temporary)) return 0;
        struct Cleanup { const wchar_t* path; ~Cleanup() { if (path) DeleteFileW(path); } } cleanup{temporary};
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(temporary, GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        const ULONGLONG start = GetTickCount64();
        uint64_t next = 0;
        do {
            if (next) {
                const ULONGLONG due = start + next;
                const ULONGLONG now = GetTickCount64();
                if (now < due) Sleep(static_cast<DWORD>(due - now));
                if (!ReadRegion(region, image) || image.frame.bindingVersion != version ||
                    image.width != width || image.height != height ||
                    image.frame.width != sourceWidth || image.frame.height != sourceHeight ||
                    image.frame.clientWidth != clientWidth || image.frame.clientHeight != clientHeight)
                    return 0;
            }
            if (SUCCEEDED(hr)) hr = WriteFrame(factory.Get(), encoder.Get(), image, pixelFormat, gif, delay);
            if (FAILED(hr)) return 0;
            if (!gif || !delay) break;
            next += static_cast<uint64_t>(delay);
        } while (next < static_cast<uint64_t>(duration));
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        encoder.Reset(); stream.Reset();
        if (FAILED(hr)) return 0;
        if (!MoveFileExW(temporary, destination.c_str(), MOVEFILE_REPLACE_EXISTING)) return 0;
        cleanup.path = nullptr;
        if (saveTemplate && !FsPinTemplatePixels(name, image.width, image.height, image.pixels.data()))
            return 0;
        return 1;
    } catch (...) { return 0; }
}
}

// 找图和保存使用同一目录解析规则，并由同一把锁保护 SetPath。
std::filesystem::path FsResolvePath(const char* path) { return Destination(path); }

int WINAPI SetPath(const char* path) {
    try {
        const auto value = Wide(path);
        if (value.empty()) return 0;
        const auto resolved = std::filesystem::absolute(std::filesystem::path(value));
        if (!std::filesystem::is_directory(resolved)) return 0;
        std::lock_guard lock(pathLock);
        basePath = resolved;
        return 1;
    } catch (...) { return 0; }
}
int WINAPI Capture(int x1, int y1, int x2, int y2, const char* file, int mode) {
    return Save({x1,y1,x2,y2}, file, mode, GUID_ContainerFormatBmp, GUID_WICPixelFormat24bppBGR, 0, 0, false);
}
int WINAPI CaptureJpg(int x1, int y1, int x2, int y2, const char* file, int mode) {
    return Save({x1,y1,x2,y2}, file, mode, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 0, 0, false);
}
int WINAPI CapturePng(int x1, int y1, int x2, int y2, const char* file, int mode) {
    return Save({x1,y1,x2,y2}, file, mode, GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, 0, 0, false);
}
int WINAPI CaptureGif(int x1, int y1, int x2, int y2, const char* file, int delay, int time) {
    return Save({x1,y1,x2,y2}, file, 2, GUID_ContainerFormatGif, GUID_WICPixelFormat8bppIndexed, delay, time, true);
}
