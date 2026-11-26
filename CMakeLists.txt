cmake_minimum_required(VERSION 3.21)
project(FsDm VERSION 0.1.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Requires Windows x64 and MSVC.")
endif()
# Release 参数被外部配置清空时，恢复 MSVC 优化和发布版断言设置。
# 仅补齐空值，保留调用者明确提供的自定义 Release 参数。
foreach(language IN ITEMS C CXX)
    if("${CMAKE_${language}_FLAGS_RELEASE}" STREQUAL "")
        set(CMAKE_${language}_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG" CACHE STRING
            "MSVC Release 编译参数" FORCE)
    endif()
endforeach()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(detours_src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Detours/src")
# 保留第三方源码，生成专用事务分配器版本，防止挂起线程持有堆锁时死锁。
include(cmake/ShiwuNeicun.cmake)
add_library(detours STATIC "${CMAKE_CURRENT_BINARY_DIR}/DetoursShengcheng.cpp"
    "${detours_src}/disasm.cpp" "${detours_src}/modules.cpp")
target_include_directories(detours PUBLIC "${detours_src}")

add_library(ZhuaTu SHARED ZhuaTu/Gouzi.cpp ZhuaTu/Huichuan.cpp ZhuaTu/Caiji.cpp ZhuaTu/ObsCaiji.cpp ZhuaTu/WgcCaiji.cpp)
target_compile_definitions(ZhuaTu PRIVATE ZHUATU_SHENGCHENG)
target_link_libraries(ZhuaTu PRIVATE detours d3d10 d3d11 d3d12 dxgi user32 dwmapi windowsapp)
# 根据公开声明生成工作台调用表；不依赖外部 Python 环境。
set(zidong_diaoyong "${CMAKE_CURRENT_BINARY_DIR}/ZidongDiaoyong.inc")
add_custom_command(OUTPUT "${zidong_diaoyong}"
    COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
        "${CMAKE_CURRENT_SOURCE_DIR}/ZhaoTu/ShengchengDiaoyong.ps1"
        -Toutou "${CMAKE_CURRENT_SOURCE_DIR}/ZhaoTu/ZhaoTu.h" -Shuchu "${zidong_diaoyong}"
    DEPENDS ZhaoTu/ShengchengDiaoyong.ps1 ZhaoTu/ZhaoTu.h VERBATIM)
add_library(ZhaoTu SHARED ZhaoTu/Kongzhi.cpp ZhaoTu/Gongzuotai.cpp ZhaoTu/TupianBaocun.cpp ZhaoTu/TuseKuozhan.cpp ZhaoTu/ZhaoTuZhaoSe.cpp ZhaoTu/XianKaChazhao.cpp
    ZhaoTu/WenziShibie.cpp ZhaoTu/WenziMoxing.cpp ZhaoTu/WenziZiku.cpp ZhaoTu/YoloShibie.cpp ZhaoTu/ZhenhuanCun.cpp
    ZhaoTu/ZifuchuanNeicun.cpp ZhaoTu/TongyongJieguo.cpp ZhaoTu/ShebeiZhanyong.cpp "${zidong_diaoyong}")
file(GLOB yolo_model_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/YoloMoxing/*")
set(yolo_model_stamp "${CMAKE_CURRENT_BINARY_DIR}/YoloMoxingCopied-$<CONFIG>.stamp")
add_custom_command(OUTPUT "${yolo_model_stamp}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/YoloMoxing"
        "$<TARGET_FILE_DIR:ZhaoTu>/YoloMoxing"
    COMMAND ${CMAKE_COMMAND} -E touch "${yolo_model_stamp}"
    DEPENDS ${yolo_model_files} VERBATIM)
add_custom_target(YoloMoxingResources DEPENDS "${yolo_model_stamp}")
add_dependencies(ZhaoTu YoloMoxingResources)
target_include_directories(ZhaoTu PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(ZhaoTu PRIVATE ZHAOTU_SHENGCHENG)
target_link_libraries(ZhaoTu PRIVATE user32 windowscodecs ole32 d3d11 dxgi d3dcompiler pdh)

# 使用 CPU 静态构建，避免依赖工作区中需要 CUDA 的 OpenCV 版本。
include(FetchContent)
set(BUILD_LIST core,imgproc CACHE STRING "OpenCV 模块" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "静态链接 OpenCV" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "不构建 OpenCV 测试" FORCE)
set(BUILD_PERF_TESTS OFF CACHE BOOL "不构建 OpenCV 性能测试" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "不构建 OpenCV 示例" FORCE)
set(BUILD_opencv_apps OFF CACHE BOOL "不构建 OpenCV 工具" FORCE)
set(WITH_CUDA OFF CACHE BOOL "不使用 CUDA" FORCE)
set(WITH_IPP OFF CACHE BOOL "不下载 IPP" FORCE)
set(WITH_OPENCL OFF CACHE BOOL "不使用 OpenCL" FORCE)
set(OPENCV_ENABLE_NONFREE OFF CACHE BOOL "仅使用开放模块" FORCE)
FetchContent_Declare(opencv
    URL "https://codeload.github.com/opencv/opencv/tar.gz/2e1f8da65e4f9fa1a98423e6ac223187438a4db8"
    URL_HASH SHA256=f278379b7c923d19d0a372658f1e32038c8002bff93173235d7ec11d3f719087 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(opencv)
target_include_directories(ZhaoTu SYSTEM PRIVATE
    "${opencv_SOURCE_DIR}/modules/core/include" "${opencv_SOURCE_DIR}/modules/imgproc/include"
    "${opencv_BINARY_DIR}")
target_link_libraries(ZhaoTu PRIVATE opencv_core opencv_imgproc)
install(FILES "${opencv_SOURCE_DIR}/LICENSE" DESTINATION dll/licenses RENAME OpenCV-LICENSE.txt)
install(FILES "${opencv_SOURCE_DIR}/LICENSE" DESTINATION ZhaoTu/dist/licenses RENAME OpenCV-LICENSE.txt)
target_include_directories(ZhaoTu SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include")
target_link_libraries(ZhaoTu PRIVATE d3d12)
# 仅使用本项目配套 x64 运行库；模型和 DLL 都以 ZhaoTu.dll 的目录为基准。
# DLL 延迟动态加载，未使用 OCR 时不增加运行库/模型初始化成本。
set(ocr_runtime_files onnxruntime.dll onnxruntime_providers_shared.dll DirectML.dll)
foreach(file IN LISTS ocr_runtime_files)
    add_custom_command(TARGET ZhaoTu POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/bin/${file}" "$<TARGET_FILE_DIR:ZhaoTu>/${file}")
    install(FILES "third_party/onnxruntime/bin/${file}" DESTINATION ZhaoTu/dist/bin)
    install(FILES "third_party/onnxruntime/bin/${file}" DESTINATION dll)
endforeach()
add_custom_command(TARGET ZhaoTu POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ocr_models" "$<TARGET_FILE_DIR:ZhaoTu>/ocr_models")
install(DIRECTORY third_party/ocr_models DESTINATION ZhaoTu/dist/bin)
install(DIRECTORY third_party/ocr_models DESTINATION dll)
install(DIRECTORY third_party/onnxruntime/licenses/ DESTINATION ZhaoTu/dist/licenses)
install(DIRECTORY third_party/onnxruntime/licenses/ DESTINATION dll/licenses)
install(FILES third_party/onnxruntime/Laiyuan.md DESTINATION dll/licenses RENAME OCR-Laiyuan.md)
install(FILES OcrCeshi.txt DESTINATION dll)

include(CTest)
if(BUILD_TESTING)
    add_executable(JichengCeshi Ceshi/JichengCeshi.cpp)
    target_include_directories(JichengCeshi PRIVATE ZhuaTu Ceshi)
    add_dependencies(JichengCeshi ZhaoTu ZhuaTu)
    target_link_libraries(JichengCeshi PRIVATE d3d10 d3d11 d3d12 dxgi user32 windowscodecs ole32)
    add_executable(ZhaoSeCeshi Ceshi/ZhaoSeCeshi.cpp)
    target_include_directories(ZhaoSeCeshi PRIVATE ZhuaTu Ceshi)
    target_link_libraries(ZhaoSeCeshi PRIVATE d3d11 dxgi user32 windowscodecs ole32)
    add_dependencies(ZhaoSeCeshi ZhaoTu ZhuaTu)
    # 直接编译生产读回实现，验证真实设备/纹理身份，避免增加仅供测试的 DLL 接口。
    add_executable(FuyongCeshi Ceshi/FuyongCeshi.cpp ZhuaTu/Huichuan.cpp)
    target_include_directories(FuyongCeshi PRIVATE ZhuaTu Ceshi)
    target_link_libraries(FuyongCeshi PRIVATE d3d10 d3d11 d3d12 dxgi user32)
    add_executable(SuoCeshi Ceshi/SuoCeshi.cpp)
    target_include_directories(SuoCeshi PRIVATE ZhuaTu)
    target_link_libraries(SuoCeshi PRIVATE detours)
    add_executable(JiekouCeshi Ceshi/JiekouCeshi.c)
    target_compile_definitions(JiekouCeshi PRIVATE
        FSDM_YOLO_MODEL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/YoloMoxing")
    add_executable(ZifuchuanCeshi Ceshi/ZifuchuanCeshi.cpp ZhaoTu/ZifuchuanNeicun.cpp ZhaoTu/TongyongJieguo.cpp)
    target_compile_definitions(ZifuchuanCeshi PRIVATE ZHAOTU_SHENGCHENG)
    add_executable(WenziCeshi Ceshi/WenziCeshi.cpp)
    target_include_directories(WenziCeshi PRIVATE ZhuaTu Ceshi)
    target_link_libraries(WenziCeshi PRIVATE ZhaoTu d3d11 dxgi user32 gdi32)
    add_dependencies(WenziCeshi ZhuaTu)
    add_executable(ObsCeshi Ceshi/ObsCeshi.cpp)
    target_include_directories(ObsCeshi PRIVATE ZhuaTu Ceshi)
    target_link_libraries(ObsCeshi PRIVATE d3d10 d3d11 d3d12 dxgi user32)
    add_dependencies(ObsCeshi ZhaoTu ZhuaTu)
    add_executable(WgcCeshi Ceshi/WgcCeshi.cpp)
    target_include_directories(WgcCeshi PRIVATE ZhuaTu Ceshi)
    target_link_libraries(WgcCeshi PRIVATE d3d11 dxgi user32 gdi32 dwmapi)
    add_dependencies(WgcCeshi ZhaoTu ZhuaTu)
    # 单独的缺依赖目录，不改动并行测试正在使用的完整二进制目录。
    add_custom_target(QueyilaiCeshiZhunbei ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ObsCeshi>/queyilai"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:ObsCeshi>"
            "$<TARGET_FILE:ZhaoTu>" "$<TARGET_FILE:ZhuaTu>" "$<TARGET_FILE_DIR:ObsCeshi>/queyilai"
        DEPENDS ObsCeshi ZhaoTu ZhuaTu)
    set_property(TARGET JiekouCeshi PROPERTY C_STANDARD 11)
    target_link_libraries(JiekouCeshi PRIVATE ZhaoTu)
    add_test(NAME c_abi COMMAND JiekouCeshi)
    add_test(NAME string_memory COMMAND ZifuchuanCeshi)
    set_tests_properties(string_memory PROPERTIES TIMEOUT 20)
    add_test(NAME ocr_cpu COMMAND WenziCeshi)
    add_test(NAME ocr_directml COMMAND WenziCeshi --directml)
    add_test(NAME ocr_dictionary COMMAND WenziCeshi --dict)
    set_tests_properties(ocr_cpu ocr_directml PROPERTIES TIMEOUT 300)
    set_tests_properties(ocr_dictionary PROPERTIES TIMEOUT 45)
    add_test(NAME detours_allocator COMMAND SuoCeshi)
    add_test(NAME capture_lifecycle COMMAND JichengCeshi)
    add_test(NAME workbench_images COMMAND JichengCeshi --workbench-images)
    add_test(NAME cpu_search COMMAND ZhaoSeCeshi)
    add_test(NAME gpu_search COMMAND ZhaoSeCeshi --gpu)
    add_test(NAME gpu_resource_reuse COMMAND ZhaoSeCeshi --gpu-reuse)
    add_test(NAME gpu_search_hardware COMMAND ZhaoSeCeshi --gpu-hardware)
    set_tests_properties(gpu_search PROPERTIES TIMEOUT 240)
    set_tests_properties(gpu_resource_reuse PROPERTIES TIMEOUT 90)
    set_tests_properties(gpu_search_hardware PROPERTIES TIMEOUT 180)
    add_test(NAME resource_reuse COMMAND FuyongCeshi)
    set_tests_properties(resource_reuse PROPERTIES TIMEOUT 45)
    set_tests_properties(cpu_search PROPERTIES TIMEOUT 90)
    add_test(NAME obs_hardware COMMAND ObsCeshi)
    add_test(NAME obs_dx10 COMMAND ObsCeshi --dx10)
    add_test(NAME obs_dx12 COMMAND ObsCeshi --dx12)
    add_test(NAME obs_missing COMMAND "$<TARGET_FILE_DIR:ObsCeshi>/queyilai/ObsCeshi.exe" --missing)
    add_test(NAME obs_conflict COMMAND ObsCeshi --conflict)
    add_test(NAME obs_window_mismatch COMMAND ObsCeshi --mismatch)
    add_test(NAME wgc_hardware COMMAND WgcCeshi)
    set_tests_properties(wgc_hardware PROPERTIES TIMEOUT 90)
    set_tests_properties(obs_hardware obs_dx10 obs_dx12 obs_missing obs_conflict obs_window_mismatch PROPERTIES TIMEOUT 60)
    set_tests_properties(c_abi PROPERTIES TIMEOUT 60)
    set_tests_properties(detours_allocator PROPERTIES TIMEOUT 10)
    set_tests_properties(capture_lifecycle workbench_images PROPERTIES TIMEOUT 120)
endif()
foreach(target ZhuaTu ZhaoTu JichengCeshi ZhaoSeCeshi FuyongCeshi SuoCeshi JiekouCeshi ZifuchuanCeshi ObsCeshi WgcCeshi WenziCeshi)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE /W4 /utf-8
            "$<$<COMPILE_LANGUAGE:CXX>:/permissive->" "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        if(NOT target STREQUAL "JiekouCeshi")
            target_compile_features(${target} PRIVATE cxx_std_20)
        endif()
    endif()
endforeach()
foreach(target ZhuaTu ZhaoTu)
    install(TARGETS ${target} RUNTIME DESTINATION "${target}/dist/bin"
        ARCHIVE DESTINATION "${target}/dist/lib")
    install(FILES "${target}/${target}.h" DESTINATION "${target}/dist/include/${target}")
endforeach()
install(FILES Gonggong/Jiekou.h DESTINATION ZhuaTu/dist/include/Gonggong)
install(FILES Gonggong/Jiekou.h DESTINATION ZhaoTu/dist/include/Gonggong)
install(FILES README.md THIRD_PARTY.md Yanzheng.md HanshuShuoming.md DESTINATION .)
install(FILES third_party/Detours/LICENSE.md DESTINATION ZhuaTu/dist/licenses RENAME Detours-LICENSE.md)

# 构建树、原 dist 和统一 dll 目录都配齐相同的 x64 OBS 依赖。
set(obs_files graphics-hook64.dll get-graphics-offsets64.exe)
foreach(file IN LISTS obs_files)
    add_custom_command(TARGET ZhuaTu POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs/${file}" "$<TARGET_FILE_DIR:ZhuaTu>/${file}")
    install(FILES "third_party/obs/${file}" DESTINATION ZhuaTu/dist/bin)
    install(FILES "third_party/obs/${file}" DESTINATION dll)
endforeach()
install(TARGETS ZhaoTu ZhuaTu RUNTIME DESTINATION dll)
install(FILES README.md THIRD_PARTY.md Yanzheng.md HanshuShuoming.md DESTINATION dll)
install(FILES third_party/Detours/LICENSE.md DESTINATION dll/licenses RENAME Detours-LICENSE.md)
install(FILES third_party/obs/COPYING DESTINATION dll/licenses RENAME OBS-COPYING.txt)
install(FILES third_party/obs/COPYING DESTINATION ZhuaTu/dist/licenses RENAME OBS-COPYING.txt)
