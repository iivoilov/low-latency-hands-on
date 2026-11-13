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
add_library(processor_core processors/cpp/src/processor.cpp)
target_include_directories(processor_core PUBLIC processors/cpp/include)
target_link_libraries(processor_core PUBLIC simulator_core)
target_compile_options(processor_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)

add_executable(processor_cpp processors/cpp/src/main.cpp)
target_include_directories(processor_cpp PRIVATE simulator/src)
target_link_libraries(processor_cpp PRIVATE processor_core Threads::Threads)
target_compile_options(processor_cpp PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(processor_cpp PRIVATE PROCESSOR_BUILD_TYPE="$<CONFIG>")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(wire_rx_probe benchmark/wire_rx_probe.cpp)
    target_include_directories(wire_rx_probe PRIVATE simulator/src)
    target_link_libraries(wire_rx_probe PRIVATE processor_core Threads::Threads)
    target_compile_options(wire_rx_probe PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(simulator_tests simulator/tests/test_simulator.cpp)
    target_link_libraries(simulator_tests PRIVATE simulator_core)
    target_compile_options(simulator_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME simulator_core COMMAND simulator_tests)
    add_executable(processor_tests processors/cpp/tests/test_processor.cpp)
    target_link_libraries(processor_tests PRIVATE processor_core Threads::Threads)
    target_compile_options(processor_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
    add_test(NAME processor_core COMMAND processor_tests)
    set_tests_properties(processor_core PROPERTIES TIMEOUT 30)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
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
