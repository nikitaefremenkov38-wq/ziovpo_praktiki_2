cmake_minimum_required(VERSION 3.20)

project(TrayWin32App LANGUAGES CXX)

add_executable(TrayWin32App WIN32
    src/main.cpp
    resources/app.rc
)

target_compile_features(TrayWin32App PRIVATE cxx_std_17)
target_compile_definitions(TrayWin32App PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

target_link_libraries(TrayWin32App PRIVATE user32 shell32 gdi32)

install(TARGETS TrayWin32App RUNTIME DESTINATION .)
