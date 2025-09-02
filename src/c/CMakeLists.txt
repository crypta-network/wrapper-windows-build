cmake_minimum_required(VERSION 3.20)
project(JavaServiceWrapperNative C)

if(NOT WIN32)
  message(FATAL_ERROR "This CMake file is for Windows builds only.")
endif()

# Output folders relative to repo root (src/c/../../bin and lib)
get_filename_component(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${REPO_ROOT}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${REPO_ROOT}/lib)

# Common definitions
add_definitions(
  -DWIN32
  -DUNICODE
  -D_UNICODE
  -D_CRT_SECURE_NO_DEPRECATE
  -D_WIN32_WINNT=0x0501
)

include_directories(
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
)

# JNI headers
# Prefer CMake's FindJNI (works with or without JAVA_HOME),
# but also fall back to $ENV{JAVA_HOME} if available.
find_package(JNI)
if(JNI_FOUND)
  message(STATUS "Found JNI: ${JNI_INCLUDE_DIRS}")
  include_directories(${JNI_INCLUDE_DIRS})
elseif(DEFINED ENV{JAVA_HOME})
  message(STATUS "JNI via JAVA_HOME: $ENV{JAVA_HOME}")
  include_directories($ENV{JAVA_HOME}/include $ENV{JAVA_HOME}/include/win32)
else()
  message(FATAL_ERROR "JNI headers not found. Install a JDK and set JAVA_HOME or ensure Java is in PATH.")
endif()

set(WRAPPER_COMMON_SOURCES
  wrapper.c
  wrapperinfo.c
  wrappereventloop.c
  wrapper_jvm_launch.c
  property.c
  logger.c
  logger_file.c
  wrapper_file.c
  wrapper_i18n.c
  wrapper_hashmap.c
  wrapper_ulimit.c
  wrapper_encoding.c
  wrapper_jvminfo.c
  wrapper_secure_file.c
  wrapper_cipher.c
  wrapper_cipher_base.c
)

# Executable (wrapper.exe)
add_executable(wrapper_exe
  ${WRAPPER_COMMON_SOURCES}
  wrapper_win.c
  Wrapper.rc
)
set_target_properties(wrapper_exe PROPERTIES OUTPUT_NAME wrapper)
target_link_libraries(wrapper_exe PRIVATE
  mpr
  shell32
  netapi32
  wsock32
  Ws2_32
  shlwapi
  advapi32
  user32
  Crypt32
  Wintrust
  pdh
  psapi
  ole32
  OleAut32
  activeds
  adsiid
)

# JNI DLL (wrapper.dll)
add_library(wrapper_dll SHARED
  wrapper_i18n.c
  wrapperjni_win.c
  wrapperinfo.c
  wrapperjni.c
  loggerjni.c
  Wrapper.rc
)
# Ensure MSVC treats wrapperinfo.c as C (not C++)
set_source_files_properties(wrapperinfo.c PROPERTIES LANGUAGE C)
set_target_properties(wrapper_dll PROPERTIES OUTPUT_NAME wrapper)
target_link_libraries(wrapper_dll PRIVATE
  shell32
  wsock32
  advapi32
  user32
  Iphlpapi
)
