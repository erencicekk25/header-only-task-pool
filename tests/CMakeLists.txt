add_executable(taskpool_tests
    test_main.cpp
    test_task_queue.cpp
    test_task_pool.cpp)

target_link_libraries(taskpool_tests PRIVATE taskpool::taskpool)
taskpool_configure_target(taskpool_tests)

add_test(NAME taskpool_tests COMMAND taskpool_tests)
