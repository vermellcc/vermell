cmake_minimum_required(VERSION 3.14)
project(vermell VERSION 1.0.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VERMELL_HARDENING_FLAGS "-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE")

set(CMAKE_CXX_FLAGS_DEBUG "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wnull-dereference -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 -g -fsanitize=address ${VERMELL_HARDENING_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -pedantic -Wpedantic -Wnon-virtual-dtor -Wnull-dereference -Wstrict-overflow=5 -Wmissing-include-dirs -Wstrict-aliasing=2 -O3 ${VERMELL_HARDENING_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-pie -Wl,-z,relro,-z,now")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-pie -Wl,-z,relro,-z,now")

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

option(TESTING "Enable testing" OFF)
if (TESTING)

    enable_testing()
    add_executable(main tests/test_cases.cpp)
    target_link_libraries(
            main
            vermell
    )

    include(FetchContent)


    find_package(GTest  QUIET)

    if (NOT GTest_FOUND)

        message(STATUS "extracting https://github.com/google/googletest.git")
        FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG v1.15.2
        )
        FetchContent_MakeAvailable(googletest)
        message(STATUS "using -> build/ [gtest]")
    else ()
        message(STATUS "#### using local GTest: In case of error verify your installation")
    endif ()

    target_link_libraries(main GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(main)


    find_package(CURL QUIET)
    if ( NOT CURL_FOUND )

        message( STATUS "extracting https://github.com/curl/curl.git")
        FetchContent_Declare(
                curl
                GIT_REPOSITORY https://github.com/curl/curl.git
                GIT_TAG 83bedbd
        )
        FetchContent_MakeAvailable(curl)
        message(STATUS "using -> build/ [curl]")
    else ()
        message(STATUS "#### using local curl: In case of error with .so.4 verify your installation curl --version")
    endif ()

    target_link_libraries(main CURL::libcurl)

endif ()

include(GNUInstallDirs)
install(TARGETS vermell
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/vermell" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES "${CMAKE_SOURCE_DIR}/README.md" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/vermell)


if(NOT TARGET uninstall)
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
