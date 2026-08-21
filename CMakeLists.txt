# Top-level ESP-IDF project file for Bugne.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bugne)

# At -O2, GCC's deeper maybe-uninitialized analysis raises a false positive in the
# vendored sendspin-cpp (std::optional<std::string> in GroupUpdateObject), which
# IDF's default -Werror=maybe-uninitialized turns into a build failure. Keep the
# warning but do not treat it as an error so the optimized build links.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
# Same class of false positive since sendspin-cpp 0.7.0: GCC 14 flags a range-for
# over an ArduinoJson JsonArrayConst returned by value (protocol.cpp
# supported_commands) as a dangling reference. It is not (the array is a view).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=dangling-reference" APPEND)

# Patch the vendored sendspin-cpp after the component manager fetches it. The
# managed_components/ tree is not tracked by git and is re-downloaded on a clean
# build, so this re-applies our fix every configure. Since 0.7.0 the library
# handles the WS handshake natively; the one remaining local fix enlarges the
# httpd task stack to 8 KB (the 4 KB default corrupts the LWIP select semaphore
# under load). Idempotent: only applied when the marker is absent. See patches/
# and AGENTS.md.
set(_ss_dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/sendspin__sendspin-cpp")
set(_ss_ws "${_ss_dir}/src/esp/ws_server.cpp")
set(_ss_patch "${CMAKE_CURRENT_LIST_DIR}/patches/sendspin-cpp-0.7.2-esp-httpd-stack.patch")
if(EXISTS "${_ss_ws}")
    file(READ "${_ss_ws}" _ss_ws_src)
    string(FIND "${_ss_ws_src}" "config.stack_size = 8192" _ss_marker)
    if(_ss_marker EQUAL -1)
        execute_process(
            COMMAND patch -p1 -i "${_ss_patch}"
            WORKING_DIRECTORY "${_ss_dir}"
            RESULT_VARIABLE _ss_rc OUTPUT_VARIABLE _ss_out ERROR_VARIABLE _ss_out)
        if(NOT _ss_rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply sendspin-cpp patch:\n${_ss_out}")
        endif()
        message(STATUS "Applied sendspin-cpp httpd stack patch")
    endif()
endif()
