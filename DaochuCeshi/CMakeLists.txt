cmake_minimum_required(VERSION 3.21)
project(DaochuCeshi LANGUAGES CXX)

# 本项目独立配置、独立编译，不引入也不修改 DLL 项目的 CMake。
# 原测试通过函数地址调用；新增截图测试直接链接交付 LIB，两者均不包含内部实现。
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Requires Windows x64 and MSVC.")
endif()
get_filename_component(fsdm_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FSDM_DLL_DIR "${fsdm_root}/out/build/bin/Release" CACHE PATH "Test executable output directory")
set(FSDM_JIAOFU_DIR "${fsdm_root}/out/build/bin/Release/Jiaofu" CACHE PATH "Delivered header, DLL and LIB directory")
set(jiekou "${FSDM_JIAOFU_DIR}/FsDmJiekou.h")
if(NOT EXISTS "${jiekou}")
    message(FATAL_ERROR "Delivered FsDmJiekou.h is missing: ${jiekou}")
endif()

# 从公开函数指针声明生成检查表，新增公开接口后重新配置本项目即可纳入检查。
# 实际导出表还会反向核对，发现 DLL 多导出或头文件遗漏时同样报错。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${jiekou}")
file(STRINGS "${jiekou}" shengming REGEX "^typedef .*WINAPI\\* FsDm_[A-Za-z0-9_]+Fn")
set(biao "// 从交付头文件自动生成，请勿手动编辑。\n")
foreach(hang IN LISTS shengming)
    string(REGEX REPLACE ".*WINAPI\\* FsDm_([A-Za-z0-9_]+)Fn.*" "\\1" ming "${hang}")
    string(APPEND biao "DAOCHU_ZHAOTU(${ming})\n")
endforeach()
file(STRINGS "${fsdm_root}/ZhuaTu/ZhuaTu.h" zhuatu_shengming REGEX "^ZT_API .*WINAPI [A-Za-z0-9_]+\\(")
foreach(hang IN LISTS zhuatu_shengming)
    string(REGEX REPLACE ".*WINAPI ([A-Za-z0-9_]+)\\(.*" "\\1" ming "${hang}")
    string(APPEND biao "DAOCHU_ZHUATU(${ming})\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DaochuBiao.inc" "${biao}")

add_executable(DaochuCeshi DaochuCeshi.cpp)
target_include_directories(DaochuCeshi PRIVATE "${FSDM_JIAOFU_DIR}" "${fsdm_root}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(DaochuCeshi PRIVATE cxx_std_20)
target_compile_definitions(DaochuCeshi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(DaochuCeshi PRIVATE /W4 /utf-8 /permissive- /EHsc)
set_property(TARGET DaochuCeshi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(DaochuCeshi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${FSDM_DLL_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${FSDM_DLL_DIR}")

enable_testing()
add_test(NAME dao_chu_jie_kou COMMAND DaochuCeshi --no-pause --dll-dir "${FSDM_JIAOFU_DIR}")
set_tests_properties(dao_chu_jie_kou PROPERTIES TIMEOUT 45)

# 直接链接，就是编译 EXE 时由 LIB 确定 DLL 入口；这里专门使用交付目录，
# 防止误把源码构建目录里的旧 LIB 当成本次交付文件验证。
foreach(wenjian FsDmJiekou.h ZhaoTu.dll ZhaoTu.lib ZhuaTu.dll ZhuaTu.lib)
    if(NOT EXISTS "${FSDM_JIAOFU_DIR}/${wenjian}")
        message(FATAL_ERROR "Missing delivered file: ${FSDM_JIAOFU_DIR}/${wenjian}")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FSDM_JIAOFU_DIR}/FsDmJiekou.h")
file(STRINGS "${FSDM_JIAOFU_DIR}/FsDmJiekou.h" zhilian_shengming REGEX "^typedef .*WINAPI\\* FsDm_[A-Za-z0-9_]+Fn")
set(zhilian_biao "// 从本次交付头文件生成，确保每个公开函数都实际参与链接。\n")
foreach(hang IN LISTS zhilian_shengming)
    string(REGEX REPLACE ".*WINAPI\\* FsDm_([A-Za-z0-9_]+)Fn.*" "\\1" ming "${hang}")
    string(APPEND zhilian_biao "ZHILIAN_HANSHU(${ming})\n")
endforeach()
file(STRINGS "${fsdm_root}/ZhuaTu/ZhuaTu.h" zhilian_zhuatu_shengming REGEX "^ZT_API .*WINAPI [A-Za-z0-9_]+\\(")
foreach(hang IN LISTS zhilian_zhuatu_shengming)
    string(REGEX REPLACE ".*WINAPI ([A-Za-z0-9_]+)\\(.*" "\\1" ming "${hang}")
    string(APPEND zhilian_biao "ZHILIAN_HANSHU(${ming})\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ZhilianBiao.inc" "${zhilian_biao}")

# 使用 Windows 子系统创建无控制台 EXE，以便检查两个 DLL 会不会自行打开工作台。
add_executable(JietuZhilianCeshi WIN32 JietuZhilianCeshi.cpp)
target_include_directories(JietuZhilianCeshi PRIVATE "${FSDM_JIAOFU_DIR}" "${fsdm_root}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(JietuZhilianCeshi PRIVATE cxx_std_20)
target_compile_definitions(JietuZhilianCeshi PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(JietuZhilianCeshi PRIVATE /W4 /utf-8 /permissive- /EHsc)
set_property(TARGET JietuZhilianCeshi PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(JietuZhilianCeshi PRIVATE
    "${FSDM_JIAOFU_DIR}/ZhaoTu.lib" "${FSDM_JIAOFU_DIR}/ZhuaTu.lib"
    d3d11 dxgi windowscodecs ole32 user32)
set(zhilian_shuchu "${CMAKE_CURRENT_BINARY_DIR}/ZhilianYanzheng")
set_target_properties(JietuZhilianCeshi PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${zhilian_shuchu}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${zhilian_shuchu}")
foreach(ming ZhaoTu ZhuaTu)
    add_custom_command(TARGET JietuZhilianCeshi POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${FSDM_JIAOFU_DIR}/${ming}.dll" "$<TARGET_FILE_DIR:JietuZhilianCeshi>/${ming}.dll")
endforeach()
add_test(NAME jietu_zhi_lian COMMAND JietuZhilianCeshi)
set_tests_properties(jietu_zhi_lian PROPERTIES TIMEOUT 90)
