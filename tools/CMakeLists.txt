add_executable(test-client test_client.cpp)
target_compile_features(test-client PRIVATE cxx_std_17)
target_link_libraries(test-client PRIVATE kenshi-mp-common)

add_executable(test-snapshot-packets test_snapshot_packets.cpp)
target_compile_features(test-snapshot-packets PRIVATE cxx_std_17)
target_link_libraries(test-snapshot-packets PRIVATE kenshi-mp-common)

add_executable(test-snapshot-store test_snapshot_store.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp)
target_include_directories(test-snapshot-store PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src)
target_link_libraries(test-snapshot-store PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-store PRIVATE cxx_std_17)

add_executable(test-snapshot-upload test_snapshot_upload.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot_upload.cpp)
target_include_directories(test-snapshot-upload PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-snapshot-upload PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-upload PRIVATE cxx_std_17)

set_source_files_properties(${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c PROPERTIES LANGUAGE C)
add_executable(test-snapshot-zip test_snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/core/src/snapshot_zip.cpp
    ${CMAKE_SOURCE_DIR}/deps/miniz/miniz.c)
target_include_directories(test-snapshot-zip PRIVATE
    ${CMAKE_SOURCE_DIR}/core/src
    ${CMAKE_SOURCE_DIR}/deps/miniz)
target_link_libraries(test-snapshot-zip PRIVATE kenshi-mp-common)
target_compile_features(test-snapshot-zip PRIVATE cxx_std_17)

add_executable(test-http-sidecar test_http_sidecar.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/snapshot.cpp
    ${CMAKE_SOURCE_DIR}/server/core/src/http_sidecar.cpp)
target_include_directories(test-http-sidecar PRIVATE
    ${CMAKE_SOURCE_DIR}/server/core/src
    ${CMAKE_SOURCE_DIR}/deps/cpp-httplib
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-http-sidecar PRIVATE kenshi-mp-common ws2_32)
target_compile_features(test-http-sidecar PRIVATE cxx_std_17)

add_executable(test-snapshot-e2e test_snapshot_e2e.cpp)
target_include_directories(test-snapshot-e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/deps/cpp-httplib
    ${CMAKE_SOURCE_DIR}/deps/picosha2)
target_link_libraries(test-snapshot-e2e PRIVATE kenshi-mp-common ws2_32 winmm)
target_compile_features(test-snapshot-e2e PRIVATE cxx_std_17)
if(ENET_DIR)
    target_include_directories(test-snapshot-e2e PRIVATE ${ENET_DIR}/include)
    target_link_directories(test-snapshot-e2e PRIVATE
        ${ENET_DIR}/lib ${ENET_DIR}/build/Release ${ENET_DIR}/build/Debug)
    target_link_libraries(test-snapshot-e2e PRIVATE enet)
endif()

if(ENET_DIR)
    target_include_directories(test-client PRIVATE ${ENET_DIR}/include)
    target_link_directories(test-client PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
    )
    target_link_libraries(test-client PRIVATE enet)
endif()

target_link_libraries(test-client PRIVATE ws2_32 winmm)
