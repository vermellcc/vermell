cmake_minimum_required(VERSION 3.14)
project(vermell VERSION 1.0.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # MSVC has no GCC-style hardening flags; keep the equivalent basics.
    set(CMAKE_CXX_FLAGS_DEBUG "/W4 /permissive- /utf-8 /EHsc /Zi /FS")
    set(CMAKE_CXX_FLAGS_RELEASE "/W4 /permissive- /utf-8 /EHsc /O2")
elseif(WIN32) # MinGW / Clang on Windows
    set(VERMELL_HARDENING_FLAGS "-fstack-protector-strong")
    set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -pedantic -Wnon-virtual-dtor -O3 -g ${VERMELL_HARDENING_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -pedantic -Wnon-virtual-dtor -O3 ${VERMELL_HARDENING_FLAGS}")
else()
    # -fPIE is redundant on Apple (PIE is the toolchain default and the
    # driver would forward -pie to dylib links, which ld64 rejects).
    set(VERMELL_HARDENING_FLAGS "-fstack-protector-strong")
    if(NOT APPLE)
        list(APPEND VERMELL_HARDENING_FLAGS "-fPIE")
    endif()
    # _FORTIFY_SOURCE only on Linux: AppleClang predefines it and warns on
    # redefinition; glibc's fortify headers are what it actually hardens.
    if(NOT APPLE)
        list(APPEND VERMELL_HARDENING_FLAGS "-D_FORTIFY_SOURCE=2")
    endif()
    set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wnull-dereference -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 -g ${VERMELL_HARDENING_FLAGS}")
    set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 ${VERMELL_HARDENING_FLAGS}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        string(APPEND CMAKE_CXX_FLAGS_DEBUG " -fsanitize=address")
    endif()
endif()

# -pie/-z relro are ELF linkers; ld64 (macOS) and lld-link (Windows) reject them.
if(NOT WIN32 AND NOT APPLE)
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-pie -Wl,-z,relro,-z,now")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-pie -Wl,-z,relro,-z,now")
endif()

file(GLOB_RECURSE SOURCES
        "${CMAKE_SOURCE_DIR}/sources/*.cpp"
)

file(GLOB_RECURSE HEADERS
        "${CMAKE_SOURCE_DIR}/include/vermell/*.h"
)
# Shared when -DBUILD_SHARED_LIBS=ON (Debian), static otherwise (GitHub CI default).
add_library(vermell ${SOURCES} ${HEADERS})
set_target_properties(vermell PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)
target_include_directories(vermell PUBLIC "${CMAKE_SOURCE_DIR}/include")

# Winsock2 for every socket call; PSAPI for Process::memory_usage().
# PUBLIC so executables linking vermell pick them up too.
if(WIN32)
    target_link_libraries(vermell PUBLIC ws2_32)
    target_link_libraries(vermell PUBLIC psapi)
endif()

option(TESTING "Enable testing" OFF)
if (TESTING)

    enable_testing()
    add_executable(main tests/test_cases.cpp)
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

endif ()

include(GNUInstallDirs)
install(TARGETS vermell
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/vermell" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_SOURCE_DIR}/README.md" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/vermell)


if(NOT TARGET uninstall AND EXISTS "${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake.in")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/uninstall.cmake.in"
        "${CMAKE_BINARY_DIR}/uninstall.cmake"
        IMMEDIATE @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/uninstall.cmake"
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
