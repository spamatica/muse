#=============================================================================
#  MusE
#  Linux Music Editor
#  $Id:$
#
#  Copyright (C) 1999-2011 by Werner Schweer and others
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the
#  Free Software Foundation, Inc.,
#  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
#=============================================================================

##
## List of source files to compile
##
file (GLOB plugin_scan_source_files
      muse_plugin_scan.cpp
      )

##
## Define target
##
add_executable ( muse_plugin_scan
      ${plugin_scan_source_files}
      )

target_link_libraries(muse_plugin_scan
      plugin_scan_module
      plugin_cache_writer_module
      dl
      ${QT_LIBRARIES}
      )

if (VST3_SUPPORT)
      ##
      ## VST3 scanning. The VST3 host code in ../vst3 is shared with the stand-alone tests there.
      ## sdk_hosting does not contain the Linux module loader nor PlugProvider, the SDK examples add
      ## those to their own sources, and so do we.
      ##
      set (VST3_HOSTING_DIR ${VST3_SDK_PATH}/public.sdk/source/vst/hosting)
      add_library ( muse_vst3host STATIC
            ${CMAKE_CURRENT_SOURCE_DIR}/../vst3/vst3host.cpp
            ${VST3_HOSTING_DIR}/module_linux.cpp
            ${VST3_HOSTING_DIR}/plugprovider.cpp
            ${VST3_SDK_PATH}/public.sdk/source/common/memorystream.cpp
            )
      target_include_directories ( muse_vst3host PUBLIC ${VST3_SDK_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/.. )
      target_link_libraries ( muse_vst3host PUBLIC sdk_hosting sdk_common sdk base pluginterfaces ${CMAKE_DL_LIBS} )

      target_sources ( muse_plugin_scan PRIVATE vst3_scan.cpp vst3_scan.h )
      target_include_directories ( muse_plugin_scan PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/.. )
      target_link_libraries ( muse_plugin_scan muse_vst3host )
endif (VST3_SUPPORT)

##
## Install location
##
install(TARGETS muse_plugin_scan
      DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
      )
