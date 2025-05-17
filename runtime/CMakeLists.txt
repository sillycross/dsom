add_library(runtime 
  runtime_utils.cpp
  vm.cpp
  som_lexer.cpp
  som_compile_file.cpp
  som_class.cpp
  som_primitives_container.cpp
)

add_dependencies(runtime 
  deegen_fps_lib
)
set_target_properties(runtime PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION ")

