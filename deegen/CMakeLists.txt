SET(DEEGEN_UTIL_LIBRARY_SOURCES
  type_speculation_printer.cpp
  llvm_ir_cleanup_pass.cpp
  llvm_extract_function.cpp
  deegen_desugaring_level.cpp
  tvalue_typecheck_optimization.cpp
  deegen_ast_make_call.cpp
  deegen_link_in_common_snippet.cpp
  deegen_bytecode_operand.cpp
  deegen_register_pinning_scheme.cpp
  deegen_ast_return.cpp
  deegen_ast_return_value_accessor.cpp
  deegen_interpreter_bytecode_impl_creator.cpp
  deegen_function_entry_logic_creator.cpp
  llvm_llc_helper.cpp
  deegen_process_lib_func.cpp
  deegen_ast_throw_error.cpp
  deegen_process_bytecode_definition_for_interpreter.cpp
  deegen_ast_simple_lowering_utils.cpp
  deegen_ast_get_global_object.cpp
  deegen_ast_guest_language_funtion_return.cpp
  deegen_ast_new_closure.cpp
  deegen_ast_upvalue_accessor.cpp
  deegen_ast_variadic_args_accessor.cpp
  deegen_ast_variadic_results_accessor.cpp
  tag_register_optimization.cpp
  llvm_fcmp_extra_optimizations.cpp
  deegen_ast_inline_cache.cpp
  deegen_analyze_lambda_capture_pass.cpp
  deegen_bytecode_metadata.cpp
  deegen_rewrite_closure_call.cpp
  deegen_ast_slow_path.cpp
  deegen_call_inline_cache.cpp
  llvm_flatten_select_structure_pass.cpp
  deegen_ast_get_output_slot.cpp
  deegen_bytecode_ir_components.cpp
  deegen_type_based_hcs_helper.cpp
  deegen_baseline_jit_impl_creator.cpp
  deegen_stencil_runtime_constant_insertion_pass.cpp
  deegen_stencil_lowering_pass.cpp
  invoke_clang_helper.cpp
  deegen_jit_runtime_constant_utils.cpp
  deegen_stencil_creator.cpp
  deegen_jit_codegen_logic_creator.cpp
  deegen_process_bytecode_for_baseline_jit.cpp
  deegen_postprocess_module_linker.cpp
  llvm_identical_function_merger.cpp
  deegen_parse_asm_text.cpp
  deegen_recover_asm_cfg.cpp
  deegen_stencil_inline_cache_extraction_pass.cpp
  deegen_stencil_fixup_cross_reference_helper.cpp
  deegen_global_bytecode_trait_accessor.cpp
  deegen_dfg_jit_process_call_inlining.cpp
  llvm_check_function_effectful.cpp
  deegen_simple_operand_expression.cpp
  deegen_analyze_read_locations.cpp
  deegen_jit_slow_path_data.cpp
  deegen_jit_impl_creator_base.cpp
  deegen_dfg_jit_impl_creator.cpp
  deegen_bytecode_impl_creator_base.cpp
  deegen_jit_register_patch_analyzer.cpp
  deegen_dfg_jit_regalloc_rt_call_wrapper.cpp
  deegen_dfg_aot_slowpath_save_reg_stub.cpp
  fps_post_process_link_implementations.cpp
  deegen_dfg_reg_alloc_variants.cpp
  deegen_dfg_builtin_node_impl_creator.cpp
  deegen_dfg_builtin_nodes.cpp
  typemask_overapprox_automata_generator.cpp
  deegen_dfg_select_variant_logic_creator.cpp
) 

# Always compile these files with O3 to save build time since we won't need to debug into these anyway
#
add_library(deegen_always_o3_utils OBJECT
  base64_util.cpp
  json_parse_dump.cpp	
)
# these options are appended after the normal options, and for Clang, 
# later options always override earlier options, so -O0 -O3 would have the effect of O3
# This is a bit fragile, but even if this breaks it won't be a correctness issue, but only slows down the build 
#
target_compile_options(deegen_always_o3_utils PRIVATE  -O3 -DNDEBUG )

add_library(deegen_utils 
	${DEEGEN_UTIL_LIBRARY_SOURCES}
	$<TARGET_OBJECTS:deegen_always_o3_utils>	
)

# Build the main Futamura projection stage binary
#
add_executable(fps
  fps_main.cpp
  fps_utils.cpp
  fps_generate_interpreter_function_entry_logic.cpp
  fps_process_user_builtin_lib.cpp
  fps_process_bytecode_definition_for_interpreter.cpp
  fps_process_bytecode_definition_for_baseline_jit.cpp
  fps_generate_jit_function_entry_logic.cpp
  fps_process_bytecode_definition_for_dfg_jit.cpp
  fps_process_dfg_builtin_nodes.cpp
)

target_link_libraries(fps PUBLIC
  -Wl,--start-group
  common_utils
  deegen_utils
  deegen_common_snippet_ir
  zydis
  -Wl,--end-group
  ${LLVM_EXTRA_LINK_LIBRARIES} 
)

set(fps_exec "${PROJECT_BINARY_DIR}/deegen/fps")

set(deegen_interpreter_function_entry_sources "")

# generate the interpreter function entry logic
#
set(hdr_name "${GENERATED_FILES_DIR}/get_guest_language_function_interpreter_entry_point.h")
set_source_files_properties(${hdr_name} PROPERTIES GENERATED true)

set(asm_name "${GENERATED_FILES_DIR}/guest_language_function_interpreter_entry_point_asm.s")
set_source_files_properties(${asm_name} PROPERTIES GENERATED true)
list(APPEND deegen_interpreter_function_entry_sources ${asm_name})

set(cpp_name "${GENERATED_FILES_DIR}/guest_language_function_interpreter_entry_point_cpp.cpp")
set_source_files_properties(${cpp_name} PROPERTIES GENERATED true)
list(APPEND deegen_interpreter_function_entry_sources ${cpp_name})

add_custom_command(
  OUTPUT ${hdr_name}
  OUTPUT ${asm_name}
  OUTPUT ${cpp_name}
  COMMAND ${fps_exec} --generate-interp-fn-entry-logic --hdr-output=${hdr_name} --asm-output=${asm_name} --cpp-output=${cpp_name} --audit-dir=${GENERATED_FILES_DIR}/../audit 
  DEPENDS fps
)

add_library(deegen_interpreter_fn_entry_logic OBJECT
  ${deegen_interpreter_function_entry_sources}
)

