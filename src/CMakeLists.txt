add_executable(naturalPaint
  main.cpp
  app/SelfTest.cpp
  gfx/Context.cpp
  gfx/ShaderLoader.cpp
  paint/Palette.cpp
  sim/PaintSim.cpp
  ui/MacPaintUI.cpp
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

# Shaders and the pigment LUT are read from the source tree at runtime, so the
# solver can be edited and reloaded (Cmd+R) without a rebuild.
target_compile_definitions(naturalPaint PRIVATE
  NP_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders"
  NP_MIXBOX_LUT="${CMAKE_SOURCE_DIR}/third_party/mixbox/shaders/mixbox_lut.png"
)

if(NP_USE_MIXBOX)
  target_compile_definitions(naturalPaint PRIVATE NP_USE_MIXBOX=1)
endif()
