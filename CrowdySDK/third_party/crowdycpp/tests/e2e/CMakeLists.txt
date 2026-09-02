# Env-gated end-to-end tests. They connect to a deployment you configure via
# CROWDY_E2E_* environment variables and are skipped (exit 77) when unset.
#
# Labels:
#   e2e          default black-box suites (ctest -L e2e)
#   e2e_slow     long-running suites (soak, cache-TTL waits); also gated on
#                CROWDY_E2E_SLOW=1
#   e2e_optional suites needing extra deployment features (operator account,
#                multiple replication servers); gated on their own env vars
file(GLOB E2E_SOURCES CONFIGURE_DEPENDS *.cpp)

set(E2E_SLOW_SUITES e2e_permission_refresh e2e_soak_two_clients)
set(E2E_OPTIONAL_SUITES
  e2e_agentic_studio
  e2e_crowdy_studio
  e2e_native_studio_integration
  e2e_cross_server
  e2e_graphql_websocket
  e2e_marketplace_claims
  e2e_operator
)

foreach(src ${E2E_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE crowdy)
  add_test(NAME ${name} COMMAND ${name})
  if(name IN_LIST E2E_SLOW_SUITES)
    set_tests_properties(${name} PROPERTIES LABELS "e2e_slow" TIMEOUT 900)
  elseif(name IN_LIST E2E_OPTIONAL_SUITES)
    set_tests_properties(${name} PROPERTIES LABELS "e2e_optional" TIMEOUT 300)
  else()
    set_tests_properties(${name} PROPERTIES LABELS "e2e" TIMEOUT 300)
  endif()
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
