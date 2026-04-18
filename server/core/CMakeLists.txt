# server/core/ — kenshi-mp-server-core SHARED library.

add_library(kenshi-mp-server-core SHARED
    src/core.cpp
    src/events.cpp
    src/session.cpp
    src/world_state.cpp
    src/relay.cpp
    src/server_config.cpp
    src/admin.cpp
    src/spawn.cpp
)

target_compile_features(kenshi-mp-server-core PRIVATE cxx_std_17)
target_compile_definitions(kenshi-mp-server-core PRIVATE KMP_SERVER_CORE_EXPORTS)

target_include_directories(kenshi-mp-server-core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(kenshi-mp-server-core PRIVATE kenshi-mp-common)

if(ENET_DIR)
    target_include_directories(kenshi-mp-server-core PRIVATE ${ENET_DIR}/include)
    target_link_directories(kenshi-mp-server-core PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
        ${ENET_DIR}/build/Debug
    )
    target_link_libraries(kenshi-mp-server-core PRIVATE enet)
endif()

target_link_libraries(kenshi-mp-server-core PRIVATE ws2_32 winmm)
target_link_libraries(kenshi-mp-server-core PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Force predictable DLL name (no "lib" prefix on MSVC anyway, but set OUTPUT_NAME).
set_target_properties(kenshi-mp-server-core PROPERTIES
    OUTPUT_NAME "kenshi-mp-server-core"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
