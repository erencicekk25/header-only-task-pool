add_executable(basic_usage basic_usage.cpp)
target_link_libraries(basic_usage PRIVATE taskpool::taskpool)
taskpool_configure_target(basic_usage)
