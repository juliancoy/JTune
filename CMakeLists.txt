cmake_minimum_required(VERSION 3.16)
project(JTuneAutotune LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
option(JTUNE_BUILD_QT_GUI "Build the Qt GUI app" ON)
option(JTUNE_BUILD_IMGUI_UI "Build the Dear ImGui live-effects app" ON)

find_package(Qt6 COMPONENTS Core Gui Network REQUIRED)
find_package(Vulkan REQUIRED)

add_subdirectory(Loiacono/rtaudio)

add_library(loiacono_algos STATIC
    pitch_system.cpp
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
    JTUNE_BUNDLED_PITCH_SYSTEM_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data/ord-cc32\"
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

if(JTUNE_BUILD_IMGUI_UI)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
    find_package(OpenGL REQUIRED)

    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 56b37bf93c3cbead5f2d4d4369848415a3b0a8b2
    )
    FetchContent_GetProperties(imgui)
    if(NOT imgui_POPULATED)
        FetchContent_Populate(imgui)
    endif()

    add_library(jtune_imgui_glfw_opengl3 STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(jtune_imgui_glfw_opengl3 PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(jtune_imgui_glfw_opengl3 PRIVATE -Wno-stringop-overflow)
    endif()
    target_link_libraries(jtune_imgui_glfw_opengl3 PUBLIC OpenGL::GL PkgConfig::GLFW3)

    add_executable(jtune_imgui
        autotune_core.cpp
        effects.cpp
        Loiacono/pitch_tracker.cpp
        imgui_ui.cpp
    )
    target_include_directories(jtune_imgui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
        ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
    )
    target_link_libraries(jtune_imgui PRIVATE
        rtaudio
        loiacono_algos
        jtune_imgui_glfw_opengl3
        OpenGL::GL
        PkgConfig::GLFW3
    )
endif()

enable_testing()

add_executable(test_pitch_system
    tests/test_pitch_system.cpp
)
target_include_directories(test_pitch_system PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pitch_system PRIVATE loiacono_algos)
add_test(NAME pitch_system COMMAND test_pitch_system)

add_executable(test_effects
    effects.cpp
    tests/test_effects.cpp
)
target_include_directories(test_effects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME effects COMMAND test_effects)

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

add_executable(test_streaming_autotune
    autotune_core.cpp
    Loiacono/pitch_tracker.cpp
    tests/test_streaming_autotune.cpp
)

target_include_directories(test_streaming_autotune PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono/rtaudio
)

target_link_libraries(test_streaming_autotune PRIVATE loiacono_algos rtaudio)

add_test(NAME streaming_autotune COMMAND test_streaming_autotune)

add_executable(test_resynthesis_parity
    autotune_core.cpp
    tests/test_resynthesis_parity.cpp
)

target_include_directories(test_resynthesis_parity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(test_resynthesis_parity PRIVATE loiacono_algos)

add_test(NAME resynthesis_parity COMMAND test_resynthesis_parity)
set_tests_properties(resynthesis_parity PROPERTIES TIMEOUT 20)

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

add_executable(test_frequency_domain_quality
    autotune_core.cpp
    tests/test_frequency_domain_quality.cpp
)

target_include_directories(test_frequency_domain_quality PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Loiacono
)

target_link_libraries(test_frequency_domain_quality PRIVATE loiacono_algos)

add_test(NAME frequency_domain_quality COMMAND test_frequency_domain_quality)
set_tests_properties(loiacono_analyze_synthesize_passthrough PROPERTIES TIMEOUT 15)

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
