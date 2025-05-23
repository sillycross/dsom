SET(DEEGEN_BYTECODE_DEFINITION_SOURCES
  bytecode_mov.cpp
  new_closure.cpp
  upvalue_bytecodes.cpp
  range_fill_nils.cpp
  return_bytecodes.cpp
  unconditional_branch.cpp
  som_global_get.cpp
  som_call.cpp
  som_super_call.cpp
  som_self_call.cpp
  som_array_dup.cpp
  som_field.cpp
  som_throw.cpp
  som_branch.cpp
  som_binary_operators.cpp
  som_unary_operators.cpp
  som_ternary_operators.cpp
)

set(fps_exec "${PROJECT_BINARY_DIR}/deegen/fps")

add_library(guest_lang_typecheck_impl_info OBJECT
  typecheck_impl_info.cpp
)
set_target_properties(guest_lang_typecheck_impl_info PROPERTIES COMPILE_FLAGS " ${EXTRA_CXX_FLAGS_FOR_LLVM_IR} -DDEEGEN_ANNOTATED_SOURCE_FOR_BYTECODE_DEFINITION ")

# generate the interpreter 
#
set(generated_json_list "")
set(src_ord "0")
foreach(cur_src ${DEEGEN_BYTECODE_DEFINITION_SOURCES})
  set(lib_name "deegen_bytecode_definition_ir_source_${src_ord}")
  
  add_library(${lib_name} OBJECT
    ${cur_src}
  )
  set_target_properties(${lib_name} PROPERTIES COMPILE_FLAGS " ${EXTRA_CXX_FLAGS_FOR_LLVM_IR} -DDEEGEN_ANNOTATED_SOURCE_FOR_BYTECODE_DEFINITION ")

  set(post_process_json "${GENERATED_FILES_DIR}/bytecode_info.interpreter.${cur_src}.json")
  string(SHA1 cur_src_hash "${cur_src}")
  set(post_process_hdr "${GENERATED_FILES_DIR}/deegen_bytecode_builder_defs.${cur_src_hash}.h")
  list(APPEND generated_json_list "${post_process_json}")
  set_source_files_properties(${post_process_json} PROPERTIES GENERATED true)
  set_source_files_properties(${post_process_hdr} PROPERTIES GENERATED true)
  
  add_custom_command(
    OUTPUT ${post_process_json}
    OUTPUT ${post_process_hdr}
    COMMAND ${fps_exec} --process-bytecode-definition-for-interpreter --ir-input=$<TARGET_OBJECTS:${lib_name}> --json-output=${post_process_json} --hdr-output=${post_process_hdr} --cur-src-filename=${cur_src} --audit-dir=${GENERATED_FILES_DIR}/../audit
    DEPENDS fps $<TARGET_OBJECTS:${lib_name}>
  )
  
  math(EXPR src_ord "${src_ord} + 1")
endforeach()

set(bytecode_builder_api_header "${GENERATED_FILES_DIR}/all_bytecode_builder_apis.h")
set(interpreter_dispatch_table_builder "${GENERATED_FILES_DIR}/build_interpreter_dispatch_table.cpp")
set(interpreter_opcode_name_table_builder "${GENERATED_FILES_DIR}/build_interpreter_opcode_name_table.cpp")
set_source_files_properties(${bytecode_builder_api_header} PROPERTIES GENERATED true)
set_source_files_properties(${interpreter_dispatch_table_builder} PROPERTIES GENERATED true)
set_source_files_properties(${interpreter_opcode_name_table_builder} PROPERTIES GENERATED true)
list(JOIN generated_json_list "," comma_separated_json_file_list)
add_custom_command(
  OUTPUT ${bytecode_builder_api_header}
  OUTPUT ${interpreter_dispatch_table_builder}
  OUTPUT ${interpreter_opcode_name_table_builder}
  COMMAND ${fps_exec} --generate-bytecode-builder-api-header --input-list='${comma_separated_json_file_list}' --hdr-output=${bytecode_builder_api_header} --cpp-output=${interpreter_dispatch_table_builder} --cpp-output-2=${interpreter_opcode_name_table_builder}
  DEPENDS fps ${generated_json_list} 
)

# generate the interpreter dispatch table 
#
add_executable(deegen_generate_interpreter_opcode_name_table_json
  ${interpreter_opcode_name_table_builder}
)

set(interpreter_opcode_name_table_builder_exec "${PROJECT_BINARY_DIR}/annotated/bytecodes/deegen_generate_interpreter_opcode_name_table_json")

set(interpreter_opcode_name_table_json "${GENERATED_FILES_DIR}/interpreter_opcode_names.json")
set_source_files_properties(${interpreter_opcode_name_table_json} PROPERTIES GENERATED true)
add_custom_command(
  OUTPUT ${interpreter_opcode_name_table_json}
  COMMAND ${interpreter_opcode_name_table_builder_exec} ${interpreter_opcode_name_table_json}
  DEPENDS deegen_generate_interpreter_opcode_name_table_json
)

set(bytecode_opcode_trait_table_json "${GENERATED_FILES_DIR}/bytecode_opcode_trait_table.json")
set_source_files_properties(${bytecode_opcode_trait_table_json} PROPERTIES GENERATED true)
add_custom_command(
  OUTPUT ${bytecode_opcode_trait_table_json}
  COMMAND ${fps_exec} --generate-bytecode-opcode-trait-table --input-list='${comma_separated_json_file_list}' --bytecode-name-table=${interpreter_opcode_name_table_json} --json-output=${bytecode_opcode_trait_table_json} 
  DEPENDS fps ${generated_json_list} ${interpreter_opcode_name_table_json} 
)

# generate the baseline jit
#
set(baseline_jit_dispatch_table_hdr_file_list "")
set(baseline_jit_generated_json_file_list "")
foreach(cur_src ${DEEGEN_BYTECODE_DEFINITION_SOURCES})
  set(input_json "${GENERATED_FILES_DIR}/bytecode_info.interpreter.${cur_src}.json")

  string(SHA1 cur_src_hash "${cur_src}")
  
  set(post_process_hdr "${GENERATED_FILES_DIR}/deegen_baseline_jit_bytecode_traits.${cur_src_hash}.h")
  set_source_files_properties(${post_process_hdr} PROPERTIES GENERATED true)
  list(APPEND baseline_jit_dispatch_table_hdr_file_list "${post_process_hdr}")
  
  set(post_process_json "${GENERATED_FILES_DIR}/deegen_baseline_jit_generated_logic.${cur_src_hash}.json")
  set_source_files_properties(${post_process_json} PROPERTIES GENERATED true)
  list(APPEND baseline_jit_generated_json_file_list "${post_process_json}")
  
  add_custom_command(
    OUTPUT ${post_process_json}
    OUTPUT ${post_process_hdr}
    COMMAND ${fps_exec} --process-bytecode-definition-for-baseline-jit --bytecode-name-table=${interpreter_opcode_name_table_json} --bytecode-trait-table=${bytecode_opcode_trait_table_json} --json-input=${input_json} --json-output=${post_process_json} --hdr-output=${post_process_hdr} --audit-dir=${GENERATED_FILES_DIR}/../audit 
    DEPENDS fps ${interpreter_opcode_name_table_json} ${input_json} ${bytecode_opcode_trait_table_json}
  )
endforeach()

# generate the baseline JIT codegen function dispatch table and bytecode trait table
#
set(baseline_jit_dispatch_and_trait_table_cpp "${GENERATED_FILES_DIR}/build_baseline_jit_dispatch_and_trait_table.cpp")
set_source_files_properties(${baseline_jit_dispatch_and_trait_table_cpp} PROPERTIES GENERATED true)

set(baseline_jit_misc_helper_logic_asm "${GENERATED_FILES_DIR}/baseline_jit_misc_helper_logic.s")
set_source_files_properties(${baseline_jit_misc_helper_logic_asm} PROPERTIES GENERATED true)

list(JOIN baseline_jit_dispatch_table_hdr_file_list "," comma_separated_baseline_jit_dispatch_table_hdr_file_list)
add_custom_command(
  OUTPUT ${baseline_jit_dispatch_and_trait_table_cpp}
  OUTPUT ${baseline_jit_misc_helper_logic_asm}
  COMMAND ${fps_exec} --generate-baseline-jit-dispatch-and-bytecode-trait-table --input-list='${comma_separated_baseline_jit_dispatch_table_hdr_file_list}' --cpp-output=${baseline_jit_dispatch_and_trait_table_cpp} --bytecode-name-table=${interpreter_opcode_name_table_json} --bytecode-trait-table=${bytecode_opcode_trait_table_json} --asm-output=${baseline_jit_misc_helper_logic_asm}
  DEPENDS fps ${baseline_jit_dispatch_table_hdr_file_list} ${interpreter_opcode_name_table_json} ${bytecode_opcode_trait_table_json}
)

# generate the baseline JIT entry logic
#
set(baseline_jit_function_entry_decl_cpp "${GENERATED_FILES_DIR}/build_baseline_jit_function_entry_logic_trait_table.cpp")
set_source_files_properties(${baseline_jit_function_entry_decl_cpp} PROPERTIES GENERATED true)

set(baseline_jit_function_entry_emitter_asm "${GENERATED_FILES_DIR}/baseline_jit_function_entry_logic_emitter.s")
set_source_files_properties(${baseline_jit_function_entry_emitter_asm} PROPERTIES GENERATED true)

add_custom_command(
  OUTPUT ${baseline_jit_function_entry_decl_cpp}
  OUTPUT ${baseline_jit_function_entry_emitter_asm}
  COMMAND ${fps_exec} --generate-baseline-jit-function-entry-logic --cpp-output=${baseline_jit_function_entry_decl_cpp} --asm-output=${baseline_jit_function_entry_emitter_asm} --audit-dir=${GENERATED_FILES_DIR}/../audit 
  DEPENDS fps 
)

add_custom_target(deegen_baseline_jit_all_generated_header_target DEPENDS ${bytecode_builder_api_header} ${baseline_jit_dispatch_table_hdr_file_list})
 
# generate the DFG JIT
#
set(dfg_jit_specialization_info_cpp_file_list "")
set(dfg_jit_specialization_info_hdr_file_list "")
set(dfg_jit_specialization_info_json_file_list "")
set(dfg_jit_c_wrapper_lists_json_file_list "")

# Generate the DFG builtin node code generators
#
set(post_process_cpp "${GENERATED_FILES_DIR}/deegen_dfg_jit_builtin_node_codegen_info.cpp")
set_source_files_properties(${post_process_cpp} PROPERTIES GENERATED true)
list(APPEND dfg_jit_specialization_info_cpp_file_list "${post_process_cpp}")

set(dfg_jit_builtin_node_codegen_info_hdr "${GENERATED_FILES_DIR}/deegen_dfg_jit_builtin_node_codegen_info.h")
set_source_files_properties(${dfg_jit_builtin_node_codegen_info_hdr} PROPERTIES GENERATED true)
  
set(post_process_json "${GENERATED_FILES_DIR}/deegen_dfg_jit_builtin_node_c_wrapper_lists.json")
set_source_files_properties(${post_process_json} PROPERTIES GENERATED true)
list(APPEND dfg_jit_c_wrapper_lists_json_file_list "${post_process_json}")

set(dfg_jit_builtin_node_code_generator_asm "${GENERATED_FILES_DIR}/deegen_dfg_jit_builtin_node_code_generators.s")
set_source_files_properties(${dfg_jit_builtin_node_code_generator_asm} PROPERTIES GENERATED true)

add_custom_command(
  OUTPUT ${post_process_cpp}
  OUTPUT ${dfg_jit_builtin_node_codegen_info_hdr}
  OUTPUT ${post_process_json}
  OUTPUT ${dfg_jit_builtin_node_code_generator_asm}
  COMMAND ${fps_exec} --process-dfg-jit-builtin-nodes --ir-input=$<TARGET_OBJECTS:guest_lang_typecheck_impl_info> --hdr-output=${dfg_jit_builtin_node_codegen_info_hdr} --cpp-output=${post_process_cpp} --json-output=${post_process_json} --asm-output=${dfg_jit_builtin_node_code_generator_asm} --audit-dir=${GENERATED_FILES_DIR}/../audit 
  DEPENDS fps $<TARGET_OBJECTS:guest_lang_typecheck_impl_info>
)

# Process each bytecode for DFG JIT
#
foreach(cur_src ${DEEGEN_BYTECODE_DEFINITION_SOURCES})
  set(input_json "${GENERATED_FILES_DIR}/bytecode_info.interpreter.${cur_src}.json")

  string(SHA1 cur_src_hash "${cur_src}")
  set(post_process_cpp "${GENERATED_FILES_DIR}/deegen_dfg_jit_specialization_info.${cur_src_hash}.cpp")
  set_source_files_properties(${post_process_cpp} PROPERTIES GENERATED true)
  list(APPEND dfg_jit_specialization_info_cpp_file_list "${post_process_cpp}")
  
  set(post_process_hdr "${GENERATED_FILES_DIR}/deegen_dfg_jit_specialization_info.${cur_src_hash}.h")
  set_source_files_properties(${post_process_hdr} PROPERTIES GENERATED true)
  list(APPEND dfg_jit_specialization_info_hdr_file_list "${post_process_hdr}")
  
  set(post_process_json "${GENERATED_FILES_DIR}/deegen_dfg_jit_specialization_info.${cur_src_hash}.json")
  set_source_files_properties(${post_process_json} PROPERTIES GENERATED true)
  list(APPEND dfg_jit_specialization_info_json_file_list "${post_process_json}")
  
  set(post_process_json_2 "${GENERATED_FILES_DIR}/deegen_dfg_jit_c_wrapper_lists.${cur_src_hash}.json")
  set_source_files_properties(${post_process_json_2} PROPERTIES GENERATED true)
  list(APPEND dfg_jit_c_wrapper_lists_json_file_list "${post_process_json_2}")
  
  add_custom_command(
    OUTPUT ${post_process_cpp}
    OUTPUT ${post_process_hdr}
    OUTPUT ${post_process_json}
    OUTPUT ${post_process_json_2}
    COMMAND ${fps_exec} --generate-dfg-jit-specialized-bytecode-info --json-input=${input_json} --bytecode-name-table=${interpreter_opcode_name_table_json} --bytecode-trait-table=${bytecode_opcode_trait_table_json} --hdr-output=${post_process_hdr} --cpp-output=${post_process_cpp} --json-output=${post_process_json} --json-output-2=${post_process_json_2} --audit-dir=${GENERATED_FILES_DIR}/../audit 
    DEPENDS fps ${interpreter_opcode_name_table_json} ${input_json} ${bytecode_opcode_trait_table_json} 
  )
endforeach()

set(dfg_jit_info_api_header "${GENERATED_FILES_DIR}/deegen_dfg_jit_all_generated_info.h")
set_source_files_properties(${dfg_jit_info_api_header} PROPERTIES GENERATED true)

list(JOIN dfg_jit_specialization_info_hdr_file_list "," comma_separated_dfg_jit_specialization_info_hdr_file_list)
add_custom_command(
    OUTPUT ${dfg_jit_info_api_header}
    COMMAND ${fps_exec} --generate-dfg-jit-bytecode-info-api-header --bytecode-name-table=${interpreter_opcode_name_table_json} --bytecode-trait-table=${bytecode_opcode_trait_table_json} --input-list='${comma_separated_dfg_jit_specialization_info_hdr_file_list}' --hdr-output=${dfg_jit_info_api_header} --audit-dir=${GENERATED_FILES_DIR}/../audit 
    DEPENDS fps ${interpreter_opcode_name_table_json} ${bytecode_opcode_trait_table_json} ${dfg_jit_specialization_info_hdr_file_list}
)

set(dfg_jit_all_c_wrapper_stubs_asm "${GENERATED_FILES_DIR}/deegen_dfg_jit_all_c_wrapper_stubs_asm.s")
set_source_files_properties(${dfg_jit_all_c_wrapper_stubs_asm} PROPERTIES GENERATED true)

list(JOIN dfg_jit_c_wrapper_lists_json_file_list "," comma_separated_dfg_jit_c_wrapper_lists_json_file_list)
add_custom_command(
    OUTPUT ${dfg_jit_all_c_wrapper_stubs_asm}
    COMMAND ${fps_exec} --generate-dfg-jit-c-call-wrapper-stubs --input-list='${comma_separated_dfg_jit_c_wrapper_lists_json_file_list}' --asm-output=${dfg_jit_all_c_wrapper_stubs_asm} 
    DEPENDS fps ${dfg_jit_c_wrapper_lists_json_file_list} 
)

# generate the DFG JIT entry logic
#
set(dfg_jit_function_entry_decl_cpp "${GENERATED_FILES_DIR}/build_dfg_jit_function_entry_logic_trait_table.cpp")
set_source_files_properties(${dfg_jit_function_entry_decl_cpp} PROPERTIES GENERATED true)

set(dfg_jit_function_entry_emitter_asm "${GENERATED_FILES_DIR}/dfg_jit_function_entry_logic_emitter.s")
set_source_files_properties(${dfg_jit_function_entry_emitter_asm} PROPERTIES GENERATED true)

add_custom_command(
  OUTPUT ${dfg_jit_function_entry_decl_cpp}
  OUTPUT ${dfg_jit_function_entry_emitter_asm}
  COMMAND ${fps_exec} --generate-dfg-jit-function-entry-logic --cpp-output=${dfg_jit_function_entry_decl_cpp} --asm-output=${dfg_jit_function_entry_emitter_asm} --audit-dir=${GENERATED_FILES_DIR}/../audit 
  DEPENDS fps 
)

add_custom_target(deegen_dfg_jit_all_generated_header_target DEPENDS ${dfg_jit_info_api_header} ${dfg_jit_specialization_info_hdr_file_list} ${dfg_jit_builtin_node_codegen_info_hdr})
 
# Link all the generated logic back into the original bytecode definition file to get the final implementation
#
set(generated_src_list "")
foreach(cur_src ${DEEGEN_BYTECODE_DEFINITION_SOURCES})
  set(input_json "${GENERATED_FILES_DIR}/bytecode_info.interpreter.${cur_src}.json")

  string(SHA1 cur_src_hash "${cur_src}")
  
  set(baseline_jit_generated_json "${GENERATED_FILES_DIR}/deegen_baseline_jit_generated_logic.${cur_src_hash}.json")
  set(dfg_jit_generated_json "${GENERATED_FILES_DIR}/deegen_dfg_jit_specialization_info.${cur_src_hash}.json")
  
  set(post_process_src "${GENERATED_FILES_DIR}/deegen_bytecode_defs.${cur_src}.s")
  set_source_files_properties(${post_process_src} PROPERTIES GENERATED true)
  list(APPEND generated_src_list "${post_process_src}")
  
  add_custom_command(
    OUTPUT ${post_process_src}
    COMMAND ${fps_exec} --post-process-link-implementations --json-input=${input_json} --json-input-2=${baseline_jit_generated_json} --json-input-3=${dfg_jit_generated_json} --asm-output=${post_process_src} --audit-dir=${GENERATED_FILES_DIR}/../audit 
    DEPENDS fps ${input_json} ${baseline_jit_generated_json} ${dfg_jit_generated_json}
  )
endforeach()

add_library(deegen_generated_interpreter_impl OBJECT
  ${generated_src_list}
  ${interpreter_dispatch_table_builder}
  ${baseline_jit_dispatch_and_trait_table_cpp}
  ${baseline_jit_misc_helper_logic_asm}
  ${baseline_jit_function_entry_decl_cpp}
  ${baseline_jit_function_entry_emitter_asm}
  ${dfg_jit_function_entry_decl_cpp}
  ${dfg_jit_function_entry_emitter_asm}
  ${dfg_jit_specialization_info_cpp_file_list}
  ${dfg_jit_all_c_wrapper_stubs_asm}
  ${dfg_jit_builtin_node_code_generator_asm}
)
set_target_properties(deegen_generated_interpreter_impl PROPERTIES COMPILE_FLAGS " -O3 ")
# Compile the generated CPP after all header files have been generated for sanity
#
add_dependencies(deegen_generated_interpreter_impl deegen_baseline_jit_all_generated_header_target)
add_dependencies(deegen_generated_interpreter_impl deegen_dfg_jit_all_generated_header_target)

