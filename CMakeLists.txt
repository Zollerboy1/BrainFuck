cmake_minimum_required(VERSION 3.17)
project(BrainFuck)

set(CMAKE_CXX_STANDARD 14)

set(LLVM_INSTALL_DIR "${PROJECT_SOURCE_DIR}/vendor/llvm/llvm/build/install")


find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_INSTALL_DIR} NO_DEFAULT_PATH)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")


set(LLVM_COMPILE_FLAG_LIST "-std=c++14" "-fvisibility=hidden")

if(NOT ${LLVM_ENABLE_RTTI})
    list(APPEND LLVM_COMPILE_FLAG_LIST "-fno-rtti")
endif()

set(OTHER_COMPILE_FLAG_LIST ${LLVM_COMPILE_FLAG_LIST})

if(NOT ${LLVM_ENABLE_EH})
    list(APPEND LLVM_COMPILE_FLAG_LIST "-fno-exceptions")
endif()

add_compile_options(${LLVM_COMPILE_FLAG_LIST})


separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})


llvm_map_components_to_libnames(LLVM_LIB_LIST support core)

foreach(target ${LLVM_TARGETS_TO_BUILD})
    set(asm_parser "LLVM${target}AsmParser")
    set(code_gen "LLVM${target}CodeGen")
    if(${asm_parser} IN_LIST LLVM_AVAILABLE_LIBS)
        list(APPEND LLVM_LIB_LIST ${asm_parser})
    endif()
    if(${code_gen} IN_LIST LLVM_AVAILABLE_LIBS)
        list(APPEND LLVM_LIB_LIST ${code_gen})
    endif()
endforeach()


include_directories(${LLVM_INSTALL_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/vendor/tclap/include)

add_executable(BrainFuck main.cpp)
target_link_libraries(BrainFuck ${LLVM_LIB_LIST})

install(TARGETS BrainFuck DESTINATION bin)
