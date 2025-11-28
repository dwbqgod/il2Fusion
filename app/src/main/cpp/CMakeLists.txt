cmake_minimum_required(VERSION 3.22.1)

project("TextExtractTool")

# 加入 Dobby 子工程
add_subdirectory(Dobby)

# 你的 native 库
add_library(localization_tool SHARED
        native-lib.cpp
        hook.cpp
)

# 添加头文件路径
target_include_directories(localization_tool PRIVATE
        ${CMAKE_SOURCE_DIR}/Dobby/include
)

# 链接 dobby
target_link_libraries(
        localization_tool
        dobby
        log
)