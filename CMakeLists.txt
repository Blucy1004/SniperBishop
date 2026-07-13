cmake_minimum_required(VERSION 3.16)
project(SniperBishop VERSION 2.61 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(SniperBishop src/sniper_bishop.cpp)

if(MSVC)
    target_compile_options(SniperBishop PRIVATE /O2 /EHsc /permissive-)
    target_compile_definitions(SniperBishop PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(SniperBishop PRIVATE -O3 -DNDEBUG)
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(SniperBishop PRIVATE pthread)
endif()

add_custom_command(
    TARGET SniperBishop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/networks/firstnet_v3.snnue"
            "$<TARGET_FILE_DIR:SniperBishop>/firstnet_v3.snnue"
    COMMENT "Copying FIRST_NET v3 beside the engine executable"
)
