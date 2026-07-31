cmake_minimum_required(VERSION 3.16)
project(rat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
	set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

if(MSVC)
	add_compile_options(/W3 /EHsc /bigobj)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
	add_compile_options(-Wall -Wextra -O2)
endif()

# targets
# base carries the shared headers and the test harness
add_library(base STATIC src/base/TestHarness.cpp)
target_include_directories(base PUBLIC src/base)

# backend
file(GLOB_RECURSE RAT_SOURCES CONFIGURE_DEPENDS src/backend/lib/*.cpp)
add_library(rat STATIC ${RAT_SOURCES})
target_include_directories(rat PUBLIC src/backend/include)
target_link_libraries(rat PUBLIC base)

add_executable(rat-driver src/backend/main.cpp)
set_target_properties(rat-driver PROPERTIES OUTPUT_NAME rat)
target_link_libraries(rat-driver PRIVATE rat)
add_executable(rat-test src/backend/test/Runner.cpp)
target_link_libraries(rat-test PRIVATE rat)

# linker
add_library(link STATIC src/linker/Linker.cpp src/linker/ElfWrite.cpp src/linker/ElfRead.cpp)
target_include_directories(link PUBLIC src/linker)
target_link_libraries(link PUBLIC base)
add_executable(link-driver src/linker/main.cpp)
set_target_properties(link-driver PROPERTIES OUTPUT_NAME link)
target_link_libraries(link-driver PRIVATE link)

# compiler
# builtin standard headers are read from src/compiler/headers at runtime,
# located relative to the executable (see BuiltinHeaders.cpp)
file(GLOB_RECURSE CC_SOURCES CONFIGURE_DEPENDS src/compiler/lib/*.cpp)
add_library(cc STATIC ${CC_SOURCES})
target_include_directories(cc PUBLIC src/compiler/include)
target_link_libraries(cc PUBLIC rat)
add_executable(cc-driver src/compiler/main.cpp)
set_target_properties(cc-driver PROPERTIES OUTPUT_NAME cc)
target_link_libraries(cc-driver PRIVATE cc)
add_executable(cc-test src/compiler/test/Runner.cpp)
target_link_libraries(cc-test PRIVATE cc link)

# tests
enable_testing()
cmake_host_system_information(RESULT RAT_NPROC QUERY NUMBER_OF_LOGICAL_CORES)

add_test(NAME rat-ir
				 COMMAND rat-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/backend)
add_test(NAME cc-x86
				 COMMAND cc-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/compiler)
add_test(NAME cc-c
				 COMMAND cc-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/compiler)
set_tests_properties(cc-x86 PROPERTIES ENVIRONMENT "RATCC_X86=1" TIMEOUT 3600)
set_tests_properties(cc-c PROPERTIES ENVIRONMENT "RATCC_X86=0" TIMEOUT 3600)
set_tests_properties(rat-ir PROPERTIES TIMEOUT 600)

add_custom_target(test-rat
									COMMAND ${CMAKE_CTEST_COMMAND} -R rat-ir --output-on-failure -C $<CONFIG>
									WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_target(test-x86
									COMMAND ${CMAKE_CTEST_COMMAND} -R "rat-ir|cc-x86" --output-on-failure -C $<CONFIG>
									WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_custom_target(test-c
									COMMAND ${CMAKE_CTEST_COMMAND} -R "rat-ir|cc-c" --output-on-failure -C $<CONFIG>
									WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# bench
if(NOT WIN32)
	enable_language(C)
	set(BENCH_REPS 5 CACHE STRING "runs per bench")
	add_custom_target(run-bench
										COMMAND ${CMAKE_SOURCE_DIR}/src/compiler/bench/compare.sh
														$<TARGET_FILE:cc-driver> ${CMAKE_C_COMPILER}
														${CMAKE_SOURCE_DIR}/src/compiler/bench/bench.c
														${CMAKE_BINARY_DIR}/bench-out ${BENCH_REPS}
										DEPENDS cc-driver
										USES_TERMINAL VERBATIM)
endif()

# format
file(GLOB_RECURSE RAT_FORMAT_SOURCES CONFIGURE_DEPENDS
		 src/base/*.h src/base/*.cpp src/linker/*.cpp src/linker/*.h
		 src/backend/lib/*.cpp src/backend/include/*.h src/backend/test/*.cpp
		 src/compiler/lib/*.cpp src/compiler/include/*.h src/compiler/test/*.cpp)
add_custom_target(format
									COMMAND clang-format -i ${RAT_FORMAT_SOURCES}
									${CMAKE_SOURCE_DIR}/src/backend/main.cpp ${CMAKE_SOURCE_DIR}/src/compiler/main.cpp
									WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})