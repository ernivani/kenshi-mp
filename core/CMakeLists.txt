# core/ — Ogre3D plugin DLL that hooks into Kenshi
#
# IMPORTANT: This must be compiled with the VS2010 (v100) toolchain to be
# ABI-compatible with KenshiLib and the game's Ogre build.
# Pass -T v100 to cmake when configuring, or set CMAKE_GENERATOR_TOOLSET.

add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/ui.cpp
)

target_compile_features(KenshiMP PRIVATE cxx_std_11)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
target_link_libraries(KenshiMP PRIVATE kenshi-mp-common)

# KenshiLib
if(KENSHILIB_DIR)
    target_include_directories(KenshiMP PRIVATE ${KENSHILIB_DIR}/include)
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE KenshiLib)
endif()

# Ogre3D
if(OGRE_DIR)
    target_include_directories(KenshiMP PRIVATE ${OGRE_DIR}/include/OGRE)
    target_link_directories(KenshiMP PRIVATE ${OGRE_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE OgreMain)
endif()

# Boost (KenshiLib dependency)
if(BOOST_ROOT)
    target_include_directories(KenshiMP PRIVATE ${BOOST_ROOT})
    target_link_directories(KenshiMP PRIVATE ${BOOST_ROOT}/lib64-msvc-10.0)
endif()

# ENet
if(ENET_DIR)
    target_include_directories(KenshiMP PRIVATE ${ENET_DIR}/include)
    target_link_directories(KenshiMP PRIVATE ${ENET_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE enet)
endif()

# MyGUI
if(MYGUI_DIR)
    target_include_directories(KenshiMP PRIVATE ${MYGUI_DIR}/include)
    target_link_directories(KenshiMP PRIVATE ${MYGUI_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE MyGUIEngine MyGUI.OgrePlatform)
endif()

# Windows networking for ENet
target_link_libraries(KenshiMP PRIVATE ws2_32 winmm)

# Output name: KenshiMP.dll (no "lib" prefix)
set_target_properties(KenshiMP PROPERTIES
    PREFIX ""
    OUTPUT_NAME "KenshiMP"
)
