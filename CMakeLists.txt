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
# librat
file(GLOB_RECURSE RAT_SOURCES CONFIGURE_DEPENDS rat/lib/*.cpp)
add_library(rat STATIC ${RAT_SOURCES})
target_include_directories(rat PUBLIC rat/include)

# rat-cli
add_executable(rat-driver rat/main.cpp)
set_target_properties(rat-driver PROPERTIES OUTPUT_NAME rat)
target_link_libraries(rat-driver PRIVATE rat)
add_executable(rat-test rat/test/Runner.cpp)
target_link_libraries(rat-test PRIVATE rat)

# ratcc
file(GLOB_RECURSE CC_SOURCES CONFIGURE_DEPENDS cc/lib/*.cpp)
add_library(ratcclib STATIC ${CC_SOURCES})
target_include_directories(ratcclib PUBLIC cc/include)
target_link_libraries(ratcclib PUBLIC rat)
add_executable(ratcc cc/main.cpp)
target_link_libraries(ratcc PRIVATE ratcclib)
add_executable(ratcc-test cc/test/Runner.cpp)
target_link_libraries(ratcc-test PRIVATE ratcclib)

# tests
enable_testing()
cmake_host_system_information(RESULT RAT_NPROC QUERY NUMBER_OF_LOGICAL_CORES)

add_test(NAME rat-ir
				 COMMAND rat-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/rat)
add_test(NAME cc-x86
				 COMMAND ratcc-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/cc)
add_test(NAME cc-c
				 COMMAND ratcc-test -j${RAT_NPROC}
				 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/cc)
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

# format
file(GLOB_RECURSE RAT_FORMAT_SOURCES CONFIGURE_DEPENDS
		 rat/lib/*.cpp rat/include/*.h rat/test/*.cpp
		 cc/lib/*.cpp cc/include/*.h cc/test/*.cpp)
add_custom_target(format
									COMMAND clang-format -i ${RAT_FORMAT_SOURCES}
									${CMAKE_SOURCE_DIR}/rat/main.cpp ${CMAKE_SOURCE_DIR}/cc/main.cpp
									WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
