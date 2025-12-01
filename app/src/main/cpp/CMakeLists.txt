cmake_minimum_required(VERSION 3.22.1)

project("TextExtractTool")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(
        native_hook
        SHARED
        native_hook.cpp
)

target_include_directories(
        native_hook PRIVATE
)

target_link_libraries(
        native_hook
        log
)

add_library(DOBBY_LIB STATIC IMPORTED)

set_target_properties(DOBBY_LIB PROPERTIES IMPORTED_LOCATION
${CMAKE_CURRENT_SOURCE_DIR}/libs/${CMAKE_ANDROID_ARCH_ABI}/libdobby.a
)

target_link_libraries(native_hook DOBBY_LIB log)
