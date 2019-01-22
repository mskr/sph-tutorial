cmake_minimum_required( VERSION 3.7.2 )
project( sph-benchmark )
set( CMAKE_CXX_STANDARD 11 )

# make sure we have OpenMP support
find_package( OpenMP REQUIRED )
if( OPENMP_FOUND )
    message( "OpenMP found" )
    set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}" )
    set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}" )
endif()

# external includes
include_directories( SYSTEM "external/glm" )
include_directories( SYSTEM "external/glad/include" )

# application code
include_directories( "include" )
add_executable(
    sph-benchmark
    "src/main.cpp"
    "external/glad/src/glad_wgl.c"
    "external/glad/src/glad.c"
	"src/gl-windows.cpp" )

target_link_libraries(sph-benchmark "opengl32.lib" "winmm.lib")