cmake_minimum_required(VERSION 3.20)
project(KenshiMP VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Global options
# ---------------------------------------------------------------------------
option(KENSHIMP_BUILD_CORE     "Build the Ogre plugin DLL (requires VS2010 toolchain)" ON)
option(KENSHIMP_BUILD_SERVER   "Build the dedicated server"                             ON)
option(KENSHIMP_BUILD_INJECTOR "Build the launcher / injector"                          ON)

# ---------------------------------------------------------------------------
# Paths to external dependencies (override via -D or environment)
# ---------------------------------------------------------------------------
set(KENSHILIB_DIR "" CACHE PATH "Path to KenshiLib root (include/ and lib/)")
set(OGRE_DIR      "" CACHE PATH "Path to Ogre3D SDK (from Kenshi or standalone)")
set(BOOST_ROOT    "" CACHE PATH "Path to Boost 1.60+ root")
set(ENET_DIR      "" CACHE PATH "Path to ENet root (include/ and lib/)")
set(KENSHILIB_DEPS_DIR "" CACHE PATH "Path to KenshiLib_Examples_deps root")
set(MYGUI_DIR     "" CACHE PATH "Path to MyGUI SDK")

# ---------------------------------------------------------------------------
# Common (header-only protocol library)
# ---------------------------------------------------------------------------
add_subdirectory(common)

# ---------------------------------------------------------------------------
# Core DLL — Ogre plugin, needs VS2010 toolchain + KenshiLib + Ogre + Boost
# ---------------------------------------------------------------------------
if(KENSHIMP_BUILD_CORE)
    add_subdirectory(core)
endif()

# ---------------------------------------------------------------------------
# Dedicated server — modern C++17, only needs ENet + nlohmann/json
# ---------------------------------------------------------------------------
if(KENSHIMP_BUILD_SERVER)
    add_subdirectory(server)
endif()

# ---------------------------------------------------------------------------
# Injector / launcher — modern C++17, no game dependencies
# ---------------------------------------------------------------------------
if(KENSHIMP_BUILD_INJECTOR)
    add_subdirectory(injector)
endif()

# ---------------------------------------------------------------------------
# Test tools
# ---------------------------------------------------------------------------
add_subdirectory(tools)
