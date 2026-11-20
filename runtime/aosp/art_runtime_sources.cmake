# ART runtime sources, taken from runtime/Android.bp (libart-runtime_common_defaults) at the
# pinned AOSP tag. Regenerate when the tag in scripts/fetch-aosp.sh changes.

set(ART_RUNTIME_COMMON_SRCS
    app_info.cc art_field.cc art_method.cc backtrace_helper.cc barrier.cc
    base/gc_visited_arena_pool.cc base/locks.cc base/mem_map_arena_pool.cc base/mutex.cc
    base/quasi_atomic.cc base/timing_logger.cc cha.cc class_linker.cc class_loader_context.cc
    class_root.cc class_table.cc common_throws.cc compat_framework.cc debug_print.cc debugger.cc
    dex/dex_file_annotations.cc dex_register_location.cc exec_utils.cc fault_handler.cc
    gc/accounting/bitmap.cc gc/accounting/card_table.cc gc/accounting/heap_bitmap.cc
    gc/accounting/mod_union_table.cc gc/accounting/remembered_set.cc
    gc/accounting/space_bitmap.cc gc/allocation_record.cc gc/allocator/art-dlmalloc.cc
    gc/allocator/rosalloc.cc gc/collector/concurrent_copying.cc
    gc/collector/garbage_collector.cc gc/collector/immune_region.cc
    gc/collector/immune_spaces.cc gc/collector/mark_compact.cc gc/collector/mark_sweep.cc
    gc/collector/partial_mark_sweep.cc gc/collector/semi_space.cc
    gc/collector/sticky_mark_sweep.cc gc/gc_cause.cc gc/heap.cc gc/reference_processor.cc
    gc/reference_queue.cc gc/scoped_gc_critical_section.cc gc/space/bump_pointer_space.cc
    gc/space/dlmalloc_space.cc gc/space/image_space.cc gc/space/large_object_space.cc
    gc/space/malloc_space.cc gc/space/region_space.cc gc/space/rosalloc_space.cc
    gc/space/space.cc gc/space/zygote_space.cc gc/task_processor.cc gc/verification.cc handle.cc
    hidden_api.cc hprof/hprof.cc indirect_reference_table.cc instrumentation.cc intern_table.cc
    interpreter/interpreter.cc interpreter/interpreter_cache.cc
    interpreter/interpreter_common.cc interpreter/interpreter_switch_impl0.cc
    interpreter/interpreter_switch_impl1.cc interpreter/lock_count_data.cc
    interpreter/shadow_frame.cc interpreter/unstarted_runtime.cc java_frame_root_info.cc
    javaheapprof/javaheapsampler.cc jit/debugger_interface.cc jit/jit.cc jit/jit_code_cache.cc
    jit/jit_memory_region.cc jit/jit_options.cc jit/profile_saver.cc jit/profiling_info.cc
    jit/small_pattern_matcher.cc jni/check_jni.cc jni/java_vm_ext.cc jni/jni_env_ext.cc
    jni/jni_id_manager.cc jni/jni_internal.cc jni/local_reference_table.cc method_handles.cc
    metrics/reporter.cc mirror/array.cc mirror/class.cc mirror/class_ext.cc mirror/dex_cache.cc
    mirror/emulated_stack_frame.cc mirror/executable.cc mirror/field.cc mirror/method.cc
    mirror/method_handle_impl.cc mirror/method_handles_lookup.cc mirror/method_type.cc
    mirror/object.cc mirror/stack_frame_info.cc mirror/stack_trace_element.cc mirror/string.cc
    mirror/throwable.cc mirror/var_handle.cc monitor.cc monitor_objects_stack_visitor.cc
    native/dalvik_system_BaseDexClassLoader.cc native/dalvik_system_DexFile.cc
    native/dalvik_system_VMDebug.cc native/dalvik_system_VMRuntime.cc
    native/dalvik_system_VMStack.cc native/dalvik_system_ZygoteHooks.cc
    native/java_lang_Class.cc native/java_lang_Object.cc native/java_lang_StackStreamFactory.cc
    native/java_lang_String.cc native/java_lang_StringFactory.cc native/java_lang_System.cc
    native/java_lang_Thread.cc native/java_lang_Throwable.cc native/java_lang_VMClassLoader.cc
    native/java_lang_invoke_MethodHandle.cc native/java_lang_invoke_MethodHandleImpl.cc
    native/java_lang_ref_FinalizerReference.cc native/java_lang_ref_Reference.cc
    native/java_lang_reflect_Array.cc native/java_lang_reflect_Constructor.cc
    native/java_lang_reflect_Executable.cc native/java_lang_reflect_Field.cc
    native/java_lang_reflect_Method.cc native/java_lang_reflect_Parameter.cc
    native/java_lang_reflect_Proxy.cc native/java_util_concurrent_atomic_AtomicLong.cc
    native/jdk_internal_misc_Unsafe.cc native/libcore_io_Memory.cc
    native/libcore_util_CharsetUtils.cc native/org_apache_harmony_dalvik_ddmc_DdmServer.cc
    native/org_apache_harmony_dalvik_ddmc_DdmVmInternal.cc native/sun_misc_Unsafe.cc
    native_bridge_art_interface.cc native_stack_dump.cc non_debuggable_classes.cc
    nterp_helpers.cc oat/aot_class_linker.cc oat/elf_file.cc oat/image.cc
    oat/index_bss_mapping.cc oat/jni_stub_hash_map.cc oat/oat.cc oat/oat_file.cc
    oat/oat_file_assistant.cc oat/oat_file_assistant_context.cc oat/oat_file_manager.cc
    oat/oat_quick_method_header.cc oat/stack_map.cc object_lock.cc offsets.cc parsed_options.cc
    plugin.cc quick_exception_handler.cc read_barrier.cc reference_table.cc reflection.cc
    reflective_handle_scope.cc reflective_value_visitor.cc runtime.cc runtime_callbacks.cc
    runtime_common.cc runtime_image.cc runtime_intrinsics.cc runtime_options.cc
    scoped_thread_state_change.cc sdk_checker.cc signal_catcher.cc stack.cc
    startup_completed_task.cc string_builder_append.cc thread.cc thread_list.cc thread_pool.cc
    ti/agent.cc trace.cc transaction.cc var_handles.cc vdex_file.cc verifier/class_verifier.cc
    verifier/instruction_flags.cc verifier/method_verifier.cc verifier/reg_type.cc
    verifier/reg_type_cache.cc verifier/register_line.cc verifier/verifier_deps.cc
    verify_object.cc well_known_classes.cc arch/context.cc arch/instruction_set_features.cc
    arch/memcmp16.cc arch/arm/instruction_set_features_arm.cc arch/arm/registers_arm.cc
    arch/arm64/instruction_set_features_arm64.cc arch/arm64/registers_arm64.cc
    arch/riscv64/instruction_set_features_riscv64.cc arch/riscv64/registers_riscv64.cc
    arch/x86/instruction_set_features_x86.cc arch/x86/registers_x86.cc
    arch/x86_64/registers_x86_64.cc entrypoints/entrypoint_utils.cc
    entrypoints/jni/jni_entrypoints.cc entrypoints/math_entrypoints.cc
    entrypoints/quick/quick_alloc_entrypoints.cc entrypoints/quick/quick_cast_entrypoints.cc
    entrypoints/quick/quick_deoptimization_entrypoints.cc
    entrypoints/quick/quick_dexcache_entrypoints.cc entrypoints/quick/quick_entrypoints_enum.cc
    entrypoints/quick/quick_field_entrypoints.cc
    entrypoints/quick/quick_fillarray_entrypoints.cc entrypoints/quick/quick_jni_entrypoints.cc
    entrypoints/quick/quick_lock_entrypoints.cc entrypoints/quick/quick_math_entrypoints.cc
    entrypoints/quick/quick_string_builder_append_entrypoints.cc
    entrypoints/quick/quick_thread_entrypoints.cc entrypoints/quick/quick_throw_entrypoints.cc
    entrypoints/quick/quick_trampoline_entrypoints.cc)

# Headers whose enums get a generated operator<< (art_operator_srcs).
set(ART_RUNTIME_OPERATOR_HDRS
    base/callee_save_type.h base/locks.h class_status.h compilation_kind.h
    gc/allocator/rosalloc.h gc/allocator_type.h gc/collector/gc_type.h
    gc/collector/mark_compact.h gc/collector_type.h gc/space/region_space.h gc/space/space.h
    gc/weak_root_state.h gc_root.h indirect_reference_table.h instrumentation.h jdwp_provider.h
    jni_id_type.h linear_alloc.h lock_word.h oat/image.h oat/oat.h oat/oat_file.h
    process_state.h reflective_value_visitor.h stack.h suspend_reason.h thread.h thread_state.h
    trace.h verifier/verifier_enums.h)
