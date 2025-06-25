cmake_minimum_required(VERSION 3.8)

project(reactivex)

file(GLOB SRC *.cpp)

add_executable(${PROJECT_NAME}  main.cpp)
target_include_directories(${PROJECT_NAME} PUBLIC 
${CMAKE_SOURCE_DIR}/include
)
