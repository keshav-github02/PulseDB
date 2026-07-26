# SDK Simulator module: a client library that generates realistic playback
# traffic and delivers it to a collector, plus a thin driver executable.

add_library(pulsedb_sdk
    src/session_generator.cpp
    src/url.cpp
    src/http_event_sink.cpp
    src/backoff.cpp
    src/retrying_sender.cpp
    src/spool_store.cpp
    src/spooling_sender.cpp
    src/simulator.cpp)
add_library(pulsedb::sdk ALIAS pulsedb_sdk)

target_include_directories(pulsedb_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(pulsedb_sdk
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        httplib::httplib
        pulsedb::warnings)

target_compile_features(pulsedb_sdk PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(pulsedb_sdk PRIVATE ws2_32)
endif()

# --- Driver executable -----------------------------------------------------
add_executable(pulsedb_sdk_app app/main.cpp)
set_target_properties(pulsedb_sdk_app PROPERTIES OUTPUT_NAME pulsedb_sdk)

target_link_libraries(pulsedb_sdk_app
    PRIVATE
        pulsedb::sdk
        nlohmann_json::nlohmann_json
        pulsedb::warnings)
