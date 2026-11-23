cmake_minimum_required(VERSION 3.16)
project(canary_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/components/canary_core)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/version.txt CN_VERSION)
string(STRIP "${CN_VERSION}" CN_VERSION)

file(GLOB CORE_SRC ${CORE}/src/*.c)
add_library(canary_core STATIC ${CORE_SRC} third_party/cJSON/cJSON.c)
target_include_directories(canary_core PUBLIC ${CORE}/include third_party/cJSON)
target_compile_definitions(canary_core PUBLIC CN_FW_VERSION="${CN_VERSION}" _GNU_SOURCE)
target_compile_options(canary_core PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter)
set_source_files_properties(third_party/cJSON/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(canary_core PUBLIC m)

# Shared pieces of the simulator: a modelled car on a fake CAN bus.
add_library(canary_sim STATIC sim/simcar.c sim/host_platform.c)
target_include_directories(canary_sim PUBLIC sim)
target_compile_options(canary_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(canary_sim PUBLIC canary_core pthread)

add_executable(canary-sim sim/main.c sim/http_server.c)
target_compile_options(canary-sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(canary-sim PRIVATE canary_sim)

enable_testing()
add_executable(canary-tests tests/test_main.c)
target_compile_options(canary-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(canary-tests PRIVATE REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(canary-tests PRIVATE canary_sim)
add_test(NAME core COMMAND canary-tests)
