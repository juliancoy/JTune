cmake_minimum_required(VERSION 3.16)
project(JTuneAutotune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
option(JTUNE_BUILD_QT_GUI "Build the Qt GUI app" ON)

find_package(Qt6 COMPONENTS Core Gui Network REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(Loiacono/rtaudio)

add_library(loiacono_algos STATIC
    Loiacono/loiacono_parallel.cpp
    Loiacono/loiacono_rolling.cpp
    Loiacono/loiacono_pitch_shift.cpp
    Loiacono/loiacono_gpu_compute.cpp
    Loiacono/loiacono_vulkan_compute.cpp
    Loiacono/loiacono_gpu_rolling_compute.cpp
)

target_include_directories(loiacono_algos PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_compile_definitions(loiacono_algos PRIVATE
    LOIACONO_SHADER_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/shaders\"
)

target_link_libraries(loiacono_algos PUBLIC
    Qt6::Core
    Qt6::Gui
    Vulkan::Vulkan
)

add_executable(jtune_autotune
    autotune_core.cpp
    main.cpp
)

target_include_directories(jtune_autotune PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
)

target_link_libraries(jtune_autotune PRIVATE rtaudio loiacono_algos)

add_executable(jtune_live_ui
    autotune_core.cpp
    Loiacono/pitch_tracker.cpp
    live_ui.cpp
)

target_include_directories(jtune_live_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
)

target_link_libraries(jtune_live_ui PRIVATE rtaudio loiacono_algos)

add_executable(jtune_control_server
    autotune_core.cpp
    Loiacono/pitch_tracker.cpp
    control_server.cpp
)

target_include_directories(jtune_control_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(jtune_control_server PRIVATE
    loiacono_algos
    Qt6::Core
    Qt6::Network
)

add_executable(jtune_export_chirp
    autotune_core.cpp
    chirp_export.cpp
)

target_include_directories(jtune_export_chirp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(jtune_export_chirp PRIVATE
    loiacono_algos
)

add_executable(jtune_process_wav
    autotune_core.cpp
    process_wav.cpp
)

target_include_directories(jtune_process_wav PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(jtune_process_wav PRIVATE
    loiacono_algos
)

if(JTUNE_BUILD_QT_GUI)
    find_package(Qt6 COMPONENTS Widgets Network REQUIRED)

    add_executable(jtune_qt_gui
        autotune_core.cpp
        Loiacono/pitch_tracker.cpp
        qt_gui.cpp
        Loiacono/audio_device_labels.cpp
    )
    set_target_properties(jtune_qt_gui PROPERTIES OUTPUT_NAME JTune)

    target_include_directories(jtune_qt_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
        ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
    )

    target_link_libraries(jtune_qt_gui PRIVATE
        rtaudio
        loiacono_algos
        Qt6::Widgets
        Qt6::Network
    )
endif()

enable_testing()

add_executable(test_autotune_chirp
    autotune_core.cpp
    Loiacono/pitch_tracker.cpp
    tests/test_autotune_chirp.cpp
)

target_include_directories(test_autotune_chirp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
)

target_link_libraries(test_autotune_chirp PRIVATE loiacono_algos rtaudio)

add_test(NAME autotune_chirp COMMAND test_autotune_chirp)

add_executable(test_autotune_chirp_cross_algo
    autotune_core.cpp
    Loiacono/pitch_tracker.cpp
    tests/test_autotune_chirp_cross_algo.cpp
)

target_include_directories(test_autotune_chirp_cross_algo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
)

target_link_libraries(test_autotune_chirp_cross_algo PRIVATE loiacono_algos rtaudio)

add_test(NAME autotune_chirp_cross_algo COMMAND test_autotune_chirp_cross_algo)

add_executable(test_passthrough_chirp
    autotune_core.cpp
    tests/test_passthrough_chirp.cpp
)

target_include_directories(test_passthrough_chirp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(test_passthrough_chirp PRIVATE loiacono_algos)

add_test(NAME passthrough_chirp COMMAND test_passthrough_chirp)

add_executable(test_loiacono_analyze_synthesize_passthrough
    tests/test_loiacono_analyze_synthesize_passthrough.cpp
)

target_include_directories(test_loiacono_analyze_synthesize_passthrough PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(test_loiacono_analyze_synthesize_passthrough PRIVATE loiacono_algos)

add_test(NAME loiacono_analyze_synthesize_passthrough COMMAND test_loiacono_analyze_synthesize_passthrough)

add_executable(test_phase_gpu_consistency
    tests/test_phase_gpu_consistency.cpp
)

target_include_directories(test_phase_gpu_consistency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(test_phase_gpu_consistency PRIVATE
    loiacono_algos
    Qt6::Core
)

add_test(NAME phase_gpu_consistency COMMAND test_phase_gpu_consistency)
