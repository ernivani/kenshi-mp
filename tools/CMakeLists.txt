add_executable(test-client test_client.cpp)
target_compile_features(test-client PRIVATE cxx_std_17)
target_link_libraries(test-client PRIVATE kenshi-mp-common)

if(ENET_DIR)
    target_include_directories(test-client PRIVATE ${ENET_DIR}/include)
    target_link_directories(test-client PRIVATE
        ${ENET_DIR}/lib
        ${ENET_DIR}/build/Release
    )
    target_link_libraries(test-client PRIVATE enet)
endif()

target_link_libraries(test-client PRIVATE ws2_32 winmm)
