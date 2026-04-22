# core/ — KenshiMP plugin DLL loaded by RE_Kenshi
#
# MUST be compiled with VS2010 (v100) x64 toolset for KenshiLib ABI compatibility.
# Pass -T v100 to cmake, or set CMAKE_GENERATOR_TOOLSET=v100.

add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/host_sync.cpp
    src/building_sync.cpp
    src/building_manager.cpp
    src/admin_panel.cpp
    src/ui.cpp
    src/client_identity.cpp
    src/save_trigger.cpp
    src/snapshot_zip.cpp
    src/snapshot_uploader.cpp
    src/snapshot_uploader_glue.cpp
    src/server_list.cpp
    src/server_pinger.cpp
    ${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c
)

# miniz — force C language so MSVC generator emits ClCompile, not None
set_source_files_properties(${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c
    PROPERTIES LANGUAGE C)
target_include_directories(KenshiMP PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/miniz
    ${CMAKE_SOURCE_DIR}/deps/picosha2)

# KenshiLib PerfTimer.h uses wide-string CreateMutex
target_compile_definitions(KenshiMP PRIVATE UNICODE _UNICODE)

# C++11 — required by v100 toolset
set_target_properties(KenshiMP PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
    OUTPUT_NAME "KenshiMP"
)

# Common protocol headers
target_link_libraries(KenshiMP PRIVATE kenshi-mp-common)

# KenshiLib (game API + hooking)
if(KENSHILIB_DIR)
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/Include
        ${KENSHILIB_DIR}/Include/ogre
        ${KENSHILIB_DIR}/Include/mygui
    )
    target_link_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/Libraries
        ${KENSHILIB_DIR}/Libraries/ogre
        ${KENSHILIB_DIR}/Libraries/mygui
    )
    if(KENSHILIB_EXAMPLES_DEPS)
        target_link_directories(KenshiMP PRIVATE
            ${KENSHILIB_EXAMPLES_DEPS}/KenshiLib/Libraries
        )
    endif()
    target_link_libraries(KenshiMP PRIVATE KenshiLib OgreMain_x64 MyGUIEngine_x64)
endif()

# Boost
if(BOOST_ROOT)
    target_include_directories(KenshiMP PRIVATE ${BOOST_ROOT})
    target_link_directories(KenshiMP PRIVATE
        ${BOOST_ROOT}/stage/lib
    )
endif()

# ENet
if(ENET_DIR)
    target_include_directories(KenshiMP PRIVATE ${ENET_DIR}/include)
    target_link_directories(KenshiMP PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
    )
    target_link_libraries(KenshiMP PRIVATE enet)
endif()

# Windows libs
target_link_libraries(KenshiMP PRIVATE ws2_32 winmm shell32)
