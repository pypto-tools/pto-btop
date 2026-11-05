# Modified for btop-cluster: rendering compatibility and terminal tests.
# SPDX-License-Identifier: Apache-2.0

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest
)
FetchContent_MakeAvailable(googletest)

add_library(libbtop_test)
target_include_directories(libbtop_test PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(libbtop_test libbtop GTest::gtest_main)

add_executable(btop_test cpu_names.cpp tools.cpp render_compatibility.cpp)
target_link_libraries(btop_test libbtop_test)

include(GoogleTest)
gtest_discover_tests(btop_test)

if(BTOP_CLUSTER)
  add_executable(cluster_render_test cluster_render.cpp ../src/cluster/version.cpp)
  target_link_libraries(cluster_render_test PRIVATE cluster_view GTest::gtest_main)
  gtest_discover_tests(cluster_render_test)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME cluster_terminal COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_terminal.py $<TARGET_FILE:btop-cluster> $<TARGET_FILE:btop>)
endif()

if(BTOP_CLUSTER)
  add_executable(cluster_collection_test cluster_collection.cpp)
  target_link_libraries(cluster_collection_test PRIVATE cluster_collector GTest::gtest_main)
  gtest_discover_tests(cluster_collection_test)
  add_test(NAME cluster_live COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_live.py $<TARGET_FILE:btop-agent> $<TARGET_FILE:btop-cluster>)
  set_tests_properties(cluster_live PROPERTIES TIMEOUT 90)
endif()

if(BTOP_CLUSTER)
  add_test(NAME cluster_https COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_https.py $<TARGET_FILE:btop-server>
    $<TARGET_FILE:btop-agent> $<TARGET_FILE:btop-cluster>
    ${PROJECT_SOURCE_DIR}/scripts/https-init.py)
  set_tests_properties(cluster_https PROPERTIES TIMEOUT 150)
endif()

if(BTOP_CLUSTER AND BTOP_CLUSTER_STATIC_CXX_RUNTIME)
  add_test(NAME cluster_packaging COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cluster_packaging.py ${PROJECT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR} ${CMAKE_COMMAND})
  set_tests_properties(cluster_packaging PROPERTIES TIMEOUT 120)
endif()
