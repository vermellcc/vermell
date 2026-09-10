cmake_minimum_required(VERSION 3.14)
project(vermell VERSION 1.0.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GCC/Clang-only tuning: MSVC rejects these spellings, so they must not leak
# into Windows builds (Linux behavior below is unchanged).
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
set(VERMELL_HARDENING_FLAGS "-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE")

set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wnull-dereference -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 -g -fsanitize=address ${VERMELL_HARDENING_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wnull-dereference -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 ${VERMELL_HARDENING_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-pie -Wl,-z,relro,-z,now")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-pie -Wl,-z,relro,-z,now")
endif()

if(MSVC)
# Required: without an EH model MSVC cannot unwind C++ exceptions (the core
# throws across router/worker boundaries). /utf-8 keeps UTF-8 sources intact.
add_compile_options(/EHsc /utf-8)
endif()

# NOTE: CMAKE_CURRENT_SOURCE_DIR (not CMAKE_SOURCE_DIR) throughout: the core
# is designed to be add_subdirectory()'d by platform backends and apps, where
# the top-level source dir is some other project.
file(GLOB_RECURSE SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/sources/*.cpp"
)

file(GLOB_RECURSE HEADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/vermell/*.h"
)

# Core / Platform split: vermell_platform is a portable INTERFACE target (headers
# only); vermell_linux is the bundled Linux backend (epoll/eventfd/sockets).
# Planned excision keeps these target names (vermell::vermell / vermell::linux).
option(VERMELL_WITH_LINUX "Build bundled Linux platform backend" ON)

add_library(vermell_platform INTERFACE)
add_library(vermell::platform ALIAS vermell_platform)
target_include_directories(vermell_platform INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Compiled once here; excluded from the core glob so symbols are never defined twice.
file(GLOB_RECURSE LINUX_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/sources/net/linux/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/sources/net/linux/.*")

# Shared when -DBUILD_SHARED_LIBS=ON (Debian), static otherwise (GitHub CI default).
add_library(vermell ${SOURCES} ${HEADERS})
add_library(vermell::vermell ALIAS vermell)
set_target_properties(vermell PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
target_include_directories(vermell PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(vermell PUBLIC vermell_platform)

if(VERMELL_WITH_LINUX)
    # OBJECT (not STATIC): the backend objects merge into the single
    # distributed libvermell archive, keeping the "one header, one library"
    # promise intact for manual `-lvermell` linking.
    add_library(vermell_linux OBJECT ${LINUX_SOURCES})
    add_library(vermell::linux ALIAS vermell_linux)
    target_include_directories(vermell_linux PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(vermell_linux PUBLIC vermell_platform)
    target_sources(vermell PRIVATE $<TARGET_OBJECTS:vermell_linux>)
endif()

option(TESTING "Enable testing" OFF)
if (TESTING)

    enable_testing()
    add_executable(main tests/test_cases.cpp tests/test_platform_spi.cpp)
    target_link_libraries(
            main
            vermell
    )

    find_package(GTest REQUIRED)
    message(STATUS "using local GTest: in case of error verify your installation")

    target_link_libraries(main GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(main)

    find_package(CURL REQUIRED)
    message(STATUS "using local curl: in case of error with .so.4 verify with `curl --version`")

    target_link_libraries(main CURL::libcurl)

    # Portable SPI must stay OS-agnostic: fail on leaked <sys/*> etc. in net headers.
    find_program(SH_BIN sh)
    if(SH_BIN)
        add_test(NAME spi_portable_headers
            COMMAND sh -c "grep -rEn '#include[[:space:]]*<(sys/|netinet/|arpa/|unistd\\.h|fcntl\\.h)' '${CMAKE_CURRENT_SOURCE_DIR}/include/vermell/net/event_loop.h' '${CMAKE_CURRENT_SOURCE_DIR}/include/vermell/net/transport.h' '${CMAKE_CURRENT_SOURCE_DIR}/include/vermell/net/platform.h' && exit 1 || exit 0")
    endif()

endif ()

include(GNUInstallDirs)
install(TARGETS vermell
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/vermell" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/vermell)


if(NOT TARGET uninstall AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
        IMMEDIATE @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/uninstall.cmake"
    )
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "libvermell")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "Version of the generated package (overridable with -DCPACK_PACKAGE_VERSION=...)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "User-friendly and compact C++20 Web Framework")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/vermellcc/vermell")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Kevin Bohorquez - kevin@bohorquez.cc")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
include(CPack)
