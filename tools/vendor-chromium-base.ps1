<#
.SYNOPSIS
    Copy the curated base/ + build/ files Chromium's sandbox depends on.

.DESCRIPTION
    Vendors the transitive dependencies of sandbox/. Subset chosen from
    Firefox's individual-files-list in security/sandbox/chromium/moz.yaml,
    cross-checked against actual #include patterns in our local Chromium
    snapshot.

    Source root: C:\code\chromium_git\chromium\src
    Vendor root: godot/thirdparty/chromium-sandbox/
#>

[CmdletBinding()]
param(
    [string]$SourceRoot = "C:\code\chromium_git\chromium\src",
    [string]$VendorRoot = "$PSScriptRoot\..\thirdparty\chromium-sandbox"
)

$ErrorActionPreference = "Stop"

function Copy-File([string]$rel) {
    $src = Join-Path $SourceRoot $rel
    if (-not (Test-Path $src)) { return $false }
    $dst = Join-Path $VendorRoot $rel
    $dstDir = Split-Path $dst -Parent
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force $dstDir | Out-Null }
    Copy-Item $src $dst -Force
    return $true
}

# Per Firefox's moz.yaml — base/ subset used by sandbox/.
$files = @(
    # Build configuration
    "build/build_config.h",
    "build/buildflag.h",

    # base/ root
    "base/at_exit.cc",
    "base/at_exit.h",
    "base/atomic_ref_count.h",
    "base/atomicops.h",
    "base/atomicops_internals_portable.h",
    "base/auto_reset.h",
    "base/base_export.h",
    "base/bit_cast.h",
    "base/bits.h",
    "base/check.cc",
    "base/check.h",
    "base/check_op.cc",
    "base/check_op.h",
    "base/compiler_specific.h",
    "base/cpu.cc",
    "base/cpu.h",
    "base/cxx20_is_constant_evaluated.h",
    "base/cxx20_to_address.h",
    "base/dcheck_is_on.h",
    "base/environment.cc",
    "base/environment.h",
    "base/features.h",
    "base/format_macros.h",
    "base/immediate_crash.h",
    "base/lazy_instance.h",
    "base/lazy_instance_helpers.cc",
    "base/lazy_instance_helpers.h",
    "base/location.cc",
    "base/location.h",
    "base/logging.h",
    "base/no_destructor.h",
    "base/notreached.h",
    "base/observer_list.h",
    "base/observer_list_internal.h",
    "base/observer_list_types.h",
    "base/pending_task.h",
    "base/rand_util.cc",
    "base/rand_util.h",
    "base/rand_util_win.cc",
    "base/scoped_clear_last_error.h",
    "base/scoped_clear_last_error_win.cc",
    "base/sequence_checker.h",
    "base/sequence_checker_impl.h",
    "base/sequence_token.h",
    "base/template_util.h",
    "base/thread_annotations.h",
    "base/token.h",
    "base/unguessable_token.h",
    "base/version.cc",
    "base/version.h",

    # base/containers
    "base/containers/adapters.h",
    "base/containers/checked_iterators.h",
    "base/containers/circular_deque.h",
    "base/containers/contains.h",
    "base/containers/contiguous_iterator.h",
    "base/containers/cxx20_erase.h",
    "base/containers/cxx20_erase_deque.h",
    "base/containers/cxx20_erase_forward_list.h",
    "base/containers/cxx20_erase_internal.h",
    "base/containers/cxx20_erase_list.h",
    "base/containers/cxx20_erase_map.h",
    "base/containers/cxx20_erase_set.h",
    "base/containers/cxx20_erase_string.h",
    "base/containers/cxx20_erase_unordered_map.h",
    "base/containers/cxx20_erase_unordered_set.h",
    "base/containers/cxx20_erase_vector.h",
    "base/containers/flat_map.h",
    "base/containers/flat_tree.h",
    "base/containers/linked_list.h",
    "base/containers/queue.h",
    "base/containers/span.h",
    "base/containers/stack.h",
    "base/containers/util.h",
    "base/containers/vector_buffer.h",

    # base/debug
    "base/debug/alias.cc",
    "base/debug/alias.h",
    "base/debug/crash_logging.cc",
    "base/debug/crash_logging.h",
    "base/debug/dump_without_crashing.h",
    "base/debug/leak_annotations.h",
    "base/debug/profiler.h",

    # base/files
    "base/files/file_path.h",

    # base/functional
    "base/functional/bind.h",
    "base/functional/bind_internal.h",
    "base/functional/callback.h",
    "base/functional/callback_forward.h",
    "base/functional/callback_helpers.h",
    "base/functional/callback_internal.cc",
    "base/functional/callback_internal.h",
    "base/functional/callback_tags.h",
    "base/functional/disallow_unretained.h",
    "base/functional/function_ref.h",
    "base/functional/identity.h",
    "base/functional/invoke.h",
    "base/functional/not_fn.h",
    "base/functional/unretained_traits.h",

    # base/hash
    "base/hash/hash.cc",
    "base/hash/hash.h",

    # base/macros
    "base/macros/concat.h",
    "base/macros/uniquify.h",

    # base/memory
    "base/memory/free_deleter.h",
    "base/memory/memory_pressure_listener.h",
    "base/memory/platform_shared_memory_handle.h",
    "base/memory/platform_shared_memory_region.h",
    "base/memory/ptr_util.h",
    "base/memory/raw_ptr.h",
    "base/memory/raw_ptr_asan_bound_arg_tracker.h",
    "base/memory/raw_ptr_exclusion.h",
    "base/memory/raw_ref.h",
    "base/memory/raw_scoped_refptr_mismatch_checker.h",
    "base/memory/ref_counted.cc",
    "base/memory/ref_counted.h",
    "base/memory/safe_ref_traits.h",
    "base/memory/scoped_refptr.h",
    "base/memory/shared_memory_mapper.h",
    "base/memory/shared_memory_mapping.h",
    "base/memory/singleton.h",
    "base/memory/unsafe_shared_memory_region.h",
    "base/memory/weak_ptr.h",

    # base/message_loop
    "base/message_loop/message_pump.h",
    "base/message_loop/message_pump_for_io.h",
    "base/message_loop/message_pump_for_ui.h",
    "base/message_loop/message_pump_libevent.h",
    "base/message_loop/message_pump_type.h",
    "base/message_loop/message_pump_win.h",
    "base/message_loop/watchable_io_message_pump_posix.h",

    # base/metrics
    "base/metrics/field_trial_params.h",

    # base/numerics
    "base/numerics/checked_math.h",
    "base/numerics/checked_math_impl.h",
    "base/numerics/clamped_math.h",
    "base/numerics/clamped_math_impl.h",
    "base/numerics/safe_conversions.h",
    "base/numerics/safe_conversions_arm_impl.h",
    "base/numerics/safe_conversions_impl.h",
    "base/numerics/safe_math.h",
    "base/numerics/safe_math_arm_impl.h",
    "base/numerics/safe_math_clang_gcc_impl.h",
    "base/numerics/safe_math_shared_impl.h",
    "base/numerics/wrapping_math.h",

    # base/posix
    "base/posix/can_lower_nice_to.cc",
    "base/posix/can_lower_nice_to.h",
    "base/posix/eintr_wrapper.h",
    "base/posix/safe_strerror.cc",
    "base/posix/safe_strerror.h",

    # base/process
    "base/process/environment_internal.cc",
    "base/process/environment_internal.h",
    "base/process/kill.h",
    "base/process/memory.h",
    "base/process/process.h",
    "base/process/process_handle.h",

    # base/ranges
    "base/ranges/algorithm.h",
    "base/ranges/functional.h",
    "base/ranges/ranges.h",

    # base/strings
    "base/strings/safe_sprintf.cc",
    "base/strings/safe_sprintf.h",
    "base/strings/string_number_conversions.cc",
    "base/strings/string_number_conversions.h",
    "base/strings/string_number_conversions_internal.h",
    "base/strings/string_number_conversions_win.h",
    "base/strings/string_piece.h",
    "base/strings/string_piece_forward.h",
    "base/strings/string_split.cc",
    "base/strings/string_split.h",
    "base/strings/string_split_internal.h",
    "base/strings/string_split_win.h",
    "base/strings/string_util.cc",
    "base/strings/string_util.h",
    "base/strings/string_util_constants.cc",
    "base/strings/string_util_impl_helpers.h",
    "base/strings/string_util_internal.h",
    "base/strings/string_util_posix.h",
    "base/strings/string_util_win.cc",
    "base/strings/string_util_win.h",
    "base/strings/stringprintf.cc",
    "base/strings/stringprintf.h",
    "base/strings/to_string.h",
    "base/strings/utf_ostream_operators.cc",
    "base/strings/utf_ostream_operators.h",
    "base/strings/utf_string_conversion_utils.cc",
    "base/strings/utf_string_conversion_utils.h",
    "base/strings/utf_string_conversions.cc",
    "base/strings/utf_string_conversions.h",

    # base/synchronization
    "base/synchronization/atomic_flag.h",
    "base/synchronization/condition_variable.h",
    "base/synchronization/condition_variable_posix.cc",
    "base/synchronization/lock.cc",
    "base/synchronization/lock.h",
    "base/synchronization/lock_impl.h",
    "base/synchronization/lock_impl_posix.cc",
    "base/synchronization/lock_impl_win.cc",
    "base/synchronization/waitable_event.h",
    "base/synchronization/waitable_event_posix.cc",

    # base/task
    "base/task/current_thread.h",
    "base/task/delay_policy.h",
    "base/task/delayed_task_handle.h",
    "base/task/post_task_and_reply_with_result_internal.h",
    "base/task/sequence_manager/task_time_observer.h",
    "base/task/sequenced_task_runner.h",
    "base/task/sequenced_task_runner_helpers.h",
    "base/task/single_thread_task_runner.h",
    "base/task/task_observer.h",
    "base/task/task_runner.h",

    # base/threading
    "base/threading/hang_watcher.h",
    "base/threading/platform_thread.cc",
    "base/threading/platform_thread.h",
    "base/threading/platform_thread_internal_posix.cc",
    "base/threading/platform_thread_internal_posix.h",
    "base/threading/platform_thread_posix.cc",
    "base/threading/platform_thread_ref.cc",
    "base/threading/platform_thread_ref.h",
    "base/threading/platform_thread_win.cc",
    "base/threading/platform_thread_win.h",
    "base/threading/scoped_thread_priority.h",
    "base/threading/simple_thread.h",
    "base/threading/thread_checker.h",
    "base/threading/thread_checker_impl.h",
    "base/threading/thread_collision_warner.cc",
    "base/threading/thread_collision_warner.h",
    "base/threading/thread_id_name_manager.cc",
    "base/threading/thread_id_name_manager.h",
    "base/threading/thread_local.h",
    "base/threading/thread_local_internal.h",
    "base/threading/thread_local_storage.cc",
    "base/threading/thread_local_storage.h",
    "base/threading/thread_local_storage_posix.cc",
    "base/threading/thread_local_storage_win.cc",
    "base/threading/thread_restrictions.cc",
    "base/threading/thread_restrictions.h",
    "base/threading/threading_features.h",

    # base/time
    "base/time/tick_clock.h",
    "base/time/time.cc",
    "base/time/time.h",
    "base/time/time_now_posix.cc",
    "base/time/time_override.h",
    "base/time/time_win.cc",

    # base/trace_event (stub - sandbox uses stub set)
    "base/trace_event/base_tracing.h",
    "base/trace_event/base_tracing_forward.h",
    "base/trace_event/common/trace_event_common.h",
    "base/trace_event/memory_allocator_dump_guid.h",
    "base/trace_event/trace_event_stub.cc",
    "base/trace_event/trace_event_stub.h",

    # base/types
    "base/types/always_false.h",
    "base/types/pass_key.h",
    "base/types/strong_alias.h",
    "base/types/supports_ostream_operator.h",

    # base/third_party
    "base/third_party/cityhash/city.cc",
    "base/third_party/cityhash/city.h",
    "base/third_party/icu/icu_utf.h",

    # base/win — Windows-specific
    "base/win/access_control_list.cc",
    "base/win/access_control_list.h",
    "base/win/access_token.cc",
    "base/win/access_token.h",
    "base/win/current_module.h",
    "base/win/message_window.h",
    "base/win/pe_image.cc",
    "base/win/pe_image.h",
    "base/win/scoped_handle.cc",
    "base/win/scoped_handle.h",
    "base/win/scoped_handle_verifier.cc",
    "base/win/scoped_handle_verifier.h",
    "base/win/scoped_localalloc.h",
    "base/win/scoped_process_information.cc",
    "base/win/scoped_process_information.h",
    "base/win/security_descriptor.cc",
    "base/win/security_descriptor.h",
    "base/win/security_util.cc",
    "base/win/security_util.h",
    "base/win/sid.cc",
    "base/win/sid.h",
    "base/win/startup_information.cc",
    "base/win/startup_information.h",
    "base/win/static_constants.cc",
    "base/win/static_constants.h",
    "base/win/win_handle_types.h",
    "base/win/win_handle_types_list.inc",
    "base/win/windows_types.h",
    "base/win/windows_version.cc",
    "base/win/windows_version.h",

    # base/allocator/partition_allocator — minimal headers needed
    "base/allocator/partition_allocator/src/partition_alloc/allocation_guard.h",
    "base/allocator/partition_allocator/src/partition_alloc/flags.h",
    "base/allocator/partition_allocator/src/partition_alloc/oom.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/augmentations/compiler_specific.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/compiler_specific.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/component_export.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/cxx20_is_constant_evaluated.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/thread_annotations.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/win/win_handle_types.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/win/win_handle_types_list.inc",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_base/win/windows_types.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_config.h",
    "base/allocator/partition_allocator/src/partition_alloc/partition_alloc_forward.h",
    "base/allocator/partition_allocator/src/partition_alloc/pointers/raw_ptr.h",
    "base/allocator/partition_allocator/src/partition_alloc/pointers/raw_ptr_exclusion.h",
    "base/allocator/partition_allocator/src/partition_alloc/pointers/raw_ptr_noop_impl.h",
    "base/allocator/partition_allocator/src/partition_alloc/pointers/raw_ref.h"
)

$copied = 0
$missing = @()
foreach ($rel in $files) {
    if (Copy-File $rel) { $copied++ } else { $missing += $rel }
}

Write-Host "Copied $copied of $($files.Count) files."
if ($missing.Count -gt 0) {
    Write-Host "Missing $($missing.Count):"
    foreach ($m in $missing) { Write-Host "  $m" }
}
