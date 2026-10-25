set(SOURCES DeviceResources.cpp
            DirectXHelper.cpp
            RenderSystemDX.cpp
            ScreenshotSurfaceWindows.cpp)

set(HEADERS DeviceResources.h
            DirectXHelper.h
            RenderContext.h
            RenderSystemDX.h
            ScreenshotSurfaceWindows.h)

if(CORE_SYSTEM_NAME STREQUAL windows)
  list(APPEND SOURCES DolbyVisionOutput.cpp)
  list(APPEND HEADERS DolbyVisionOutput.h)
endif()

core_add_library(rendering_dx)
