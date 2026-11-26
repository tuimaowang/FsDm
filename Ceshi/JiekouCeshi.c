/* 用真正的 C 编译单元检查结构布局、导出名称以及未绑定时的契约。 */
#include "../ZhaoTu/ZhaoTu.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
_Static_assert(sizeof(FsZhen) == 88, "FsZhen ABI");
_Static_assert(sizeof(FsGpuZhen) == 136, "FsGpuZhen ABI");
_Static_assert(sizeof(FsCaijiZhuangtai) == 72, "FsCaijiZhuangtai ABI");
_Static_assert(sizeof(FsZhuangtai) == 208, "FsZhuangtai ABI");
_Static_assert(sizeof(FsChazhaoZhuangtai) == 88, "FsChazhaoZhuangtai ABI");
_Static_assert(sizeof(FsOcrZhuangtai) == 624, "FsOcrZhuangtai ABI");
_Static_assert(offsetof(FsZhen, requiredBytes) == 32, "frame offset");
_Static_assert(offsetof(FsZhuangtai, frame) == 120, "status offset");
_Static_assert(sizeof(FsJieguoXiang)==40, "native item ABI");
_Static_assert(sizeof(FsTongyongJieguo)==40, "native result ABI");
_Static_assert(offsetof(FsJieguoXiang,text)==24, "item text offset");
_Static_assert(offsetof(FsTongyongJieguo,items)==24, "result items offset");
_Static_assert(offsetof(FsTongyongJieguo,fullText)==32, "result text offset");
int main(void) {
    FsZhuangtai state = {0};
    FsZhen frame = {0};
    /* 删除的两个按函数名释放入口必须从真实 DLL 导出表消失。 */
    HMODULE module = GetModuleHandleW(L"ZhaoTu.dll");
    if (!module || GetProcAddress(module, "FreeFunctionResults") ||
        GetProcAddress(module, "FreeStringResult") ||
        !GetProcAddress(module, "SetOcrModelFiles")) return 49;
    /* 使用真正的 C 调用校验 double 返回值和导出名称。GPU 驱动不可用时允许明确返回 -1。 */
    {
        double cpu, gpu;
        if (!GetProcAddress(module, "GetCpuUsage") || !GetProcAddress(module, "GetGpuUsage")) return 50;
        cpu = GetCpuUsage();
        gpu = GetGpuUsage();
        if (!isfinite(cpu) || cpu < 0.0 || cpu > 100.0 ||
            !isfinite(gpu) || (gpu != -1.0 && (gpu < 0.0 || gpu > 100.0))) return 51;
        printf("Device usage CPU=%.2f%% GPU=%.2f%%\n", cpu, gpu);
    }
    state.size = sizeof(state);
    frame.size = sizeof(frame);
    if (GetStatus(&state) != FS_CHENGGONG || state.bound || state.inputImplemented) return 1;
    if (CopyFrame(NULL, 0, &frame) != FS_WEIBANGDING) return 2;
    if (BindWindow(NULL, "dx", "normal", "normal", 0) != 0) return 3;
    if (!UnBindWindow() || !UnBindWindow()) return 4;
    if (GetStatus(NULL) != FS_CANSHU_CUOWU) return 5;
    /* 原始数据地址必须保持指针宽度；此处只检查仍返回原始地址/整数的图像接口。 */
    if (GetScreenData(0, 0, 1, 1) || IsDisplayDead(0, 0, 1, 1, 0) ||
        strlen(MatchPicName(NULL))) return 28;
    /* 三个模板接口可由 C 调用，地址与指针等宽；未绑定即可处理参数及释放缓存。 */
    if (sizeof(uintptr_t) != sizeof(void*) || LoadPic(NULL) || LoadPic("") ||
        LoadPicByte((uintptr_t)0, 0, "memory.bmp") || LoadPicByte((uintptr_t)1, 64, "memory.bmp") ||
        FreePic(NULL) || !FreePic("nonexistent.bmp")) return 20;
    {
        FsGpuZhen gpu = {0}; FsCaijiZhuangtai extra = {0};
        gpu.size = sizeof(gpu); extra.size = sizeof(extra);
        if (AcquireGpuFrame(&gpu) != FS_WEIBANGDING || gpu.token) return 6;
        if (ReleaseGpuFrame(0) != FS_CANSHU_CUOWU) return 7;
        /* 未选择模式、未加载采集模块时就应公开 DX 默认值，且没有采集或读回。 */
        if (GetCaptureStatus(&extra) || extra.captureBackend != FS_CAIJI_DX ||
            extra.cpuReadbacks || extra.gpuSlots) return 8;
    }

    /* 新结构体可由真正的 C 程序直接访问，无需 C++ 容器或分隔字符串。 */
    {
        FsChazhaoZhuangtai find={0};
        const FsTongyongJieguo* results[26];
        const FsTongyongJieguo* repeated;
        const char* error;
        char error_copy[512];
        int i,j;
        find.size=sizeof(find);
        if (GetFindStatus(&find) || find.backend!=1 || !SetFindBackend("cpu") ||
            !SetFindBackend("gpu") || SetFindBackend("bad") ||
            GetFindStatus(NULL)!=FS_CANSHU_CUOWU || !SetFindBackend("cpu")) return 9;
        results[0]=GetColor(0,0);
        results[1]=CmpColor(0,0,"000000",1);
        results[2]=FindColor(0,0,10,10,"ffffff-000000",1,0);
        results[3]=FindColorEx(0,0,10,10,"ffffff-000000",1,0);
        results[4]=FindMultiColor(0,0,10,10,"ffffff","1|1|000000",1,0);
        results[5]=FindMultiColorEx(0,0,10,10,"ffffff","1|1|000000",1,0);
        results[6]=FindPic(0,0,10,10,"test.bmp","000000",1,0);
        results[7]=FindPicS(0,0,10,10,"test.bmp","000000",1,0);
        results[8]=FindPicEx(0,0,10,10,"test.bmp","000000",1,0);
        results[9]=FindPicExS(0,0,10,10,"test.bmp","000000",1,0);
        results[10]=Ocr(0,0,10,10,"",1);
        results[11]=OcrEx(0,0,10,10,"",1);
        results[12]=OcrExOne(0,0,10,10,"",1);
        results[13]=OcrInFile(0,0,10,10,"","",1);
        results[14]=FindStr(0,0,10,10,"A","",1);
        results[15]=FindStrS(0,0,10,10,"A","",1);
        results[16]=FindStrEx(0,0,10,10,"A","",1);
        results[17]=FindStrExS(0,0,10,10,"A","",1);
        results[18]=FindStrFast(0,0,10,10,"A","",1);
        results[19]=FindStrFastS(0,0,10,10,"A","",1);
        results[20]=FindStrFastEx(0,0,10,10,"A","",1);
        results[21]=FindStrFastExS(0,0,10,10,"A","",1);
        results[22]=AiFindPic(0,0,10,10,"test.bmp",1,0);
        results[23]=AiFindPicEx(0,0,10,10,"test.bmp",1,0);
        results[24]=AiFindPicMem(0,0,10,10,"",1,0);
        results[25]=AiFindPicMemEx(0,0,10,10,"",1,0);
        for(i=0;i<26;++i) {
            const FsTongyongJieguo* r=results[i];
            if(!r || r->size!=sizeof(*r) || r->version!=1 || r->count || r->items ||
                !r->fullText || r->fullText[0] ||
                r->status==FS_CHENGGONG) return 10+i;
            for(j=0;j<i;++j) if(r==results[j]) return 40;
        }
        repeated=GetColor(0,0);
        if(!repeated || repeated==results[0] || !FreeResult(repeated) ||
            FreeResult(repeated) || !FreeResult(NULL) ||
            FreeResult((const FsTongyongJieguo*)(uintptr_t)1)) return 41;
        /* 按地址释放一个结果，其余结果仍然可读；覆盖新增 AiFindPic 导出。 */
        for(i=0;i<26;++i) {
            if(results[i]->status==FS_CHENGGONG || !FreeResult(results[i])) return 42;
        }
        /* 保留的两个字符串接口继续返回独立 UTF-8 数组，同一组释放接口能够清理它们。 */
        if(SetOcrEngine("bad")) return 43;
        error=GetOcrError();
        if(!error || !*error) return 44;
        strcpy_s(error_copy,sizeof(error_copy),error);
        if(!SetOcrEngine("model") || strcmp(error,error_copy)) return 45;
        if(!FreeAllResults() || !FreeAllResults() || !FreeAllStringResults()) return 46;
    }
    {
        FsOcrZhuangtai ocr={0};ocr.size=sizeof(ocr);
        if(GetOcrStatus(&ocr) || ocr.loadedSessions || ocr.sessionsCreated || ocr.devicesCreated ||
            GetOcrStatus(NULL)!=FS_CANSHU_CUOWU) return 47;
        if(!SetOcrBackend("directml") || !SetOcrBackend("cpu") || SetOcrBackend("cuda") ||
            !SetOcrEngine("dict") || !SetOcrEngine("model") ||
            SetOcrModelFiles(-1,NULL,NULL,NULL,NULL) ||
            GetNowDict()!=0 || GetDictCount(0) || UseDict(0) || !ClearDict(0)) return 48;
    }
    {
        const char* sorted;
        const char* classes;
        const char* detections;
        char copy[128];
        if (AiYoloSetVersion("v4") || !AiYoloSetVersion("v5-7.0") ||
            AiYoloSetModelMemory(0, (uintptr_t)0, 10, "") ||
            AiYoloUseModel(0) || !AiYoloFreeModel(0)) return 50;
#ifdef FSDM_YOLO_MODEL_DIR
        if (!SetPath(FSDM_YOLO_MODEL_DIR)) return 55;
        if (!AiYoloSetModel(0, "yolov5n.onnx", "")) {
            fprintf(stderr, "YOLO model setup failed: %s\n", GetOcrError());
            return 56;
        }
        if (!AiYoloUseModel(0) || !AiYoloFreeModel(0)) return 57;
#endif
        sorted = AiYoloSortsObjects("1,0.8,30,40,10,10|0,0.9,5,10,8,8", 20);
        if (!sorted || strcmp(sorted, "0,0.9,5,10,8,8|1,0.8,30,40,10,10")) return 51;
        strcpy_s(copy, sizeof(copy), sorted);
        if (strcmp(sorted, copy)) return 52;
        detections = AiYoloDetectObjects(0,0,10,10,0.5,0.45);
        classes = AiYoloObjectsToString("");
        if (!detections || *detections || !classes || *classes) return 53;
        if (!FreeAllStringResults()) return 54;
    }
    return 0;
}
