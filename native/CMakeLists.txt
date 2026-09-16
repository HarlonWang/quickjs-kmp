cmake_minimum_required(VERSION 3.22)
project(quickjs_kmp C)

# Host-side JNI library so Android host tests can exercise the real JNI path on the build machine
option(QJS_HOST_JNI "build the JNI shared library for the host JVM" OFF)
option(QJS_SHIM_TEST "build the host shim test binary (native/test)" OFF)
option(QJS_TOOLS "build the host command line tools (native/tools)" OFF)
option(QJS_ASAN "compile everything with AddressSanitizer" OFF)

set(QJS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/quickjs)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/UPSTREAM QJS_UPSTREAM_LINE REGEX "^commit=")
string(REGEX REPLACE "^commit=" "" QJS_UPSTREAM_COMMIT "${QJS_UPSTREAM_LINE}")
string(STRIP "${QJS_UPSTREAM_COMMIT}" QJS_UPSTREAM_COMMIT)
string(LENGTH "${QJS_UPSTREAM_COMMIT}" QJS_UPSTREAM_COMMIT_LEN)
if(NOT QJS_UPSTREAM_COMMIT_LEN EQUAL 40)
    message(FATAL_ERROR "native/UPSTREAM must contain a full 40-char commit= line, got '${QJS_UPSTREAM_COMMIT}'")
endif()

file(STRINGS ${QJS_ROOT}/VERSION QJS_VERSION LIMIT_COUNT 1)
string(STRIP "${QJS_VERSION}" QJS_VERSION)

add_library(quickjs_kmp_static STATIC
    ${QJS_ROOT}/quickjs.c
    ${QJS_ROOT}/libregexp.c
    ${QJS_ROOT}/libunicode.c
    ${QJS_ROOT}/cutils.c
    ${QJS_ROOT}/dtoa.c
    shim/quickjs_kmp.c
)
set_target_properties(quickjs_kmp_static PROPERTIES OUTPUT_NAME quickjs_kmp)
target_include_directories(quickjs_kmp_static
    PUBLIC shim
    PRIVATE ${QJS_ROOT}
)
target_compile_definitions(quickjs_kmp_static PRIVATE
    _GNU_SOURCE
    CONFIG_VERSION="${QJS_VERSION}"
    KMPJS_UPSTREAM_COMMIT="${QJS_UPSTREAM_COMMIT}"
)
target_compile_options(quickjs_kmp_static PRIVATE -Os -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -fno-math-errno -fno-trapping-math -fvisibility=hidden)
if(QJS_ASAN)
    target_compile_options(quickjs_kmp_static PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(quickjs_kmp_static PUBLIC -fsanitize=address)
endif()

if(ANDROID)
    add_library(quickjs_kmp SHARED jni/quickjs_jni.c)
    target_compile_options(quickjs_kmp PRIVATE -Os -Wall -fvisibility=hidden)
    target_link_libraries(quickjs_kmp PRIVATE quickjs_kmp_static m)
    target_link_options(quickjs_kmp PRIVATE -Wl,--exclude-libs,ALL -Wl,--strip-all)
elseif(QJS_HOST_JNI)
    # QJS_JNI_INCLUDE lets the build pass the JDK's include dirs directly (FindJNI insists on AWT on macOS)
    if(QJS_JNI_INCLUDE)
        set(JNI_INCLUDE_DIRS ${QJS_JNI_INCLUDE})
    else()
        find_package(JNI REQUIRED COMPONENTS Development)
    endif()
    add_library(quickjs_kmp SHARED jni/quickjs_jni.c)
    target_compile_options(quickjs_kmp PRIVATE -Os -Wall -fvisibility=hidden)
    target_include_directories(quickjs_kmp PRIVATE ${JNI_INCLUDE_DIRS})
    target_link_libraries(quickjs_kmp PRIVATE quickjs_kmp_static m)
endif()

if(QJS_SHIM_TEST)
    add_executable(shim_test test/shim_test.c)
    target_compile_options(shim_test PRIVATE -Wall)
    target_link_libraries(shim_test PRIVATE quickjs_kmp_static m)
endif()

if(QJS_TOOLS)
    add_executable(qjsc-kmp tools/qjsc_kmp.c)
    target_compile_options(qjsc-kmp PRIVATE -Wall)
    target_link_libraries(qjsc-kmp PRIVATE quickjs_kmp_static m)
endif()
