cmake_minimum_required(VERSION 3.10)
project(micromode LANGUAGES C)

# Emit compile_commands.json so editors and clang-tidy see the real flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Command line options for the build.
set(MICROMODE_BUILD_TESTS OFF CACHE BOOL "Build the micromode unit tests")
set(ASAN OFF CACHE BOOL "Compile with AddressSanitizer")
# Accepts -DREACTOR_UC_PATH=<path>, falling back to the environment variable.
set(REACTOR_UC_PATH "$ENV{REACTOR_UC_PATH}" CACHE PATH "Path to a reactor-uc checkout")

# Setup AddressSanitizer.
if(ASAN)
  add_compile_options(-fsanitize=address)
  add_link_options(-fsanitize=address)
endif()

# Check for reactor-uc.
if(NOT TARGET reactor-uc)
  if(NOT REACTOR_UC_PATH)
    message(FATAL_ERROR
      "No reactor-uc target exists and REACTOR_UC_PATH is not set. Either add_subdirectory() "
      "reactor-uc before micromode, or export REACTOR_UC_PATH to a reactor-uc checkout.")
  endif()
  get_filename_component(REACTOR_UC_PATH "${REACTOR_UC_PATH}" ABSOLUTE)
  set(LF_RUNTIME_EXTENSIONS ON CACHE BOOL "" FORCE)
  add_subdirectory(${REACTOR_UC_PATH} reactor-uc-build)
else()
  # Someone else configured the runtime, so we need to check that LF_RUNTIME_EXTENSIONS is on.
  get_target_property(_uc_defs reactor-uc INTERFACE_COMPILE_DEFINITIONS)
  if(NOT _uc_defs OR NOT "${_uc_defs}" MATCHES "(^|;)LF_RUNTIME_EXTENSIONS(=[^;]*)?(;|$)")
    message(FATAL_ERROR
      "The reactor-uc target was added without LF_RUNTIME_EXTENSIONS. Micromode "
      "cannot compile without it. Configure reactor-uc with -DLF_RUNTIME_EXTENSIONS=ON.")
  endif()
  unset(_uc_defs)
endif()

add_library(micromode STATIC src/core.c src/commit.c src/triggers.c)
add_library(micromode::micromode ALIAS micromode)
target_include_directories(micromode PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)

# Link to the REACTOR_UC runtime. Prefer the namespaced name if reactor-uc exports one, but
# do not invent it here: an ALIAS created in this project does not carry reactor-uc's usage
# requirements through, so micromode would compile without reactor-uc's include directories.
if(TARGET reactor-uc::runtime)
  target_link_libraries(micromode PUBLIC reactor-uc::runtime)
else()
  target_link_libraries(micromode PUBLIC reactor-uc)
endif()

# Compile options and definitions.
target_compile_definitions(micromode PUBLIC LF_MODAL_MODELS)
target_compile_options(micromode PRIVATE -Wall -Wextra -Werror -fdiagnostics-color=always)

# Enable testing if requested and if this is the top-level project.
if(MICROMODE_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
  endif()

  # Clang-tidy for static code analysis. Scoped to micromode's own target: reactor-uc
  # configures its own, and we do not want to relint it through our settings.
  find_program(CLANG_TIDY clang-tidy)
  if(CLANG_TIDY)
    set_target_properties(micromode PROPERTIES C_CLANG_TIDY
      "clang-tidy;--header-filter=include/\(.*\)\\.h;--warnings-as-errors=*;-extra-arg=-Wno-unknown-warning-option")
  else()
    message(WARNING "Please install clang-tidy!")
  endif()

  include(CTest)
  add_subdirectory(test)
endif()
