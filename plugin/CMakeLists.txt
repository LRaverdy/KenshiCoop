set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)

add_library(KenshiCoop SHARED main.cpp kenshi.cpp world.cpp hooks.cpp overlay.cpp util.cpp debug.cpp
    steam_link.cpp host_console.cpp factions.cpp doors.cpp prisons.cpp ranged.cpp buildings.cpp map.cpp overlay_map.cpp)
target_link_libraries(KenshiCoop PRIVATE kc_common minhook imgui d3d11 dxgi bcrypt dinput8 dxguid ws2_32 comctl32)
target_compile_options(KenshiCoop PRIVATE /W4 /WX /permissive- /Zi)
target_link_options(KenshiCoop PRIVATE /DEBUG /OPT:REF /OPT:ICF)
