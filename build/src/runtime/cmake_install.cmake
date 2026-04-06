# Install script for directory: C:/Projects/GitHub/craw-cpp-code/src/runtime

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/claw-cpp-code")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Projects/GitHub/craw-cpp-code/build/lib/Debug/claw_runtime.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Projects/GitHub/craw-cpp-code/build/lib/Release/claw_runtime.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Projects/GitHub/craw-cpp-code/build/lib/MinSizeRel/claw_runtime.lib")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "C:/Projects/GitHub/craw-cpp-code/build/lib/RelWithDebInfo/claw_runtime.lib")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/claw_runtime" TYPE FILE FILES
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/bash.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/bash_validation.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/bootstrap.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/compact.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/config.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/conversation.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/file_ops.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/green_contract.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/hooks.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/json_value.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/lane_events.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/lsp_client.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/mcp.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/mcp_client.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/mcp_lifecycle_hardened.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/mcp_stdio.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/mcp_tool_bridge.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/oauth.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/permissions.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/permission_enforcer.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/plugin_lifecycle.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/policy_engine.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/prompt.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/recovery_recipes.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/remote.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/sandbox.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/session.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/session_control.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/sse.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/stale_branch.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/summary_compression.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/task_packet.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/task_registry.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/team_cron_registry.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/trust_resolver.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/usage.hpp"
    "C:/Projects/GitHub/craw-cpp-code/src/runtime/include/worker_boot.hpp"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Projects/GitHub/craw-cpp-code/build/src/runtime/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
