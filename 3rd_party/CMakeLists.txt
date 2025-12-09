# HandMadeMath
add_library(hmm INTERFACE)
target_include_directories(hmm INTERFACE HandmadeMath)

# parallel-util
add_library(parallel_util INTERFACE)
target_include_directories(parallel_util INTERFACE parallel-util/include)

# tinyobjloader
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE tinyobjloader-c)

# stb
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE stb)
