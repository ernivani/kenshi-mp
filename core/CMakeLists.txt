# core/ — KenshiMP plugin DLL loaded by RE_Kenshi
#
# Build requirements:
#   - Visual Studio 2019+ with Windows 7.1 SDK toolset
#   - KenshiLib (headers + lib)
#   - KenshiLib_Examples_deps (Ogre, Boost, MinHook, MyGUI)
#   - ENet

add_library(KenshiMP SHARED
    src/plugin.cpp
    src/client.cpp
    src/game_hooks.cpp
    src/player_sync.cpp
    src/npc_manager.cpp
    src/ui.cpp
)

target_compile_features(KenshiMP PRIVATE cxx_std_17)

# Common protocol headers
target_link_libraries(KenshiMP PRIVATE kenshi-mp-common)

# KenshiLib (game API + hooking)
if(KENSHILIB_DIR)
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DIR}/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE KenshiLib)
endif()

# KenshiLib deps root (contains Ogre, Boost, MinHook, MyGUI)
if(KENSHILIB_DEPS_DIR)
    # Ogre3D
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DEPS_DIR}/ogre/OgreMain/include
        ${KENSHILIB_DEPS_DIR}/ogre/build/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/ogre/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE OgreMain)

    # Boost
    target_include_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/boost_1_60_0)
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/boost_1_60_0/lib64-msvc-10.0)

    # MyGUI
    target_include_directories(KenshiMP PRIVATE
        ${KENSHILIB_DEPS_DIR}/mygui/MyGUIEngine/include
        ${KENSHILIB_DEPS_DIR}/mygui/build/include
    )
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/mygui/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE MyGUIEngine)

    # MinHook (used by KenshiLib::AddHook)
    target_include_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/minhook/include)
    target_link_directories(KenshiMP PRIVATE ${KENSHILIB_DEPS_DIR}/minhook/build/lib/Release)
    target_link_libraries(KenshiMP PRIVATE MinHook)
else()
    # Fallback: use individual dir variables
    if(OGRE_DIR)
        target_include_directories(KenshiMP PRIVATE ${OGRE_DIR}/include/OGRE)
        target_link_directories(KenshiMP PRIVATE ${OGRE_DIR}/lib)
        target_link_libraries(KenshiMP PRIVATE OgreMain)
    endif()

    if(BOOST_ROOT)
        target_include_directories(KenshiMP PRIVATE ${BOOST_ROOT})
        target_link_directories(KenshiMP PRIVATE ${BOOST_ROOT}/lib64-msvc-10.0)
    endif()

    if(MYGUI_DIR)
        target_include_directories(KenshiMP PRIVATE ${MYGUI_DIR}/include)
        target_link_directories(KenshiMP PRIVATE ${MYGUI_DIR}/lib)
        target_link_libraries(KenshiMP PRIVATE MyGUIEngine)
    endif()
endif()

# ENet
if(ENET_DIR)
    target_include_directories(KenshiMP PRIVATE ${ENET_DIR}/include)
    target_link_directories(KenshiMP PRIVATE ${ENET_DIR}/lib)
    target_link_libraries(KenshiMP PRIVATE enet)
endif()

# Windows libs (networking for ENet, input for GetAsyncKeyState)
target_link_libraries(KenshiMP PRIVATE ws2_32 winmm)

# Output: KenshiMP.dll (no "lib" prefix)
set_target_properties(KenshiMP PROPERTIES
    PREFIX ""
    OUTPUT_NAME "KenshiMP"
)
