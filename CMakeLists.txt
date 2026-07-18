cmake_minimum_required(VERSION 3.22)
project(powerboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- PLATFORM DETECTION ---
if(WIN32)
    set(PLATFORM_WINDOWS TRUE)
elseif(APPLE)
    set(PLATFORM_MACOS TRUE)
else()
    set(PLATFORM_LINUX TRUE)
endif()

# Platform-specific compiler flags
if(PLATFORM_LINUX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
elseif(PLATFORM_WINDOWS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
elseif(PLATFORM_MACOS)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()

find_package(ftxui CONFIG REQUIRED)

add_executable(powerboard
    src/main.cpp
)
target_link_libraries(powerboard PRIVATE ftxui::screen ftxui::dom ftxui::component sqlite3)

# --- Install & packaging (Linux only) ---
if(PLATFORM_LINUX)
    include(GNUInstallDirs)
    install(TARGETS powerboard RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/powerboard.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

    set(CPACK_PACKAGE_NAME "aura-flux")
    set(CPACK_PACKAGE_VERSION "0.2.3")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "TUI hardware, power and cost monitor with AI copilot")
    set(CPACK_PACKAGE_CONTACT "Dusan Milosavljevic <dukezr78@gmail.com>")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libsqlite3-0, libc6, libstdc++6")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "wl-clipboard, power-profiles-daemon")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    include(CPack)
endif()

# --- Platform info output ---
message(STATUS "Building for: ${CMAKE_SYSTEM_NAME}")
if(PLATFORM_LINUX)
    message(STATUS "Linux package generation: ENABLED")
elseif(PLATFORM_WINDOWS)
    message(STATUS "Windows build: native APIs stubbed (preview mode)")
elseif(PLATFORM_MACOS)
    message(STATUS "macOS build: native APIs stubbed (preview mode)")
endif()
