if(CROWDY_WITH_OPENSSL)
  add_executable(wire_test wire_test.cpp)
  target_link_libraries(wire_test PRIVATE crowdy_wire crowdy_core crowdy_crypto_openssl)
  add_test(NAME wire_test COMMAND wire_test)

  add_executable(core_test core_test.cpp)
  target_link_libraries(core_test PRIVATE crowdy_core crowdy_crypto_openssl)
  add_test(NAME core_test COMMAND core_test)
endif()

add_executable(json_test json_test.cpp)
target_link_libraries(json_test PRIVATE crowdy_graphql)
add_test(NAME json_test COMMAND json_test)

add_executable(datacenter_redirect_test datacenter_redirect_test.cpp)
target_link_libraries(datacenter_redirect_test PRIVATE crowdy_graphql)
add_test(NAME datacenter_redirect_test COMMAND datacenter_redirect_test)

add_executable(rediscover_test rediscover_test.cpp)
target_link_libraries(rediscover_test PRIVATE crowdy_graphql Threads::Threads)
add_test(NAME rediscover_test COMMAND rediscover_test)

add_executable(estate_test estate_test.cpp)
target_link_libraries(estate_test PRIVATE crowdy_graphql)
add_test(NAME estate_test COMMAND estate_test)

add_executable(graphql_client_test graphql_client_test.cpp)
target_link_libraries(graphql_client_test PRIVATE crowdy_graphql)
if(CROWDY_WITH_CURL)
  target_link_libraries(graphql_client_test PRIVATE crowdy_http_curl)
  target_compile_definitions(graphql_client_test PRIVATE
    CROWDY_TEST_WITH_CURL=1)
endif()
add_test(NAME graphql_client_test COMMAND graphql_client_test)

add_executable(async_lifetime_test async_lifetime_test.cpp)
target_link_libraries(async_lifetime_test PRIVATE crowdy)
add_test(NAME async_lifetime_test COMMAND async_lifetime_test)

if(NOT CROWDY_NO_EXCEPTIONS)
  add_executable(graphql_subscription_test graphql_subscription_test.cpp)
  target_link_libraries(graphql_subscription_test PRIVATE crowdy)
  add_test(NAME graphql_subscription_test COMMAND graphql_subscription_test)
endif()

if(CROWDY_NO_EXCEPTIONS)
  add_executable(no_exceptions_umbrella_test no_exceptions_umbrella_test.cpp)
  target_link_libraries(no_exceptions_umbrella_test PRIVATE crowdy)
  add_test(NAME no_exceptions_umbrella_test COMMAND no_exceptions_umbrella_test)
  add_test(
    NAME no_exceptions_install_test
    COMMAND "${CMAKE_COMMAND}"
      "-DCROWDY_BUILD_DIR=${PROJECT_BINARY_DIR}"
      "-DCROWDY_CMAKE_COMMAND=${CMAKE_COMMAND}"
      "-DCROWDY_CONFIG=$<CONFIG>"
      "-DCROWDY_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DCROWDY_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      -P "${PROJECT_SOURCE_DIR}/tests/no_exceptions_install_test.cmake")
  set_tests_properties(no_exceptions_install_test PROPERTIES TIMEOUT 300)
endif()

if(NOT CROWDY_NO_EXCEPTIONS)
  add_executable(player_runtime_surface_test player_runtime_surface_test.cpp)
  target_link_libraries(player_runtime_surface_test PRIVATE crowdy)
  add_test(NAME player_runtime_surface_test COMMAND player_runtime_surface_test)
endif()

add_executable(public_api_parity_test public_api_parity_test.cpp)
target_link_libraries(public_api_parity_test PRIVATE crowdy)
add_test(NAME public_api_parity_test COMMAND public_api_parity_test)

add_executable(studio_layout_test studio_layout_test.cpp)
target_include_directories(studio_layout_test PRIVATE
  "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(studio_layout_test PRIVATE crowdy)
add_test(NAME studio_layout_test COMMAND studio_layout_test)

add_executable(client_portable_test client_portable_test.cpp)
target_link_libraries(client_portable_test PRIVATE crowdy)
add_test(NAME client_portable_test COMMAND client_portable_test)

if(NOT CROWDY_NO_EXCEPTIONS)
  if(CROWDY_WITH_OPENSSL)
    add_executable(crowdy_studio_test crowdy_studio_test.cpp)
    target_link_libraries(crowdy_studio_test PRIVATE crowdy)
    add_test(NAME crowdy_studio_test COMMAND crowdy_studio_test)

    add_executable(crowdy_studio_state_test crowdy_studio_state_test.cpp)
    target_link_libraries(crowdy_studio_state_test PRIVATE crowdy)
    target_compile_definitions(crowdy_studio_state_test PRIVATE
      CROWDY_STUDIO_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tools/parity/fixtures")
    add_test(NAME crowdy_studio_state_test COMMAND crowdy_studio_state_test)
  endif()

  add_executable(agent_registry_test agent_registry_test.cpp)
  target_link_libraries(agent_registry_test PRIVATE crowdy)
  add_test(NAME agent_registry_test COMMAND agent_registry_test)

  add_executable(agent_api_test agent_api_test.cpp)
  target_link_libraries(agent_api_test PRIVATE crowdy)
  add_test(NAME agent_api_test COMMAND agent_api_test)

  add_executable(agent_controller_test agent_controller_test.cpp)
  target_link_libraries(agent_controller_test PRIVATE crowdy)
  add_test(NAME agent_controller_test COMMAND agent_controller_test)

  add_executable(player_host_test player_host_test.cpp)
  target_link_libraries(player_host_test PRIVATE crowdy)
  add_test(NAME player_host_test COMMAND player_host_test)

  add_executable(studio_integration_test studio_integration_test.cpp)
  target_link_libraries(studio_integration_test PRIVATE crowdy)
  target_compile_definitions(studio_integration_test PRIVATE
    CROWDY_PARITY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tools/parity/fixtures")
  add_test(NAME studio_integration_test COMMAND studio_integration_test)

  add_executable(native_control_gate_test native_control_gate_test.cpp)
  target_link_libraries(native_control_gate_test PRIVATE crowdy)
  target_compile_definitions(native_control_gate_test PRIVATE
    CROWDY_PARITY_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tools/parity/fixtures")
  add_test(NAME native_control_gate_test COMMAND native_control_gate_test)
endif()

add_executable(codec_struct_test codec_struct_test.cpp)
target_link_libraries(codec_struct_test PRIVATE crowdy_core)
add_test(NAME codec_struct_test COMMAND codec_struct_test)

# Deliberately not gated on WIN32: the send-error classification is the one
# part of the replication send path that can be tested on Windows, where
# replication_test is not built and where WSAEWOULDBLOCK actually occurs.
# Reaches into src/ for the internal (uninstalled) classification header.
add_executable(socket_send_test socket_send_test.cpp)
target_link_libraries(socket_send_test PRIVATE crowdy_core)
target_include_directories(socket_send_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME socket_send_test COMMAND socket_send_test)

if(NOT CROWDY_NO_EXCEPTIONS)
  add_executable(kit_test kit_test.cpp)
  target_link_libraries(kit_test PRIVATE crowdy)
  add_test(NAME kit_test COMMAND kit_test)

  add_executable(kit_wire_test kit_wire_test.cpp)
  target_link_libraries(kit_wire_test PRIVATE crowdy)
  add_test(NAME kit_wire_test COMMAND kit_wire_test)
endif()

if(NOT WIN32 AND CROWDY_WITH_OPENSSL)
  add_executable(replication_test replication_test.cpp)
  target_link_libraries(replication_test PRIVATE crowdy_replication crowdy_crypto_openssl)
  add_test(NAME replication_test COMMAND replication_test)

  add_executable(session_test session_test.cpp)
  target_link_libraries(session_test PRIVATE crowdy)
  add_test(NAME session_test COMMAND session_test)
endif()

if(NOT CROWDY_WITH_OPENSSL)
  add_executable(crypto_off_test crypto_off_test.cpp)
  target_link_libraries(crypto_off_test PRIVATE crowdy)
  add_test(NAME crypto_off_test COMMAND crypto_off_test)
endif()

if(NOT CROWDY_NO_EXCEPTIONS)
  include("${PROJECT_SOURCE_DIR}/cmake/CheckPublicHeaders.cmake")
  crowdy_add_public_header_test(public_headers_test)
endif()

if(CROWDY_BUILD_E2E AND CROWDY_WITH_OPENSSL AND
   NOT CROWDY_NO_EXCEPTIONS)
  add_subdirectory(e2e)
endif()
