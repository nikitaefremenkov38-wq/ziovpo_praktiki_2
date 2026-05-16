cmake_minimum_required(VERSION 3.20)

project(WitcherTraySuite LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RPC_IDL ${CMAKE_CURRENT_SOURCE_DIR}/rpc/WitcherControl.idl)
set(MIDL_OUT ${CMAKE_CURRENT_BINARY_DIR}/generated/rpc)

set(RPC_HEADER ${MIDL_OUT}/WitcherControl_h.h)
set(RPC_CLIENT ${MIDL_OUT}/WitcherControl_c.c)
set(RPC_SERVER ${MIDL_OUT}/WitcherControl_s.c)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MIDL_ENV x64)
else()
    set(MIDL_ENV win32)
endif()

add_custom_command(
    OUTPUT ${RPC_HEADER} ${RPC_CLIENT} ${RPC_SERVER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MIDL_OUT}
    COMMAND midl /nologo /app_config /env ${MIDL_ENV} /char signed /h WitcherControl_h.h /out ${MIDL_OUT} ${RPC_IDL}
    DEPENDS ${RPC_IDL}
    COMMENT "Generating Windows RPC stubs with MIDL"
    VERBATIM
)

add_custom_target(generate_rpc_stubs
    DEPENDS ${RPC_HEADER} ${RPC_CLIENT} ${RPC_SERVER}
)

add_executable(TrayWin32App WIN32
    src/main.cpp
    ${RPC_CLIENT}
    resources/app.rc
)

add_executable(WitcherTrayService
    src/service.cpp
    ${RPC_SERVER}
)

add_dependencies(TrayWin32App generate_rpc_stubs)
add_dependencies(WitcherTrayService generate_rpc_stubs)

target_include_directories(TrayWin32App PRIVATE
    include
    ${MIDL_OUT}
)

target_include_directories(WitcherTrayService PRIVATE
    include
    ${MIDL_OUT}
)

target_compile_definitions(TrayWin32App PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

target_compile_definitions(WitcherTrayService PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
)

target_link_libraries(TrayWin32App PRIVATE
    advapi32
    rpcrt4
    shell32
)

target_link_libraries(WitcherTrayService PRIVATE
    advapi32
    rpcrt4
    wtsapi32
    userenv
)

if(MSVC)
    target_compile_options(TrayWin32App PRIVATE /W4 /permissive-)
    target_compile_options(WitcherTrayService PRIVATE /W4 /permissive-)
endif()

install(TARGETS TrayWin32App WitcherTrayService RUNTIME DESTINATION .)
