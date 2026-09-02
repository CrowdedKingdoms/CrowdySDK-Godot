add_executable(walker walker.cpp)
target_link_libraries(walker PRIVATE crowdy)

add_executable(kit_seed_and_play kit_seed_and_play.cpp)
target_link_libraries(kit_seed_and_play PRIVATE crowdy)

add_executable(native_studio_shell native_studio_shell.cpp)
target_link_libraries(native_studio_shell PRIVATE crowdy)
