# injector/ — KenshiMP launcher (modern C++17)
add_executable(kenshi-mp-launcher
    src/main.cpp
)

target_compile_features(kenshi-mp-launcher PRIVATE cxx_std_17)
target_link_libraries(kenshi-mp-launcher PRIVATE kenshi-mp-common)
