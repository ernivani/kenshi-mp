# common/ — header-only protocol library shared by core, server, and injector
add_library(kenshi-mp-common INTERFACE)

target_include_directories(kenshi-mp-common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# C++17 for std::string_view, structured bindings, etc.
target_compile_features(kenshi-mp-common INTERFACE cxx_std_17)
