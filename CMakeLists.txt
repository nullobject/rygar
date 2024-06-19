#
# project: rygar
#
cmake_minimum_required(VERSION 3.5)
project(rygar)

# include the fips main cmake file
get_filename_component(FIPS_ROOT_DIR "../fips" ABSOLUTE)
include("${FIPS_ROOT_DIR}/cmake/fips.cmake")

fips_setup()
add_subdirectory(src)
