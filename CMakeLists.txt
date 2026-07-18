cmake_minimum_required(VERSION 3.22)
project(powerboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")

find_package(ftxui CONFIG REQUIRED)

add_executable(powerboard
    src/main.cpp
)
target_link_libraries(powerboard PRIVATE ftxui::screen ftxui::dom ftxui::component sqlite3)

# --- Install & packaging (Linux) ---
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
