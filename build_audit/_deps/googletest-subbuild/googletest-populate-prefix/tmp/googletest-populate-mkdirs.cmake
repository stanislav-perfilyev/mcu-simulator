# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-src")
  file(MAKE_DIRECTORY "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-src")
endif()
file(MAKE_DIRECTORY
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-build"
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix"
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/tmp"
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp"
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/src"
  "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/sessions/elegant-compassionate-tesla/mnt/Устройство на работу/GitHub-repos/mcu-simulator/build_audit/_deps/googletest-subbuild/googletest-populate-prefix/src/googletest-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
