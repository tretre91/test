if(NOT Kokkos_INSTALL_TESTING)
  add_subdirectory(src)
endif()

kokkos_add_test_directories(unit_tests)
kokkos_add_benchmark_directories(performance_tests)
