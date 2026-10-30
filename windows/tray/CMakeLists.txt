cmake_minimum_required(VERSION 3.14)

add_executable(FlClashTray WIN32 tray.cpp)
apply_standard_settings(FlClashTray)
set_target_properties(FlClashTray PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runner/$<CONFIG>")
target_compile_definitions(FlClashTray PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(FlClashTray PRIVATE advapi32 rasapi32 shell32 user32 wininet winhttp)

install(TARGETS FlClashTray RUNTIME DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
  COMPONENT Runtime)
