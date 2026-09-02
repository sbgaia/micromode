# Unity comes from the reactor-uc checkout rather than being vendored again. If reactor-uc's
# own test tree was configured too it already defines the target, so reuse it.
if(NOT TARGET Unity)
  if(NOT REACTOR_UC_PATH)
    message(FATAL_ERROR "Cannot locate Unity: REACTOR_UC_PATH is unset and no Unity target exists.")
  endif()
  set(UNITY_DIR ${REACTOR_UC_PATH}/external/Unity)
  add_library(Unity STATIC ${UNITY_DIR}/src/unity.c)
  target_include_directories(Unity PUBLIC ${UNITY_DIR}/src)
  set_target_properties(Unity PROPERTIES C_CLANG_TIDY "") # Disable clang-tidy for this external lib.
endif()

set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(TEST_SUFFIX test.c) # Files that are tests must have names ending with TEST_SUFFIX.

# Find all the tests in the current dir
file(
  GLOB_RECURSE TEST_SOURCES
  LIST_DIRECTORIES false
  RELATIVE ${TEST_DIR}
  *${TEST_SUFFIX}
)

# Create executables for each test.
foreach(FILE ${TEST_SOURCES})
  string(REGEX REPLACE "[./]" "_" NAME ${FILE})
  add_executable(${NAME} ${TEST_DIR}/${FILE})
  add_test(NAME ${NAME} COMMAND ${NAME})
  target_link_libraries(${NAME} PRIVATE micromode Unity)
  set_target_properties(${NAME} PROPERTIES C_CLANG_TIDY "")
endforeach(FILE ${TEST_SOURCES})
