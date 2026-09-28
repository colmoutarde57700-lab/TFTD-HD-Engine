# Contract gate for the strict REAL HD build. Removing a guard, or routing
# an OXCE raster back through presentation, must fail the build visibly.
if(NOT DEFINED HD_SOURCE_ROOT)
  message(FATAL_ERROR "HD_SOURCE_ROOT is required")
endif()

function(require_text relative needle)
  file(READ "${HD_SOURCE_ROOT}/${relative}" content)
  string(FIND "${content}" "${needle}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "REAL HD pixel firewall missing in ${relative}: ${needle}")
  endif()
endfunction()

require_text("src/Engine/Screen.cpp" "gpu.beginFrameHdOnly(_screen)")
require_text("src/Engine/Screen.cpp" "REAL HD presentation unavailable: native display is forbidden")
require_text("src/Engine/Screen.cpp" "if (Options::hdGraphics) return false; // REAL HD pixel firewall.")
require_text("src/Engine/HdGpuBackend.cpp" "bool HdGpuBackend::beginFrameHdOnly(SDL_Surface *physicalTarget)")
require_text("src/Engine/HdGpuBackend.cpp" "if (Options::hdGraphics) return false; // Native OXCE pixels cannot enter REAL HD.")
require_text("src/Engine/HdGpuBackend.cpp" "if (Options::hdGraphics) return false; // No native painter-order upload.")
require_text("src/Engine/HdGpuBackend.cpp" "if (Options::hdGraphics) return false; // No native raster replay.")
require_text("src/Engine/State.cpp" "if (Options::hdGraphics) return false; // Native family raster is forbidden.")
require_text("src/Battlescape/Map.cpp" "REAL HD world pass failed: native map display is forbidden")
require_text("src/Battlescape/Map.cpp" "No OXCE raster pixels/order are consumed.")
require_text("src/Battlescape/Map.cpp" "rebuildHdStaticSceneCommands();")

message(STATUS "REAL HD pixel firewall contract verified")
