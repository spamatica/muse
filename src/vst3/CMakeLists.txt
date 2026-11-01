#=============================================================================
#  MusE
#  Linux Music Editor
#
#  Step 1 of the VST3 plan (see ../README.vst3): headless VST3 hosting proof of
#  concept, probe tool and tests. Stand-alone for now, it is not part of the
#  main MusE build yet.
#
#    cmake -S . -B build -DVST3_SDK_PATH=/path/to/vst3sdk
#    cmake --build build -j
#    ctest --test-dir build --output-on-failure
#=============================================================================

cmake_minimum_required(VERSION 3.25)
project(muse_vst3 CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VST3_SDK_PATH "$ENV{HOME}/dev/vst3sdk" CACHE PATH "Path to a checkout of https://github.com/steinbergmedia/vst3sdk (3.8 or later, MIT)")
if(NOT EXISTS "${VST3_SDK_PATH}/CMakeLists.txt")
  message(FATAL_ERROR "VST3_SDK_PATH does not point to the VST3 SDK: ${VST3_SDK_PATH}")
endif()

# Only the SDK libraries are needed, not the examples nor VSTGUI.
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(${VST3_SDK_PATH} vst3sdk EXCLUDE_FROM_ALL)

# sdk_hosting does not contain the Linux module loader nor PlugProvider, the SDK examples add those themselves.
set(VST3_HOSTING_DIR ${VST3_SDK_PATH}/public.sdk/source/vst/hosting)
add_library(muse_vst3host STATIC
  vst3host.cpp
  vst3host.h
  ${VST3_HOSTING_DIR}/module_linux.cpp
  ${VST3_HOSTING_DIR}/plugprovider.cpp
  ${VST3_SDK_PATH}/public.sdk/source/common/memorystream.cpp
)
target_include_directories(muse_vst3host PUBLIC ${VST3_SDK_PATH})
target_link_libraries(muse_vst3host PUBLIC sdk_hosting sdk_common sdk base pluginterfaces ${CMAKE_DL_LIBS})
target_compile_options(muse_vst3host PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(vst3probe vst3probe.cpp)
target_link_libraries(vst3probe muse_vst3host)

# Step 2: the editor in a Qt5 widget (X11). Needs Qt5 Widgets and the X11 libraries.
find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(X11 REQUIRED)
add_library(muse_vst3gui STATIC vst3editor.cpp vst3editor.h)
target_link_libraries(muse_vst3gui PUBLIC muse_vst3host Qt5::Widgets)
target_compile_options(muse_vst3gui PRIVATE -Wall -Wextra -Wno-unused-parameter)

add_executable(vst3gui vst3gui.cpp)
target_link_libraries(vst3gui muse_vst3gui)

enable_testing()
add_executable(vst3_tests tests/vst3_tests.cpp)
target_link_libraries(vst3_tests muse_vst3host)
add_test(NAME vst3_tests COMMAND vst3_tests)
set_tests_properties(vst3_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

find_library(X11_XTEST_LIB Xtst REQUIRED)
add_executable(vst3_gui_tests tests/vst3_gui_tests.cpp)
target_link_libraries(vst3_gui_tests muse_vst3gui ${X11_LIBRARIES} ${X11_XTEST_LIB})
add_test(NAME vst3_gui_tests COMMAND vst3_gui_tests)
set_tests_properties(vst3_gui_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
