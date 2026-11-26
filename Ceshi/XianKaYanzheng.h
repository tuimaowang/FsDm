#pragma once
// 把同一组 GPU 查找契约接到 DX10/11/12、OBS 和 WGC 的真实采集测试中。
// 测试区域的左上角至少 16x16 是给定纯色；模板自行编码，不调用截图保存函数。
#include "../ZhaoTu/ZhaoTu.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>

inline void YanzhengXianKaChazhao(HMODULE module,const char* rgb) {
    const auto require=[](bool value,const char* label) {
        if(!value) {std::fprintf(stderr,"FAIL GPU backend: %s\n",label);throw 1;}
    };
#define FS_TEST_LOAD(name) auto name=reinterpret_cast<decltype(&::name)>(GetProcAddress(module,#name));require(name!=nullptr,#name)
    FS_TEST_LOAD(SetFindBackend); FS_TEST_LOAD(GetColor); FS_TEST_LOAD(FindColor);
    FS_TEST_LOAD(FindColorEx); FS_TEST_LOAD(FindMultiColor); FS_TEST_LOAD(FindMultiColorEx);
    FS_TEST_LOAD(FindPic); FS_TEST_LOAD(FindPicS); FS_TEST_LOAD(FindPicEx); FS_TEST_LOAD(FindPicExS);
    FS_TEST_LOAD(GetCaptureStatus); FS_TEST_LOAD(GetFindStatus); FS_TEST_LOAD(FreeResult);
#undef FS_TEST_LOAD
    wchar_t temp[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH,temp)!=0,"temp folder");
    const auto path=std::filesystem::path(temp)/(L"FsDmGpu_"+std::to_wstring(GetCurrentProcessId())+L".bmp");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {std::error_code error;std::filesystem::remove(path,error);}
    } cleanup{path};
    const auto utf8=path.u8string();const std::string filename(utf8.begin(),utf8.end());
    {
        BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=54;header.bfSize=70;
        BITMAPINFOHEADER info{};info.biSize=40;info.biWidth=info.biHeight=2;
        info.biPlanes=1;info.biBitCount=24;info.biSizeImage=16;
        const auto color=std::stoul(rgb,nullptr,16);
        const BYTE row[]={BYTE(color),BYTE(color>>8),BYTE(color>>16),
            BYTE(color),BYTE(color>>8),BYTE(color>>16),0,0};
        std::ofstream file(path,std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header),sizeof(header));
        file.write(reinterpret_cast<const char*>(&info),sizeof(info));
        file.write(reinterpret_cast<const char*>(row),sizeof(row));
        file.write(reinterpret_cast<const char*>(row),sizeof(row));
        require(file.good(),"independent BMP24");
    }
    FsCaijiZhuangtai before{},after{};before.size=sizeof(before);after.size=sizeof(after);
    require(GetCaptureStatus(&before)==FS_CHENGGONG && SetFindBackend("gpu")==1,"select GPU");
    const std::string color=std::string(rgb)+"-000000",offset=std::string("0|0|")+rgb;
    // 直接检查新版结构体，避免把坐标重新拼成字符串来验证。
    // 所有后端都必须给出完整字段；检查结束立即精确释放，不清理外部调用者的结果。
    const auto check=[&](const FsTongyongJieguo* result,const char* label,
        uint32_t fields,bool all,bool picture=false,bool pixel=false) {
        bool valid=result && result->size==sizeof(*result) && result->version==1 &&
            result->status==FS_CHENGGONG && result->items && result->fullText &&
            (all?result->count>1:result->count==1);
        if(valid) {
            for(uint32_t i=0;i<result->count;++i) {
                const auto& item=result->items[i];
                valid=valid && item.fields==fields && item.text && item.file &&
                    item.x>=0 && item.y>=0 &&
                    (!picture || std::string(item.file)==filename) &&
                    (!pixel || item.color==std::stoul(rgb,nullptr,16));
            }
            valid=valid && result->items[0].x==0 && result->items[0].y==0 &&
                (pixel || result->items[0].id==0);
        }
        if(result) valid=FreeResult(result)==1 && valid;
        require(valid,label);
    };
    check(GetColor(0,0),"GetColor",FS_JG_XY|FS_JG_COLOR,false,false,true);
    check(FindColor(0,0,16,16,color.c_str(),1.,0),"FindColor",FS_JG_ID|FS_JG_XY,false);
    check(FindColorEx(0,0,16,16,color.c_str(),1.,0),"FindColorEx",FS_JG_ID|FS_JG_XY,true);
    check(FindMultiColor(0,0,16,16,rgb,offset.c_str(),1.,0),"FindMultiColor",FS_JG_ID|FS_JG_XY,false);
    check(FindMultiColorEx(0,0,16,16,rgb,offset.c_str(),1.,0),"FindMultiColorEx",FS_JG_ID|FS_JG_XY,true);
    check(FindPic(0,0,16,16,filename.c_str(),"000000",1.,0),"FindPic",FS_JG_ID|FS_JG_XY|FS_JG_FILE,false,true);
    check(FindPicS(0,0,16,16,filename.c_str(),"000000",1.,0),"FindPicS",FS_JG_ID|FS_JG_XY|FS_JG_FILE,false,true);
    check(FindPicEx(0,0,16,16,filename.c_str(),"000000",1.,0),"FindPicEx",FS_JG_ID|FS_JG_XY|FS_JG_FILE,true,true);
    check(FindPicExS(0,0,16,16,filename.c_str(),"000000",1.,0),"FindPicExS",FS_JG_ID|FS_JG_XY|FS_JG_FILE,true,true);
    FsChazhaoZhuangtai find{};find.size=sizeof(find);
    require(GetFindStatus(&find)==FS_CHENGGONG && find.backend==1 && find.result==FS_CHENGGONG &&
        find.deviceSerial && find.resultReadbackBytes,"GPU result metadata");
    require(GetCaptureStatus(&after)==FS_CHENGGONG && before.cpuReadbacks==after.cpuReadbacks,
        "nine functions never perform CPU pixel readback");
}
