# common/ — header-only protocol library shared by core, server, and injector
add_library(kenshi-mp-common INTERFACE)

target_include_directories(kenshi-mp-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# C++11 for compatibility with VS2010/v100 toolset used by the core DLL
target_compile_features(kenshi-mp-common INTERFACE cxx_std_11)
