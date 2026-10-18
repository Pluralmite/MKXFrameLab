cmake_minimum_required(VERSION 3.16)
project(MKXFrameLab CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Mortal Kombat X is 64-bit: configure for x64.")
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(MINHOOK_DIR ${CMAKE_SOURCE_DIR}/external/minhook)

add_library(MKXFrameLab SHARED
  src/dllmain.cpp
  src/memory.cpp
  src/config.cpp
  src/game.cpp
  src/boxes.cpp
  src/ui.cpp
  src/log.cpp
  src/mkx.cpp
  src/trace.cpp
  src/buffer.cpp
  src/bot.cpp
  src/movelist.cpp
  src/dinput_block.cpp
  src/screenshot.cpp
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${MINHOOK_DIR}/src/buffer.c
  ${MINHOOK_DIR}/src/hook.c
  ${MINHOOK_DIR}/src/trampoline.c
  ${MINHOOK_DIR}/src/hde/hde64.c
)

target_include_directories(MKXFrameLab PRIVATE
  src ${IMGUI_DIR} ${IMGUI_DIR}/backends ${MINHOOK_DIR}/include)

target_compile_definitions(MKXFrameLab PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(MKXFrameLab PRIVATE windowscodecs ole32 uuid dxguid xinput9_1_0 d3d11 dxgi d3dcompiler dwmapi psapi imm32)

# The mod itself is MKXFrameLab_core.dll (hot-reloadable); MKXFrameLab.asi is a small loader that
# Ultimate ASI Loader picks up and that loads / reloads the core.
set_target_properties(MKXFrameLab PROPERTIES PREFIX "" OUTPUT_NAME "MKXFrameLab_core" SUFFIX ".dll")

add_library(MKXFrameLabLoader SHARED src/loader.cpp)
set_target_properties(MKXFrameLabLoader PROPERTIES PREFIX "" OUTPUT_NAME "MKXFrameLab" SUFFIX ".asi")
target_compile_definitions(MKXFrameLabLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

if(MINGW)
  target_link_options(MKXFrameLabLoader PRIVATE -static -static-libgcc -static-libstdc++ -s)
  target_compile_options(MKXFrameLabLoader PRIVATE -O2)
  target_link_options(MKXFrameLab PRIVATE -static -static-libgcc -static-libstdc++ -s)
  target_compile_options(MKXFrameLab PRIVATE -O2)
endif()
if(MSVC)
  set_property(TARGET MKXFrameLab PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
