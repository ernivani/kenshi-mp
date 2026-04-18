# server/headless/ — thin console wrapper around the core DLL.

add_executable(kenshi-mp-server-headless src/main.cpp)

target_compile_features(kenshi-mp-server-headless PRIVATE cxx_std_17)
target_link_libraries(kenshi-mp-server-headless PRIVATE kenshi-mp-server-core)

set_target_properties(kenshi-mp-server-headless PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
