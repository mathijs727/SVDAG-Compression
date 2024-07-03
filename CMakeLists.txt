
#=============================================================
#  This file is part of the SymVox (Symmetry Voxelization) software
#  Copyright (C) 2016 by CRS4 Visual Computing Group, Pula, Italy
#
#  For more information, visit the CRS4 Visual Computing Group 
#  web pages at http://vic.crs4.it
#
#  This file may be used under the terms of the GNU General Public
#  License as published by the Free Software Foundation and appearing
#  in the file LICENSE included in the packaging of this file.
#
#  CRS4 reserves all rights not expressly granted herein.
#  
#  This file is provided AS IS with NO WARRANTY OF ANY KIND, 
#  INCLUDING THE WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS 
#  FOR A PARTICULAR PURPOSE.
#=============================================================

cmake_minimum_required(VERSION 3.11)
project(SymVox)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake_modules")

# Required for the whole system
find_package(OpenMP REQUIRED)
find_package(SL REQUIRED)
add_library(SpaceLand INTERFACE)
target_include_directories(SpaceLand INTERFACE ${SL_INCLUDE_DIR})
target_link_libraries(SpaceLand INTERFACE ${SL_LIBRARIES})

# Optional, for building the viewer
find_package(OpenGL)
find_package(glfw3 CONFIG)
find_package(GLEW)
find_package(imgui CONFIG)

# Optional, for parsing LAS files
#find_package(LIBLAS)

############### Options
if(OPENGL_FOUND AND GLEW_FOUND)
	set(BUILD_SVVIEWER TRUE CACHE BOOL "Build SymVox GL Viewer")
else()
	set(BUILD_SVVIEWER FALSE CACHE BOOL "Build SymVox GL Viewer")
    Message("OpenGL, GLFW or GLEW libraries missing. Not building SymVoX GL Viewer")
endif()

if(LIBLAS_FOUND)
        set(BUILD_LASPARSER TRUE CACHE BOOL "Build SymVox LAS Parser")
        message( "LIBLAS library: Found!" )
else()
        set(BUILD_LASPARSER FALSE CACHE BOOL "Build SymVox LAS Parser")
        Message("LIBLAS library missing. Not building SymVoX LAS Parser")
endif(LIBLAS_FOUND)


add_library(symvox
	"src/symvox/encoded_octree.hpp"
	"src/symvox/encoded_svo.hpp"
	"src/symvox/encoded_svo.cpp"
	"src/symvox/encoded_svdag.hpp"
	"src/symvox/encoded_svdag.cpp"
	"src/symvox/encoded_ussvdag.hpp"
	"src/symvox/encoded_ussvdag.cpp"
	"src/symvox/encoded_ssvdag.hpp"
	"src/symvox/encoded_ssvdag.cpp"
	"src/symvox/scene.cpp"
	"src/symvox/scene.hpp"
	"src/symvox/test_triangle_box.cpp"
	"src/symvox/test_triangle_box.hpp"
	"src/symvox/octree.cpp"
	"src/symvox/octree.hpp"
	"src/symvox/geom_octree.cpp"
	"src/symvox/geom_octree.hpp"
	"src/symvox/octree_node.cpp"
	"src/symvox/util.hpp"
	"shaders/octree_dda.frag.glsl"
	"src/symvox/geom_octree_extension.cpp"
	"src/symvox/cluster.cpp"
	"src/symvox/cluster.hpp"
)
target_include_directories(symvox PUBLIC "src")
target_link_libraries(symvox PUBLIC SpaceLand)

add_executable(svbuilder "src/svbuilder/main.cpp")
target_link_libraries(svbuilder PUBLIC symvox)

if(BUILD_SVVIEWER)
	# Add source code
	add_executable(svviewer
		"src/svviewer/main.cpp"
		"src/svviewer/camera.hpp"
		"src/svviewer/camera.cpp"
		"src/svviewer/glslprogram.hpp"
		"src/svviewer/glslprogram.cpp"
		"src/svviewer/screenquadrenderer.hpp"
		"src/svviewer/screenquadrenderer.cpp"
		"src/svviewer/renderer.hpp"
		"src/svviewer/renderer_monitor.hpp"
		"src/svviewer/renderer_monitor.cpp"
		"src/svviewer/octree_dda_renderer.hpp"
		"src/svviewer/octree_dda_renderer.cpp"
		"src/svviewer/halton.hpp"
		"src/svviewer/halton.cpp"
	)
	target_link_libraries(svviewer PUBLIC symvox OpenGL::GL glfw GLEW::glew imgui::imgui)

	# Copy shaders
	set(shader_copies "")
	file(GLOB shader_sources "${PROJECT_SOURCE_DIR}/shaders/*.glsl")
	foreach (shader_file IN LISTS shader_sources)
		get_filename_component(file_name ${shader_file} NAME)
		add_custom_command(
			OUTPUT "${CMAKE_BINARY_DIR}/shaders/${file_name}"
			COMMAND ${CMAKE_COMMAND} -E copy
			"${PROJECT_SOURCE_DIR}/shaders/${file_name}"
			"${CMAKE_BINARY_DIR}/shaders/${file_name}"
			DEPENDS "${PROJECT_SOURCE_DIR}/shaders/${file_name}"
		)
		LIST(APPEND shader_copies "${CMAKE_BINARY_DIR}/shaders/${file_name}")
	endforeach()
	add_custom_target(copy_shaders DEPENDS ${shader_copies})
	add_dependencies(svviewer copy_shaders)
endif(BUILD_SVVIEWER)
