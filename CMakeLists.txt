cmake_minimum_required(VERSION 3.20)
project(low_latency LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT UNIX)
    message(FATAL_ERROR "The simulator currently requires POSIX IPv4 sockets")
endif()

add_library(simulator_core simulator/src/pitch.cpp simulator/src/pcap.cpp simulator/src/traffic.cpp)
target_include_directories(simulator_core PUBLIC simulator/include)
target_compile_options(simulator_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(prepare_feed simulator/src/prepare.cpp)
add_executable(play_feed simulator/src/play.cpp)
foreach(program prepare_feed play_feed)
    target_link_libraries(${program} PRIVATE simulator_core)
    target_compile_options(${program} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endforeach()

find_package(Threads REQUIRED)
add_library(processor_core processors/cpp/src/processor.cpp processors/cpp/src/pitch_message.cpp
    processors/cpp/src/pitch_message_packed.cpp processors/cpp/src/pitch_message_zero_copy.cpp processors/cpp/src/l1.cpp processors/cpp/src/recovery.cpp)
target_include_directories(processor_core PUBLIC processors/cpp/include)
target_link_libraries(processor_core PUBLIC simulator_core)
target_compile_options(processor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(processor_cpp processors/cpp/src/main.cpp)
target_include_directories(processor_cpp PRIVATE simulator/src)
target_link_libraries(processor_cpp PRIVATE processor_core Threads::Threads)
target_compile_options(processor_cpp PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(processor_cpp PRIVATE PROCESSOR_BUILD_TYPE="$<CONFIG>")

add_library(recovery_archive simulator/src/recovery_archive.cpp)
target_link_libraries(recovery_archive PUBLIC processor_core)
target_compile_options(recovery_archive PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
add_executable(serve_feed simulator/src/serve.cpp)
target_include_directories(serve_feed PRIVATE simulator/src)
target_link_libraries(serve_feed PRIVATE recovery_archive Threads::Threads)
target_compile_options(serve_feed PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(pitch_decode_bench benchmark/pitch_decode.cpp)
target_link_libraries(pitch_decode_bench PRIVATE processor_core)
target_compile_options(pitch_decode_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(pitch_decode_bench PRIVATE DECODER_BUILD_TYPE="$<CONFIG>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(wire_rx_probe benchmark/wire_rx_probe.cpp)
    target_include_directories(wire_rx_probe PRIVATE simulator/src)
    target_link_libraries(wire_rx_probe PRIVATE processor_core Threads::Threads)
    target_compile_options(wire_rx_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(recovery_archive_tests simulator/tests/test_recovery_archive.cpp)
    target_link_libraries(recovery_archive_tests PRIVATE recovery_archive)
    target_compile_options(recovery_archive_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME recovery_archive COMMAND recovery_archive_tests)
    add_executable(simulator_tests simulator/tests/test_simulator.cpp)
    target_link_libraries(simulator_tests PRIVATE simulator_core)
    target_compile_options(simulator_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME simulator_core COMMAND simulator_tests)
    add_executable(processor_tests processors/cpp/tests/test_processor.cpp)
    target_link_libraries(processor_tests PRIVATE processor_core Threads::Threads)
    target_compile_options(processor_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME processor_core COMMAND processor_tests)
    set_tests_properties(processor_core PROPERTIES TIMEOUT 30)
    add_executable(pitch_l1_tests processors/cpp/tests/test_pitch_l1.cpp)
    target_link_libraries(pitch_l1_tests PRIVATE processor_core)
    target_compile_options(pitch_l1_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME pitch_l1 COMMAND pitch_l1_tests ${CMAKE_CURRENT_SOURCE_DIR}/processors/tests/equities_messages.tsv)
    set_tests_properties(pitch_l1 PROPERTIES TIMEOUT 30)
    add_executable(pitch_decoder_tests processors/cpp/tests/test_pitch_decoders.cpp)
    target_link_libraries(pitch_decoder_tests PRIVATE processor_core)
    target_compile_options(pitch_decoder_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME pitch_decoders COMMAND pitch_decoder_tests ${CMAKE_CURRENT_SOURCE_DIR}/processors/tests/equities_messages.tsv)
    set_tests_properties(pitch_decoders PROPERTIES TIMEOUT 30)
    add_test(NAME pitch_decoder_bench_smoke COMMAND pitch_decode_bench
        --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/processors/tests/equities_messages.tsv
        --messages 23 --warmup 23 --repeats 1)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME simulator_universe COMMAND ${Python3_EXECUTABLE} -m unittest simulator.tests.test_import_sp500)
        set_tests_properties(simulator_universe PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME monitoring_core COMMAND ${Python3_EXECUTABLE} -m unittest monitoring.tests.test_core)
        set_tests_properties(monitoring_core PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME monitoring_replay COMMAND ${Python3_EXECUTABLE} -m monitoring.tests.test_replay
            $<TARGET_FILE:processor_cpp> $<TARGET_FILE:prepare_feed> $<TARGET_FILE:play_feed>)
        set_tests_properties(monitoring_replay PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            LABELS network TIMEOUT 45)
        add_test(NAME processor_recovery COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/processors/tests/test_recovery.py
            $<TARGET_FILE:processor_cpp> $<TARGET_FILE:prepare_feed> $<TARGET_FILE:serve_feed>)
        set_tests_properties(processor_recovery PROPERTIES LABELS network TIMEOUT 90)
        add_test(NAME pitch_decoder_bench_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/processors/cpp/tests/test_decode_bench.py
            $<TARGET_FILE:pitch_decode_bench> ${CMAKE_CURRENT_SOURCE_DIR}/processors/tests/equities_messages.tsv)
        add_test(NAME simulator_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/simulator/tests/test_cli.py $<TARGET_FILE:prepare_feed> $<TARGET_FILE:play_feed>)
        add_test(NAME simulator_multicast COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/simulator/tests/test_cli.py $<TARGET_FILE:prepare_feed> $<TARGET_FILE:play_feed> --network)
        set_tests_properties(simulator_multicast PROPERTIES LABELS network TIMEOUT 20)
        add_test(NAME processor_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/processors/cpp/tests/test_cli.py
            $<TARGET_FILE:processor_cpp> $<TARGET_FILE:prepare_feed> $<TARGET_FILE:play_feed>)
        add_test(NAME processor_multicast COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/processors/cpp/tests/test_cli.py
            $<TARGET_FILE:processor_cpp> $<TARGET_FILE:prepare_feed> $<TARGET_FILE:play_feed> --network)
        set_tests_properties(processor_multicast PROPERTIES LABELS network TIMEOUT 25)
    endif()
endif()
