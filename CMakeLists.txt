cmake_minimum_required(VERSION 3.16)
project(mooftcp_core C)

option(MT_SANITIZE "Build host tests with ASan+UBSan" ON)

option(MT_TEST_HOOKS "Compile test-only hooks (spy callbacks)" ON)
if(MT_TEST_HOOKS)
  add_compile_definitions(MT_TEST_HOOKS)
endif()

add_compile_options(-std=c99 -Wall -Wextra -Werror -g)
if(MT_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

file(GLOB MT_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/*.c)
add_library(mtcore STATIC ${MT_CORE_SRC})
target_include_directories(mtcore PUBLIC ${CMAKE_SOURCE_DIR}/core)

file(GLOB MT_HELPER_SRC CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/tests/net.c ${CMAKE_SOURCE_DIR}/tests/pair.c)
set(MT_TEST_LIBS mtcore)
if(MT_HELPER_SRC)
  add_library(mthelp STATIC ${MT_HELPER_SRC})
  target_link_libraries(mthelp PUBLIC mtcore)
  set(MT_TEST_LIBS mthelp)
endif()

# The MacTCP PB dispatcher (mac/glue/mt_pb*.c), host-built against the fake OS layer
# tests/pbfake.c (mt_os.h). Packed PB structs: taking a member's address is an error.
file(GLOB MT_GLUE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mac/glue/mt_pb*.c)
list(APPEND MT_GLUE_SRC ${CMAKE_SOURCE_DIR}/mac/glue/mt_gate.c ${CMAKE_SOURCE_DIR}/mac/glue/mt_enet_wd.c)
if(MT_GLUE_SRC)
  add_library(mtglue STATIC ${MT_GLUE_SRC} ${CMAKE_SOURCE_DIR}/tests/pbfake.c)
  target_include_directories(mtglue PUBLIC ${CMAKE_SOURCE_DIR}/mac/glue
                             ${CMAKE_SOURCE_DIR}/mac/include ${CMAKE_SOURCE_DIR}/tests)
  target_compile_options(mtglue PRIVATE -Werror=address-of-packed-member)
  target_link_libraries(mtglue PUBLIC ${MT_TEST_LIBS})
  set(MT_TEST_LIBS mtglue)
endif()

# Control panel field logic (mac/cdev/cfg_edit.c, portable), host-built.
add_library(mtcdev STATIC ${CMAKE_SOURCE_DIR}/mac/cdev/cfg_edit.c ${CMAKE_SOURCE_DIR}/mac/cdev/cdev_logic.c)
target_include_directories(mtcdev PUBLIC ${CMAKE_SOURCE_DIR}/mac/cdev ${CMAKE_SOURCE_DIR}/mac/glue ${CMAKE_SOURCE_DIR}/mac/include)
target_link_libraries(mtcdev PUBLIC ${MT_TEST_LIBS})
set(MT_TEST_LIBS mtcdev)

# Installer planner and executor (mac/installer/inst_plan.c, inst_exec.c, portable), host-built.
add_library(mtinst STATIC ${CMAKE_SOURCE_DIR}/mac/installer/inst_plan.c ${CMAKE_SOURCE_DIR}/mac/installer/inst_exec.c)
target_include_directories(mtinst PUBLIC ${CMAKE_SOURCE_DIR}/mac/installer)
target_link_libraries(mtinst PUBLIC ${MT_TEST_LIBS})
set(MT_TEST_LIBS mtinst)

enable_testing()
file(GLOB MT_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.c)
foreach(src ${MT_TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(${name} ${MT_TEST_LIBS})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  target_compile_definitions(${name} PRIVATE MT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME ${name} COMMAND ${name})
endforeach()
