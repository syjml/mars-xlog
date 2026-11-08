include(CMakeFindDependencyMacro)
find_dependency(Boost CONFIG COMPONENTS filesystem iostreams nowide)
find_dependency(zstd CONFIG)
include("${CMAKE_CURRENT_LIST_DIR}/xlogTargets.cmake")
