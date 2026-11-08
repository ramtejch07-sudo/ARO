cmake_minimum_required(VERSION 3.14)
project(AmbulanceRouteOptimization CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ARO_BUILD_SERVER "Build the REST API server (ambulance_server)" ON)
option(ARO_BUILD_TESTS  "Build the unit tests (downloads GoogleTest)"   ON)

if(MSVC)
  set(ARO_WARNINGS /W4)
else()
  set(ARO_WARNINGS -Wall -Wextra)
endif()

# ---- Core domain library (everything in src/ except the console demo's main) ----
file(GLOB ARO_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM ARO_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
add_library(aro_core STATIC ${ARO_CORE_SOURCES})
target_include_directories(aro_core PUBLIC include)
target_compile_options(aro_core PRIVATE ${ARO_WARNINGS})

# ---- Console demo ----
add_executable(ambulance_sim src/main.cpp)
target_link_libraries(ambulance_sim PRIVATE aro_core)
target_compile_options(ambulance_sim PRIVATE ${ARO_WARNINGS})

# ---- REST API ----
if(ARO_BUILD_SERVER OR ARO_BUILD_TESTS)
  find_package(Threads REQUIRED)
  add_library(aro_api STATIC server/ApiRoutes.cpp server/Database.cpp server/OsrmRoutingProvider.cpp)
  # SYSTEM: don't show -Wall noise from the vendored single-header libraries (httplib, json).
  target_include_directories(aro_api SYSTEM PUBLIC server)
  target_link_libraries(aro_api PUBLIC aro_core Threads::Threads)
  target_compile_options(aro_api PRIVATE ${ARO_WARNINGS})
  if(WIN32)
    target_link_libraries(aro_api PUBLIC ws2_32)
    target_compile_definitions(aro_api PUBLIC _WIN32_WINNT=0x0A00)
  endif()
endif()

if(ARO_BUILD_SERVER)
  add_executable(ambulance_server server/ApiServer.cpp)
  target_link_libraries(ambulance_server PRIVATE aro_api)
  target_compile_options(ambulance_server PRIVATE ${ARO_WARNINGS})
endif()

# ---- Tests ----
if(ARO_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)   # use extraction time for downloaded archives
  endif()
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
  FetchContent_MakeAvailable(googletest)

  add_executable(aro_tests tests/test_core.cpp tests/test_api.cpp tests/test_api_geo.cpp tests/test_routing.cpp)
  target_include_directories(aro_tests PRIVATE tests)
  target_link_libraries(aro_tests PRIVATE aro_api GTest::gtest_main)
  target_compile_options(aro_tests PRIVATE ${ARO_WARNINGS})

  include(GoogleTest)
  gtest_discover_tests(aro_tests)
endif()
