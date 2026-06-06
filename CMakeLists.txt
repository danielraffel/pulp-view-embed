# pulp-view-embed — flat C ABI for embedding a Pulp-imported frontend in a
# foreign C++ host. Consumes an INSTALLED Pulp SDK via find_package(Pulp CONFIG).
#
# Configure against the SDK installed from the seam branch:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
cmake_minimum_required(VERSION 3.24)
project(pulp_view_embed LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Pulp CONFIG REQUIRED)

option(PULP_VIEW_EMBED_SHARED "Build the embedding ABI as a shared library" OFF)
if(PULP_VIEW_EMBED_SHARED)
    add_library(pulp_view_embed SHARED src/pulp_view_embed.cpp)
else()
    add_library(pulp_view_embed STATIC src/pulp_view_embed.cpp)
endif()

target_include_directories(pulp_view_embed
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# Pulp::view carries the materializer + PluginViewHost; the rest are the
# subsystems the shim names directly. Skia comes transitively via Pulp::render.
target_link_libraries(pulp_view_embed PRIVATE
    Pulp::format Pulp::view Pulp::state Pulp::canvas Pulp::render)

if(APPLE)
    target_link_libraries(pulp_view_embed PRIVATE "-framework AppKit")
endif()

# ── smoke harness (M1.1–M1.5) ────────────────────────────────────────────
# macOS-only AppKit host: create from a DesignIR fixture, attach to a real
# NSView, resize, tick, capture, and stress create/destroy. Registered as a
# ctest so `ctest -R embed-smoke` runs the gates.
if(APPLE)
    enable_testing()
    add_executable(pulp-embed-smoke-mac examples/macos-nsview-smoke/smoke.mm)
    target_link_libraries(pulp-embed-smoke-mac PRIVATE
        pulp_view_embed "-framework AppKit")
    target_compile_definitions(pulp-embed-smoke-mac PRIVATE
        PULP_EMBED_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME embed-smoke COMMAND pulp-embed-smoke-mac)
endif()
