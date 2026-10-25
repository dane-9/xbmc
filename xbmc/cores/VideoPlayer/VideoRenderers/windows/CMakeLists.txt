set(SOURCES RendererBase.cpp
            RendererDXVA.cpp
            RendererHQ.cpp
            RendererShaders.cpp
            RendererSoftware.cpp)

set(HEADERS RendererBase.h
            RendererDXVA.h
            RendererHQ.h
            RendererShaders.h
            RendererSoftware.h)

if(CORE_SYSTEM_NAME STREQUAL windowsstore)
  list(APPEND SOURCES DolbyVisionEffect.cpp)
  list(APPEND HEADERS DolbyVisionEffect.h)
endif()

if(CORE_SYSTEM_NAME STREQUAL windows)
  list(APPEND SOURCES dolby/DoviTunnel.cpp
                      dolby/DvGpuCarrier.cpp
                      dolby/DvMappingTables.cpp
                      dolby/DvRpuTransport.cpp
                      dolby/DvTunnelStats.cpp)
  list(APPEND HEADERS dolby/DoviTunnel.h
                      dolby/DvBlob.h
                      dolby/DvGpuCarrier.h
                      dolby/DvMappingTables.h
                      dolby/DvRpuTransport.h
                      dolby/DvTunnelStats.h)
endif()

core_add_library(videorenderers-windows)

if(CORE_SYSTEM_NAME STREQUAL windows)
  # file:profile[:extra flags]
  set(DOVI_SHADERS dovi_compose_chroma:cs_5_0:/Gis
                   dovi_compose_luma:cs_5_0
                   dovi_copy_ps:ps_5_0
                   dovi_el_vertical:cs_5_0
                   dovi_fullscreen_vs:vs_5_0
                   dovi_gui_ps:ps_5_0
                   dovi_pack:cs_5_0
                   dovi_reshape:cs_5_0
                   dovi_scale:cs_5_0
                   dovi_unpack_p010:cs_5_0)
  foreach(shader ${DOVI_SHADERS})
    string(REPLACE ":" ";" parts ${shader})
    list(GET parts 0 file)
    list(GET parts 1 profile)
    set(flags "")
    list(LENGTH parts count)
    if(count GREATER 2)
      list(GET parts 2 flags)
    endif()
    add_custom_command(OUTPUT ${file}.h
                       COMMAND windows::FXC
                       ARGS /nologo /Fh ${file}.h /E main /T ${profile} /O3 ${flags} /Vn ${file}
                            /Qstrip_reflect ${CMAKE_SOURCE_DIR}/system/shaders/${file}.hlsl
                       DEPENDS ${CMAKE_SOURCE_DIR}/system/shaders/${file}.hlsl
                               ${CMAKE_SOURCE_DIR}/system/shaders/dovi_el_horizontal.hlsl
                       COMMENT "FX compile shader ${file}.hlsl"
                       VERBATIM)
    list(APPEND DOVI_SHADER_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/${file}.h)
  endforeach()
  add_custom_target(generate_dovi_shaders ALL DEPENDS ${DOVI_SHADER_HEADERS})
  set_target_properties(generate_dovi_shaders PROPERTIES FOLDER "Build Utilities")
  target_include_directories(${CORE_LIBRARY} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(${CORE_LIBRARY} generate_dovi_shaders)
endif()
