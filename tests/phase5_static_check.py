"""P5 source/contract audit; standard library, no C++ execution.

Checks frozen ownership, guards, approved P3/P4 migrations and assertion wiring.
Textual oracles are deliberately conservative, not a type checker or proof of
concurrent behavior. Coverage means reviewable source, never runtime PASS.
"""
import ast
import re
import subprocess
import sys

from phase2_static_check import ROOT, check_source, check_registration, masked_source, require
import phase4_static_check as p4

BASELINE = "4843061fce2a329832105d34aaea380a35560c0e"

# Values are (test module, suffix without test_). No placeholders.
COVERAGE = {
    "01": [("resource_collector", "cpu_optional_fields_and_guest_exclusion"), ("resource_collector", "cpu_each_component_busy_definition")],
    "02": [("resource_collector", "first_sample_and_aggregate_delta"), ("resource_collector", "cpu_zero_full_and_large_valid_delta")],
    "03": [("resource_collector", "first_sample_and_aggregate_delta"), ("resource_monitoring", "policy_partial_validity_invalid_latch_and_atomic_input_validation")],
    "04": [("resource_collector", "each_cpu_counter_regression_and_rebaseline"), ("resource_collector", "cpu_zero_delta_shape_change_and_wrap"), ("resource_collector", "invalid_cpu_text_clears_baseline")],
    "05": [("resource_collector", "invalid_memory_text_and_no_free_fallback")],
    "06": [("resource_collector", "memory_available_formula_order_whitespace_and_boundaries")],
    "07": [("resource_monitoring", "policy_hysteresis_change_only_metadata_and_direct_clear")],
    "08": [("resource_monitoring", "policy_hysteresis_change_only_metadata_and_direct_clear")],
    "09": [("resource_monitoring", "policy_hysteresis_change_only_metadata_and_direct_clear"), ("event_aggregation", "p5_policy_fifo_downgrade_and_clear_reaggregate_all_health")],
    "10": [("resource_monitoring", "policy_hysteresis_change_only_metadata_and_direct_clear")],
    "11": [("resource_monitoring", "policy_hysteresis_change_only_metadata_and_direct_clear")],
    "12": [("event_aggregation", "p5_policy_fifo_downgrade_and_clear_reaggregate_all_health"), ("recovery_coordination", "p5_resource_pressure_does_not_restart_running_service")],
    "13": [("event_aggregation", "resource_critical_downgrade_and_clear"), ("event_aggregation", "p5_policy_fifo_downgrade_and_clear_reaggregate_all_health")],
    "14": [("event_aggregation", "p5_policy_fifo_downgrade_and_clear_reaggregate_all_health")],
    "15": [("resource_collector", "process_read_failures_preserve_anchor_and_clear_cpu"), ("resource_monitoring", "native_process_not_present_is_measurement_only_and_shutdown_discards_row")],
    "16": [("resource_collector", "process_reused_pid_token_and_repeated_starttime_mismatch"), ("resource_collector", "process_validation_rejects_missing_duplicate_pid_and_anchor_commit"), ("resource_monitoring", "sm_proc_io_outside_lock_and_post_capture_launch_change")],
    "17": [("resource_collector", "read_errors_partial_validity_and_recovery_metadata"), ("resource_collector", "reader_size_limit_exact_and_oversize"), ("resource_monitoring", "native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown")],
    "18": [("resource_monitoring", "native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown"), ("resource_monitoring", "native_process_not_present_is_measurement_only_and_shutdown_discards_row"), ("resource_monitoring", "native_observer_failure_stops_sampling_and_restores_signal_handler"), ("recovery_coordination", "signal_and_observer_failure_during_recovery_cleanup")],
    "19": [("resource_collector", "process_batch_sort_partial_failure_completion_and_gate")],
    "20": [("recovery_coordination", "p5_resource_pressure_does_not_restart_running_service"), ("recovery_coordination", "real_crash_candidate_success_and_reap"), ("resource_monitoring", "native_process_not_present_is_measurement_only_and_shutdown_discards_row")],
    "21": [("resource_collector", "process_comm_fields_units_and_multicore_cpu"), ("resource_collector", "process_zombie_and_malformed_stat"), ("resource_collector", "process_states_read_limit_exceptions_and_post_validation_gate")],
    "22": [("resource_collector", "process_comm_fields_units_and_multicore_cpu"), ("resource_collector", "process_actual_elapsed_long_gap_and_counter_regression"), ("resource_collector", "process_platform_failure_isolated_from_system")],
    "23": [("resource_monitoring", "sm_try_lock_busy_skips_io_and_recovers_after_launch"), ("resource_collector", "process_validation_busy_mismatch_and_unavailable_capture")],
    "24": [("resource_monitoring", "sampling_deadlines_boundaries_backwards_and_slow_completion"), ("resource_collector", "process_actual_elapsed_long_gap_and_counter_regression")],
    "25": [("resource_monitoring", "config_legacy_roots_defaults_wrapper_and_service_fields"), ("resource_monitoring", "config_seven_fields_and_partial_fixed_defaults"), ("resource_monitoring", "config_json_boundaries_and_threshold_order"), ("resource_monitoring", "config_wrong_types_unknown_duplicate_float_and_overflow"), ("resource_monitoring", "config_preserves_service_validation_and_file_limit")],
    "26": [("resource_monitoring", "policy_legacy_derived_clear_explicit_validation_and_unlocked_sink"), ("resource_monitoring", "config_programmatic_fractional_finite_and_legacy_thresholds"), ("runtime_core", "resource_monitor_thresholds"), ("resource_monitoring", "runtime_modes_reserved_sources_envelope_and_pre_run_fifo")],
    "27": [("resource_monitoring", "runtime_modes_reserved_sources_envelope_and_pre_run_fifo"), ("runtime_core", "runtime_resource_fact_adapter")],
    "28": [("resource_monitoring", "native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown"), ("resource_collector", "process_validation_busy_mismatch_and_unavailable_capture"), ("resource_monitoring", "native_identity_change_query_unavailable_and_signal_shutdown")],
    "29": [("event_aggregation", "p5_policy_clear_to_ready_and_offline_remains_terminal"), ("event_aggregation", "p5_policy_fifo_downgrade_and_clear_reaggregate_all_health")],
    "30": [("phase2_integration", "p5_native_default_reader_system_process_and_shutdown")],
    "31": [("device_ipc", "running_health_requires_confirmed_heartbeats"), ("device_ipc", "subscriptions_and_burst_order"), ("ipc_integration", "ipc_extension_boundaries")],
    "32": [("phase2_integration", "runtime_shutdown_respects_long_grace_period"), ("recovery_manager", "policy_lifetime_success_cancel_and_manual_budget"), ("recovery_coordination", "offline_cancels_other_task_and_leaves_no_recovering_service")],
    "33": [("resource_collector", "unexpected_reader_exception_reaches_caller_boundary"), ("resource_monitoring", "sm_identities_launch_token_cleanup_and_read_only_validation")],
    "34": [("resource_monitoring", "policy_partial_validity_invalid_latch_and_atomic_input_validation"), ("resource_monitoring", "native_pipeline_invalid_retention_query_concurrency_and_inflight_shutdown"), ("resource_collector", "first_read_failure_has_no_success_or_value")],
    "35": [("resource_monitoring", "native_unexpected_worker_exception_shutdown_and_reader_lifetime")],
    "36": [("recovery_dependency", "invalid_graphs_have_no_side_effects"), ("device_ipc", "optional_device_callback_and_sink_lifetime"), ("device_ipc", "producer_overflow_disconnect_and_resubscribe"), ("device_ipc", "unread_client_output_bound"), ("phase2_integration", "p5_native_default_reader_system_process_and_shutdown")],
}


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def baseline(name):
    return subprocess.run(["git", "show", BASELINE + ":" + name], cwd=ROOT,
                          check=True, capture_output=True).stdout.decode("utf-8-sig").replace("\r\n", "\n")


def function(source, signature):
    """Extract brace-balanced definition using masked positions, retaining text."""
    clean = masked_source(source)
    match = re.search(signature + r"\s*\{", clean)
    require(match is not None, "missing function: " + signature)
    opening = clean.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        require(end < len(clean), "unclosed function: " + signature)
        depth += (clean[end] == "{") - (clean[end] == "}")
        end += 1
    return source[match.start():end]


def snippets(source, required, label):
    for item in required:
        require(item in source, label + " missing: " + item)


def check_protection():
    protected = set(p4.PROTECTED) | {
        "include/runtime/event.hpp", "src/runtime/event.cpp",
        "include/runtime/service_aggregation.hpp", "src/runtime/service_aggregation.cpp",
        "include/runtime/recovery.hpp", "include/runtime/recovery_manager.hpp", "src/runtime/recovery_manager.cpp",
        "src/ipc/ipc_manager.cpp", "tests/recovery_manager_tests.cpp", "tests/recovery_dependency_tests.cpp",
        "tests/phase3_aggregation_static_check.py",
    }
    for name in protected:
        require(read(name) == baseline(name), "P5 protected file changed: " + name)
    history = subprocess.run(["git", "ls-tree", "-r", "--name-only", BASELINE, "docs/P0", "docs/P1",
                              "docs/P2", "docs/P3", "docs/P4"], cwd=ROOT, check=True, capture_output=True).stdout.decode().splitlines()
    for name in history:
        require(read(name) == baseline(name), "historical document/log changed: " + name)
    p4.check_freeze()
    p4.check_ownership_and_bridge()
    p4.check_matrix()
    # Approved checker migrations cannot silently drop unrelated old oracles.
    old_p3, new_p3 = baseline("tests/phase3_validation_static_check.py"), read("tests/phase3_validation_static_check.py")
    start, end = '    monitor = masked_source(read("src/monitor/monitor.cpp"))', '    service = masked_source(read("src/service/service_manager.cpp"))'
    require(old_p3[:old_p3.index(start)] == new_p3[:new_p3.index(start)] and
            old_p3[old_p3.index(end):] == new_p3[new_p3.index(end):], "P3 migration escaped Monitor oracle")
    old_p4, new_p4 = baseline("tests/phase4_static_check.py"), read("tests/phase4_static_check.py")
    restored = new_p4.replace('"src/ipc/ipc_manager.hpp", "src/logger/logger.cpp",',
                              '"src/ipc/ipc_manager.hpp", "src/ipc/main.cpp", "src/logger/logger.cpp",')
    restored = restored.replace("    check_native_startup()\n", "")
    begin, end = restored.index("def check_native_startup():"), restored.index("def check_ownership_and_bridge():")
    restored = restored[:begin] + restored[end:]
    require(restored == old_p4, "P4 migration escaped main protection/startup oracle")
    # Old test definitions remain byte-identical except the explicitly migrated threshold test.
    for module in ("runtime_core", "event_aggregation", "recovery_coordination", "phase2_integration",
                   "ipc_integration", "device_ipc"):
        name = "tests/" + module + "_tests.cpp"
        old, new = baseline(name), read(name)
        for test in re.findall(r"\bvoid\s+(test_\w+)\s*\(", masked_source(old)):
            if module == "runtime_core" and test == "test_resource_monitor_thresholds": continue
            signature = r"\bvoid\s+" + test + r"\([^)]*\)"
            require(function(old, signature) == function(new, signature), "old test weakened: " + test)
    return len(protected), len(history)


def check_collector_identity_and_units():
    collector = read("src/monitor/resource_collector.cpp")
    clean = masked_source(collector)
    model = masked_source(read("include/runtime/resource_snapshot.hpp"))
    require(not re.findall(r'^\s*#\s*include\s+"([^"]+)"', read("include/runtime/resource_snapshot.hpp"), re.M),
            "measurement model acquired runtime ownership dependencies")
    require(re.findall(r'^\s*#\s*include\s+"([^"]+)"', read("include/runtime/resource_collector.hpp"), re.M) ==
            ["runtime/resource_snapshot.hpp"], "collector header acquired event/state/recovery dependency")
    require(not re.search(r"\b(?:Monitor|ServiceManager|RecoveryManager|DeviceState|RuntimeEvent|ResourceSeverity|thread|waitpid|kill|fork|reap)\b", clean + model),
            "measurement acquired policy/lifecycle/backend/worker")
    # Three reads plus the reader_ member initializer in the constructor.
    require(collector.count('reader_(') == 4 and collector.count('std::fopen(') == 1 and
            'std::fopen(path.c_str(), "rb")' in collector, "proc I/O escaped bounded read-only reader")
    snippets(collector, ('reader_("/proc/stat", system_file_limit)', 'reader_("/proc/meminfo", system_file_limit)',
        'reader_("/proc/" + std::to_string(identity.pid) + "/stat", process_file_limit)',
        '::sysconf(_SC_CLK_TCK)', '::sysconf(_SC_PAGESIZE)', "observation.value.reset()",
        "observation.consecutive_errors < std::numeric_limits<std::uint64_t>::max()",
        "fields.rfind(')')", "field == 14", "field == 15", "field == 22", "field == 24",
        "previous.starttime && *previous.starttime != stat->starttime", "previous.cpu_ticks.reset()",
        "stat->state == 'Z'", "elapsed <= 0", "stat->cpu_ticks < *previous.cpu_ticks",
        "elapsed <= 3.0 * static_cast<double>(interval.count())", "std::isfinite(percent) && percent >= 0",
        "platform_.clock_ticks_per_second <= 0", "platform_.page_size_bytes <= 0",
        "const auto validated = validate(*captured)", "validated->size() != captured->size()",
        "matches.emplace(identity.service_name, identity)", "match->second.instance_generation != identity.instance_generation",
        "previous_processes_ = std::move(pending)", "left.service_name < right.service_name",
        "counters->field_count != previous.field_count", "counters->components[i] < previous.components[i]",
        "total_delta == 0 || idle_delta > total_delta", "total_delta - idle_delta",
        "counters.components[3]", "counters.components[4]", "counters.field_count < 4",
        'name != "MemTotal:" && name != "MemAvailable:"', 'token(fields) != "kB"',
        "*total == 0 || *available > *total", "*total - *available", "kib * 1024"), "collector")
    require("MemFree" not in collector, "memory fallback introduced")
    require(collector.count("::sysconf(_SC_CLK_TCK)") == collector.count("::sysconf(_SC_PAGESIZE)") == 1,
            "platform units no longer startup-only")
    process = function(collector, r"ProcessResourceScan ResourceCollector::collectProcesses\([^{}]*\)")
    require(process.index("reader_(") < process.index("validate(*captured)") < process.index("previous_processes_ ="),
            "process I/O/revalidation/commit order lost")
    require(process.count("keep_running && !keep_running()") >= 4 and "previous.starttime.reset()" not in process,
            "stop gates or persistent launch anchor lost")
    sm_path = "src/service/service_manager.cpp"
    sm = read(sm_path)
    restored = sm
    for name in ("trySnapshotProcessIdentities", "tryValidateProcessIdentities"):
        signature = r"std::optional<std::vector<ProcessIdentity>> ServiceManager::" + name + r"\([^{}]*\) const"
        body = function(sm, signature)
        require(body.count("std::try_to_lock") == 1 and "lock.owns_lock()" in body and "status.launched_generation" in body,
                "SM identity interface no longer one read-only try-lock")
        require(not re.search(r"\b(?:processes_|monitor_|logger_|generation\+\+|startService|stopService)\b", masked_source(body)),
                "SM identity query mutates lifecycle")
        restored = restored.replace(body + "\n\n", "")
    require(restored == baseline(sm_path), "P5 changed SM lifecycle outside two read-only methods")
    monitor = read("src/monitor/monitor.cpp")
    for name in ("watch", "unwatch", "heartbeat", "check"):
        signature = r"void Monitor::" + name + r"\([^{}]*\)"
        require(function(monitor, signature) == function(baseline("src/monitor/monitor.cpp"), signature),
                "P5 rewrote heartbeat: " + name)


def check_config_policy_runtime():
    config = read("src/config/config_manager.cpp")
    snippets(config, ("return load_runtime_file(path).services", "return {std::move(result), monitoring(*object)}",
        "config.cpu_clear < config.cpu_warning", "config.memory_clear < config.memory_warning",
        "config.memory_warning <= config.memory_critical_clear", "config.memory_critical_clear < config.memory_critical",
        "std::isfinite(value)", "sample_interval_seconds.count() < 1", "sample_interval_seconds.count() > 60"), "global config")
    for key in ("sample_interval_seconds", "cpu_warning", "cpu_clear", "memory_warning", "memory_clear", "memory_critical", "memory_critical_clear"):
        require('get<long long>(*object, "' + key + '"' in config, "noninteger monitoring parser: " + key)
    header = read("include/runtime/config_manager.hpp")
    require(function(header, "struct ServiceConfig") == function(baseline("include/runtime/config_manager.hpp"), "struct ServiceConfig"),
            "legacy ServiceConfig layout changed")
    parser = config[config.index("class Parser"):config.index("MonitoringConfig monitoring(")]
    old = baseline("src/config/config_manager.cpp")
    require(parser == old[old.index("class Parser"):old.index("ServiceConfig service(")], "legacy integer parser changed")
    p4.check_review_fixes()  # Finite/typed validity/change-only/inclusive policy plus old terminal oracles.
    policy = function(read("src/monitor/monitor.cpp"), r"void Monitor::observeResources\([^{}]*\)")
    require(policy.index("throw std::invalid_argument") < policy.index("lock(resource_mutex_)") <
            policy.index("for (auto& fact : facts) resources_"), "policy validation/publication order changed")
    cpu = policy[policy.index("if (snapshot.cpu.quality"):policy.index("if (snapshot.memory.quality")]
    require("Pressure::critical" not in cpu, "CPU acquired critical policy")
    runtime = read("src/runtime/runtime_manager.cpp")
    clean = masked_source(runtime)
    require(clean.count("std::thread(") == 2, "new sampling/recovery worker")
    snippets(runtime, ("ResourceInputMode::external", "native_thresholds(config.monitoring)",
        "input_mode == ResourceInputMode::native", "std::make_unique<ResourceCollector>",
        "!tick_pending_.exchange(true)", "tick_pending_ = false", "now >= *next_due_", "next_due_ = now + interval_",
        'event.runtime_event->source == "cpu_monitor"', 'event.runtime_event->source == "memory_monitor"',
        "native mode rejects external resource percentages", "std::lock_guard<std::mutex> lock(resource_submit_mutex_)",
        "resource_stopped_ = true", "if (resource_stopped_) return;", "resource_sampling_active() const { return running_ && !resource_stopped_",
        "if (!shutting_down_ && event.runtime_event", "monitor_.check(Clock::now())",
        "resource_schedule_.tryQueueTick()", "resource_schedule_.consumeTick()", "monitor_failure_ = std::current_exception()",
        "if (!failure && monitor_failure_) failure = monitor_failure_", "std::chrono::seconds{30}"), "Runtime")
    sample = function(runtime, r"void RuntimeManager::sample_resources\(\)")
    order = ("collectSystem()", "trySnapshotProcessIdentities()", "collectProcesses(", "tryValidateProcessIdentities(",
             "resource_snapshot_ = snapshot", "observeResources(snapshot.system)", "resource_schedule_.completed(resource_now_())")
    require([sample.index(item) for item in order] == sorted(sample.index(item) for item in order), "native cycle order changed")
    require(sample.count("if (!resource_sampling_active()) return;") >= 4, "native stop gates lost")
    query = function(runtime, r"std::optional<ResourceSnapshotView> RuntimeManager::queryResourceSnapshot\([^{}]*\) const")
    require(not re.search(r"\b(?:collector_|services_|monitor_|queue_|resource_schedule_)\b", masked_source(query)),
            "snapshot query performs collection/policy/lifecycle work")
    snippets(query, ("view.snapshot = *resource_snapshot_", "std::nullopt", "> 3 * resource_interval_", "Clock::duration::zero()"), "query")
    for name in ("captured_recovery", "cancel_closure", "apply_change", "drain_work", "reap_children"):
        signature = r"(?:bool|void) RuntimeManager::" + name + r"\([^{}]*\)(?: const)?"
        require(function(runtime, signature) == function(baseline("src/runtime/runtime_manager.cpp"), signature),
                "P5 altered recovery/lifecycle writer: " + name)
    handle = r"void RuntimeManager::handle_event\([^{}]*\)"
    require(function(runtime, handle).replace("!shutting_down_ && ", "") ==
            function(baseline("src/runtime/runtime_manager.cpp"), handle), "P5 altered resource/recovery event ownership")
    worker = runtime[runtime.index("monitor_thread_ = std::thread"):runtime.index("timer_thread_ = std::thread")]
    require(worker.index("monitor_.check(Clock::now())") < worker.index("resource_schedule_.due(resource_now_())"),
            "heartbeat no longer precedes resource I/O")
    suffix = "    // Normal runtime shutdown preserves the last health snapshot"
    old = baseline("src/runtime/runtime_manager.cpp")
    require(runtime[runtime.index(suffix):] == old[old.index(suffix):], "P5 changed grace/reap/signal cleanup suffix")
    main = read("src/ipc/main.cpp")
    p4.check_native_startup()
    require("ResourceInputMode input_mode = ResourceInputMode::native" in read("include/runtime/runtime_manager.hpp") and
            "src/monitor/resource_collector.cpp" in read("CMakeLists.txt") and "core(config, {}," in main,
            "production native collector disconnected")


def check_matrix():
    require(set(COVERAGE) == {f"{i:02}" for i in range(1, 37)}, "missing/extra P5 evidence ID")
    require(set(re.findall(r"\| P5-(\d\d) \|", read("docs/P5/TEST_PLAN.md"))) == set(COVERAGE), "P5 plan/matrix mismatch")
    report = read("docs/P5/validation_review.md")
    require(set(re.findall(r"\| P5-(\d\d) \|", report)) == set(COVERAGE), "P5 review omits coverage row")
    require(set(re.findall(r"\| (\d+) \|", report)) >= {str(i) for i in range(1, 25)}, "P5 review omits acceptance row")
    functions = set()
    for case, references in COVERAGE.items():
        for module, suffix in references:
            path, name = "tests/" + module + "_tests.cpp", "test_" + suffix
            source = read(path)
            body = masked_source(function(source, r"\bvoid\s+" + name + r"\([^)]*\)"))
            assertion = "require(" in body
            if module == "resource_collector":
                for helper, signature, predicates in (
                    ("expect_percent", r"void expect_percent\([^)]*\)",
                     ("observation.quality == MetricQuality::valid", "observation.value.has_value()", "std::isfinite", "std::abs")),
                    ("expect_unavailable", r"void expect_unavailable\([^)]*\)",
                     ("observation.quality == MetricQuality::unavailable", "!observation.value"))):
                    if helper + "(" in body:
                        oracle = masked_source(function(source, signature))
                        snippets(oracle, ("require(",) + predicates, "collector assertion helper")
                        assertion = True
            if module == "resource_monitoring" and "rejects(" in body:
                snippets(source, ("catch (const std::exception& error)", "caught = true;", "require(caught, message)"), "rejection oracle")
                assertion = True
            if module == "event_aggregation" and ".expect(DeviceState::" in body:
                require("require(states.query().current == state" in source, "empty state oracle")
                assertion = True
            require(assertion and name + "(" in masked_source(source)[source.index("int main("):],
                    "P5-" + case + " missing assertion/main wiring: " + name)
            functions.add((path, name))
    cmake = read("tests/CMakeLists.txt")
    for name in ("collector", "monitoring"):
        require("add_test(NAME phase5_resource_" + name + "_unit COMMAND resource_" + name + "_tests)" in cmake,
                "P5 test executable not registered")
        require(re.search(r"set_tests_properties\(phase5_resource_" + name + r"_unit PROPERTIES TIMEOUT \d+\)", cmake),
                "P5 gate fixtures need bounded timeout")
    return len(functions)


def main():
    try:
        files = sorted(path for folder in ("include", "src", "tools", "tests") for path in (ROOT / folder).rglob("*")
                       if path.suffix in (".cpp", ".hpp"))
        scenarios = sum(check_source(path) for path in files)
        targets, registrations = check_registration()
        protected, history = check_protection()
        check_collector_identity_and_units()
        check_config_policy_runtime()
        evidence = check_matrix()
        for path in files + list((ROOT / "tests").glob("*.py")) + list((ROOT / "docs/P5").glob("*.md")):
            source = path.read_text(encoding="utf-8-sig")
            require(source.endswith("\n") and all(line == line.rstrip() for line in source.splitlines()), "format: " + str(path))
            require(not re.search(r"^(?:<{7}|={7}|>{7})(?:\s|$)", source, re.M), "conflict: " + str(path))
            if path.suffix == ".py": ast.parse(source, filename=str(path))
        for path in (ROOT / "src/monitor").glob("resource*"):
            require(not re.search(r"\b(?:thread|thermal|statvfs|sqlite|rknn|cuda|mqtt|http|grpc)\b", masked_source(path.read_text())),
                    "resource scope expansion")
    except (ValueError, OSError, SyntaxError, subprocess.CalledProcessError) as error:
        print("FAIL (P5 static): " + str(error), file=sys.stderr)
        return 1
    print(f"PASS (P5 static): {len(files)} C++ files; {scenarios} main-wired tests; {targets} targets/{registrations} CTest entries")
    print(f"PASS (P5 static): {protected} frozen files; {history} historical documents/logs; old test bodies and six-second/PID/reap retained")
    print("PASS (P5 static): approved P3 Monitor/P4 main migrations only; all P4 COVERAGE/owner/budget/terminal/DSM/IPC checks retained")
    print("PASS (P5 static): bounded measurement, launch anchors, batch try-lock/revalidation, units, typed policy, native ingress, query, gates and two workers")
    print(f"PASS (P5 static): 36 plan IDs map to {evidence} assertion-bearing functions; 24 acceptance rows present")
    print("LIMIT: source/contract evidence only; assertions, concurrency, live procfs and performance are Not Verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
