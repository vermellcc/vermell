# Cross-compile toolchain for armhf (32-bit ARMv7, hard-float)
# Used by CI (deb-package.yml) to build libvermell_<ver>_armhf.deb
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_AR arm-linux-gnueabihf-ar)
set(CMAKE_RANLIB arm-linux-gnueabihf-ranlib)
set(CMAKE_STRIP arm-linux-gnueabihf-strip)

set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
