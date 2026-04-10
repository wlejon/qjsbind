add_executable(qjsbind_test test_main.cpp)
target_link_libraries(qjsbind_test PRIVATE qjsbind)
set_target_properties(qjsbind_test PROPERTIES CXX_STANDARD 20)
add_test(NAME qjsbind_test COMMAND qjsbind_test)
