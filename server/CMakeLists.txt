# server/ — Dedicated KenshiMP server (modern C++17)
add_executable(kenshi-mp-server
    src/main.cpp
    src/session.cpp
    src/world_state.cpp
    src/relay.cpp
)

target_compile_features(kenshi-mp-server PRIVATE cxx_std_17)
target_link_libraries(kenshi-mp-server PRIVATE kenshi-mp-common)

# ENet
if(ENET_DIR)
    target_include_directories(kenshi-mp-server PRIVATE ${ENET_DIR}/include)
    target_link_directories(kenshi-mp-server PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
        ${ENET_DIR}/build/Debug
    )
    target_link_libraries(kenshi-mp-server PRIVATE enet)
endif()

# Windows networking
target_link_libraries(kenshi-mp-server PRIVATE ws2_32 winmm)

# nlohmann/json (header-only, fetch if not available)
include(FetchContent)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)
target_link_libraries(kenshi-mp-server PRIVATE nlohmann_json::nlohmann_json)

# spdlog for logging
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.13.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)
target_link_libraries(kenshi-mp-server PRIVATE spdlog::spdlog)
