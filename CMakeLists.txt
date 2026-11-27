cmake_minimum_required(VERSION 3.20)
project(TinyBrowser CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(tinybrowser WIN32
  src/main.cpp
  src/common.cpp
  src/url.cpp
  src/html.cpp
  src/css.cpp
  src/svg.cpp
  src/text.cpp
  src/layout_engine.cpp
  src/render.cpp
  src/http.cpp
  src/storage.cpp
  src/gpu.cpp
  src/shell.cpp
  src/script.cpp
  src/selftest.cpp
  src/js/parser.cpp
  src/js/compiler.cpp
  src/js/vm.cpp
  src/js/builtins.cpp
  src/js/builtins2.cpp
  src/js/regex.cpp
  src/js/regexp.cpp
  src/js/typedarray.cpp
  src/js/module.cpp
  src/js/proxy.cpp
  src/js/shadowrealm.cpp
  src/js/bigint.cpp
  src/js/prelude.cpp
  src/js/array.cpp
  src/js/props.cpp
  src/js/asyncgen.cpp
  src/js/jsrun.cpp
)

target_compile_definitions(tinybrowser PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(tinybrowser PRIVATE /W3 /utf-8 /permissive- /EHsc /MP $<$<CONFIG:Release>:/O2 /GL /Gy>)
target_link_options(tinybrowser PRIVATE /STACK:67108864 $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
target_link_libraries(tinybrowser PRIVATE d3d11 dxgi d2d1 dwrite windowscodecs winhttp crypt32 bcrypt ole32 user32 gdi32 shell32)
