add_executable(naturalPaint
  main.cpp
  app/CurveEdit.cpp
  app/Keymap.cpp
  app/Latency.cpp
  app/Memory.cpp
  app/SelfTest.cpp
  app/Snapping.cpp
  app/ViewTransform.cpp
  brush/StrokePath.cpp
  color/LutBake.cpp
  color/Shaper.cpp
  color/Space.cpp
  core/Half.cpp
  core/Histogram.cpp
  core/OpStack.cpp
  core/Probe.cpp
  gfx/Context.cpp
  gfx/ShaderLoader.cpp
  io/Export.cpp
  io/ImageDecode.cpp
  io/ImageIO.cpp
  ops/PointOps.cpp
  paint/Palette.cpp
  sim/PaintSim.cpp
  ui/MacPaintUI.cpp
  ui/NaturalPaintUI.cpp
  ui/Theme.cpp
)

target_include_directories(naturalPaint PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(naturalPaint PRIVATE
  SDL3::SDL3-static
  wgpu_native
  imgui
)

# Shaders, the pigment LUT and the keymap are read from the source tree at
# runtime, so the solver can be edited and reloaded (Cmd+R) and the keymap
# hand-edited, without a rebuild -- same convention for all three.
target_compile_definitions(naturalPaint PRIVATE
  NP_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
  NP_MIXBOX_LUT="${CMAKE_SOURCE_DIR}/third_party/mixbox/shaders/mixbox_lut.png"
  NP_KEYMAP_DIR="${CMAKE_SOURCE_DIR}/keymaps"
)

if(NP_USE_MIXBOX)
  target_compile_definitions(naturalPaint PRIVATE NP_USE_MIXBOX=1)
endif()
