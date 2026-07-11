# Top-level ESP-IDF project file for Bugne.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bugne)

# At -O2, GCC's deeper maybe-uninitialized analysis raises a false positive in the
# vendored sendspin-cpp (std::optional<std::string> in GroupUpdateObject), which
# IDF's default -Werror=maybe-uninitialized turns into a build failure. Keep the
# warning but do not treat it as an error so the optimized build links.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)

# Patch the vendored sendspin-cpp after the component manager fetches it. The
# managed_components/ tree is not tracked by git and is re-downloaded on a clean
# build, so this re-applies our fix every configure. The patch adds the IDF
# post-handshake callback (the player sends client/hello, so Music Assistant
# registers it) and enlarges the httpd task stack (fixes the LWIP select crash).
# Idempotent: only applied when the marker is absent. See patches/ and AGENTS.md.
set(_ss_dir "${CMAKE_CURRENT_LIST_DIR}/managed_components/sendspin__sendspin-cpp")
set(_ss_ws "${_ss_dir}/src/esp/ws_server.cpp")
set(_ss_patch "${CMAKE_CURRENT_LIST_DIR}/patches/sendspin-cpp-0.6.1-esp-ws.patch")
if(EXISTS "${_ss_ws}")
    file(READ "${_ss_ws}" _ss_ws_src)
    string(FIND "${_ss_ws_src}" "ws_post_handshake" _ss_marker)
    if(_ss_marker EQUAL -1)
        execute_process(
            COMMAND patch -p1 -i "${_ss_patch}"
            WORKING_DIRECTORY "${_ss_dir}"
            RESULT_VARIABLE _ss_rc OUTPUT_VARIABLE _ss_out ERROR_VARIABLE _ss_out)
        if(NOT _ss_rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply sendspin-cpp patch:\n${_ss_out}")
        endif()
        message(STATUS "Applied sendspin-cpp ESP WebSocket patch")
    endif()
endif()
