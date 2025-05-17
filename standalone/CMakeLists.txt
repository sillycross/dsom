add_library(dsom_standalone OBJECT
  main.cpp
)

add_dependencies(dsom_standalone 
  deegen_fps_lib
)
set_target_properties(dsom_standalone PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION ")

