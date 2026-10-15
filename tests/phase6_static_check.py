"""P6 source audit. Standard library only; no C++ or socket execution.

Approved freeze migrations use pinned LF-normalized SHA256 plus explicit
semantic oracles. They never accept a file merely because its path is allowed.
Historical P4 main migration is checked against the exact pre-P6 checker after
verifying the migrated checker hash. See docs/P6/review_fixes.md for each delta.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from phase2_static_check import ROOT, masked_source, require

BASELINE = "f8507027c87a9ae9899f51da708f8d469552c473"
FIXTURE_SHA = "9c3a7d1543d259a281eb8003f80c7fe09254e029719194b6453c00629dac3680"
# Filled after review; source and checker hashes are deliberately separate.
APPROVED_SOURCE = {
    'include/runtime/component_health.hpp': '682686a989240670db0bc4d20818c773190dd9239798c70660e71e9fc39bcffe',
    'include/runtime/health_policy.hpp': 'e41c53111525bc2e98d4f219f0576387794f472d841d0105e2e53fc8b37d12c2',
    'include/runtime/event.hpp': '2c077c6294f85850b1cd9d7158cb1f96f35ec4c7777bd35189e18766840e83d6',
    'src/runtime/event.cpp': '5ca9c330791a057444edecd543441e3458db199e721dc59cc369605b5597baa5',
    'include/runtime/service_aggregation.hpp': '3b275c864499024709cab8b3bbbb38af5da840c533dcb128393e323832b14e98',
    'src/runtime/service_aggregation.cpp': 'a5423000b324aba096a9501c08798e2efe13186f131bc9275559d5264ed8ba27',
    'include/runtime/runtime_manager.hpp': '1f9c3d377a40e1e935e7e332836adadb07b9283e5d9e3db901c5678c95caf5be',
    'src/runtime/runtime_manager.cpp': 'c3ffdb9258de37da7bbd9e0868e9e49fbb7eeec861dbc8bd3e88dab63705cbb6',
    'src/service/service_manager.cpp': '748599dabc517acf6b6ac77db5ffe8f6ff1f614bfa326238b2d02886148111f3',
    'include/runtime/config_manager.hpp': 'be38eea9683dfd6a688ca2fd541b850dfa4362053acb2f1b5fc624e9599a0dfa',
    'src/config/config_manager.cpp': '599a249234bc480a99d9a34fc55200d52d331c1d63414f4f328f48568d42a71e',
    'src/ipc/frame.hpp': 'b058eea5f0984ed021f87a5b2786a3641591ace9d469511c6ae5fff474c6cb5d',
    'src/ipc/ipc_manager.hpp': '7cb8715db3cb633a04a2b7b0c4dfab3e65d4a8fe9c28dc0e54c1e8ed4e17343b',
    'src/ipc/ipc_manager.cpp': 'd4ba13ae1205ae9852226e3d44df062c5589599e04cbe0a166bd8489a90e7b99',
    'src/ipc/main.cpp': '29b869c9422cb1e2483d26847a0eb746273055e9d7ef4e9dbc409e753294e699',
}
APPROVED_CHECKERS = {
    'tests/phase3_aggregation_static_check.py': '181a75f2cc9a9f5900bf78e4de589bac4fb82bb0f00059465c8449fdf75acc9b',
    'tests/phase3_ipc_static_check.py': '2f06baa8dee1d6dd197df3f31283ca2721fa70eed9437e164c3e24cf2186e7b6',
    'tests/phase4_static_check.py': '3a0c9deee65d1bbae0e3160a518f8f8dbd0f7b9c843a0b10a716bd512114a572',
    'tests/phase5_static_check.py': '1feefdc2e9a26e341d58a0e48c8b694d70c4bb8d538c643704014cc416f910ab',
}


def read(name):
    return (ROOT / name).read_text(encoding="utf-8-sig")


def read_baseline_p6(name):
    return subprocess.run(["git", "show", BASELINE + ":" + name], cwd=ROOT,
                          check=True, capture_output=True).stdout.decode("utf-8-sig").replace("\r\n", "\n")


def digest(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def check_approved_freeze(name, original):
    expected = APPROVED_SOURCE.get(name, APPROVED_CHECKERS.get(name))
    if expected is None:
        require(read(name) == original, "unapproved frozen file changed: " + name)
    else:
        require(digest(read(name)) == expected, "reviewed P6 migration changed: " + name)


def legacy_checker(name):
    require(name in APPROVED_CHECKERS, "no P6 checker migration: " + name)
    check_approved_freeze(name, "")
    return read_baseline_p6(name)


def body(source, signature):
    # Imported only on demand: P4/P5 import the helpers above during their audit.
    from phase5_static_check import function
    return function(source, signature)


def snippets(source, values, label):
    for value in values:
        require(value in source, label + " missing: " + value)


def check_legacy_and_scope():
    for name in sorted(APPROVED_SOURCE.keys() | APPROVED_CHECKERS.keys()):
        check_approved_freeze(name, "")
    tracked = subprocess.run(["git", "ls-files"], cwd=ROOT, check=True, capture_output=True).stdout.decode().splitlines()
    opened = set(APPROVED_SOURCE) | set(APPROVED_CHECKERS) | {"tests/event_aggregation_tests.cpp", "tests/CMakeLists.txt"}
    for name in tracked:
        if name not in opened:
            require(read(name) == read_baseline_p6(name), "change outside approved P6 files: " + name)
    # Every original test definition, not just selected assertions, stays intact.
    legacy_tests = subprocess.run(["git", "ls-tree", "-r", "--name-only", BASELINE, "tests"],
                                  cwd=ROOT, check=True, capture_output=True).stdout.decode().splitlines()
    for name in legacy_tests:
        if not name.endswith(".cpp"):
            continue
        old, new = read_baseline_p6(name), read(name)
        for test in re.findall(r"\bvoid\s+(test_\w+)\s*\(", masked_source(old)):
            signature = r"\bvoid\s+" + test + r"\([^)]*\)"
            require(body(old, signature) == body(new, signature), "pre-P6 test altered: " + test)
    cmake = read("tests/CMakeLists.txt")
    require(cmake.startswith(read_baseline_p6("tests/CMakeLists.txt")), "original 12/13 registration prefix changed")
    require(not re.search(r"\b(?:DISABLED|WILL_FAIL)\b", cmake), "masked failure")
    from phase2_static_check import check_registration
    require(check_registration() == (14, 15), "unexpected target/registration count")
    for name in ("component_health", "component_ipc"):
        snippets(cmake, ("add_executable(" + name + "_tests " + name + "_tests.cpp)",
                        "COMMAND " + name + "_tests)",), "P6 registration")
    # No new wire owner, retry owner, worker, or UART library in Runtime.
    production = "\n".join(p.read_text(encoding="utf-8-sig") for folder in ("include", "src")
                           for p in (ROOT / folder).rglob("*") if p.suffix in (".hpp", ".cpp"))
    require(not re.search(r'#\s*include[^\n]*(?:uart|serial)|\b(?:UartLink|SerialPort|tcsetattr|TIOCSRS485)\b|/dev/tty',
                          production, re.I), "UART ownership escaped VisionArm")
    for name in ("src/runtime/recovery_manager.cpp", "src/service/process_supervisor.cpp",
                 "src/runtime/device_state_manager.cpp", "src/monitor/monitor.cpp", "src/monitor/resource_collector.cpp",
                 "CMakeLists.txt", "tools/fake_service/main.cpp"):
        require(read(name) == read_baseline_p6(name), "P6 expanded frozen owner: " + name)
    runtime = read("src/runtime/runtime_manager.cpp")
    require(masked_source(runtime).count("std::thread(") == 2, "new Runtime worker")
    admission = runtime[runtime.index("ComponentAdmission::ComponentAdmission"):runtime.index("ResourceSamplingSchedule::ResourceSamplingSchedule")]
    require(not re.search(r'\b(?:FailureType|RecoveryRequest|restartService|kill|fork|reap|open|read|write|send|recv)\b',
                          masked_source(admission)), "component admission acquired process/wire action")
    print("PASS (P6 static): pinned migrations; every pre-P6 test body and original 12/13 registration prefix; frozen owners")


def check_admission_and_writer():
    model = read("include/runtime/component_health.hpp")
    require(re.search(r'enum class ComponentHealthStatus \{ unknown, connecting, healthy, degraded, unhealthy \}', model),
            "five normalized statuses changed")
    snippets(model, ("component_report_interval{1000}", "component_observation_lease{3500}",
                    "component_ingress_limit = 32", "component_payload_limit = 4096",
                    "std::weak_ptr<ComponentReplySink> reply", "std::atomic<bool> revoked{false}"), "normalized model")
    runtime = read("src/runtime/runtime_manager.cpp")
    instance = body(runtime, r"bool ComponentAdmission::current_instance\([^{}]*\) const")
    snippets(instance, ("status->state == ServiceState::running", "status->pid == peer->pid",
                       "status->launched_generation == identity_.launched_generation", "peer->accepted_at >= *status->start_time",
                       "peer->revoked", "peer != peer_"), "commit instance gate")
    apply = body(runtime, r"ComponentReply ComponentAdmission::apply\([^{}]*\)")
    snippets(apply, ("checkpoint(now)", "if (stopped_)", "ingress.peer->pid != status->pid",
                    "ingress.peer->accepted_at < *status->start_time", "current_instance(ingress.peer)",
                    "ingress.report.runtime_epoch != runtime_epoch_", "ingress.report.session_id != identity_.session_id",
                    "report.report_seq == last_report_->report_seq", "same_component_sample(sample, last_report_->sample)",
                    "now - ingress.received_at >= component_observation_lease", "sample.projection_revision < last_owner_->projection_revision",
                    "sample.link_epoch < last_owner_->link_epoch", "sample.peer_boot_id == last_owner_->peer_boot_id",
                    "const bool issue_to_current", "const bool repeated_issue", "!repeated_issue",
                    "throw std::overflow_error", "std::numeric_limits<std::uint64_t>::max()"), "admission")
    duplicate = apply[apply.index("if (last_report_ &&"):apply.index("if (ingress.received_at > now", apply.index("if (last_report_ &&"))]
    require("commit(" not in duplicate and "received_at =" not in duplicate, "duplicate sequence renews observation")
    checkpoint = body(runtime, r"void ComponentAdmission::checkpoint\([^{}]*\)")
    snippets(checkpoint, ("if (stopped_) return", "observe_lifecycle(now)", ">= component_observation_lease",
                         "ComponentReason::report_expired"), "lease checkpoint")
    query = body(runtime, r"std::vector<ComponentHealthSnapshot> RuntimeManager::queryComponentHealth\([^{}]*\) const")
    require(not re.search(r'\b(?:services_|component_admission_|component_checkpoint|queue_|monitor_)\b', masked_source(query)),
            "query advanced state or consulted owners")
    snippets(runtime, ("::getrandom(bytes + done", '"health runtime entropy unavailable"',
                       "mailbox->outstanding >= component_ingress_limit", "ingress.peer->pending.exchange(true)",
                       "i < batch && i < 8", "event.component_ingress_batch", "wake.component_ingress_batch = 1",
                       "stop_component_ingress();", "if (shutting_down_ || !component_admission_)"), "writer bounds/gates")
    drain = body(runtime, r"void RuntimeManager::drain_component_ingress\([^{}]*\)")
    require(drain.index("component_admission_->apply(") < drain.index("Publish committed cache") < drain.index("sink->completed(reply)"),
            "ACK precedes writer/cache application")
    shutdown = runtime[runtime.index("    stop_resource_sampling();\n    shutting_down_ = true;"):]
    require(shutdown.index("stop_component_ingress();") < shutdown.index("recovery_->cancelAll("), "shutdown component gate too late")
    require(not re.search(r'\bdetach\s*\(', masked_source(runtime)), "detached worker")
    print("PASS (P6 static): exact child/launch/session writer gates; replay/lease/order, private bounds, read-only cache and shutdown")


def check_aggregation_extension():
    event = read("include/runtime/event.hpp")
    old = read_baseline_p6("include/runtime/event.hpp")
    pattern = r'enum class RuntimeEventType\s*\{([^}]+)\}'
    members = lambda source: re.findall(r'\b\w+\b', re.search(pattern, source).group(1))
    require(members(event) == members(old) + ["component_health_changed"], "original event ordinals changed")
    dispatcher = read("src/runtime/event.cpp")
    require(dispatcher.replace('    case RuntimeEventType::component_health_changed: return "COMPONENT_HEALTH_CHANGED";\n', '', 1) ==
            read_baseline_p6("src/runtime/event.cpp"), "dispatcher changed beyond one label")
    source = read("src/runtime/service_aggregation.cpp")
    restored = source.replace('    if (event.type == RuntimeEventType::component_health_changed) return handle_component(event);\n', '', 1)
    begin = restored.index('    bool critical_component = false;')
    end = restored.index('    for (const auto& [name, health] : services_)', begin)
    restored = restored[:begin] + restored[end:]
    restored = restored.replace('    if (critical_component) return DeviceState::error;\n', '', 1)
    restored = restored.replace('                       (event.type == RuntimeEventType::component_health_changed && target == DeviceState::error) ||\n', '', 1)
    for signature in (r'void ServiceAggregation::add_component\([^{}]*\)', r'bool ServiceAggregation::handle_component\([^{}]*\)'):
        restored = restored.replace(body(restored, signature) + '\n\n', '', 1)
    require(restored == read_baseline_p6("src/runtime/service_aggregation.cpp"), "P6 altered old process/resource/terminal aggregation")
    evaluate = body(source, r'DeviceState ServiceAggregation::evaluate\(\) const')
    order = ('if (recovery_failed_)', 'if (critical_resource)', 'if (critical_component)', 'if (critical_failure)', 'if (warning)')
    require([evaluate.index(s) for s in order] == sorted(evaluate.index(s) for s in order), "component priority")
    component = body(source, r'bool ServiceAggregation::handle_component\([^{}]*\)')
    snippets(component, ('fact.identity.launched_generation < health.fact.identity.launched_generation',
                         'fact.observation_fresh', 'health.ever_healthy = true', 'health.adverse.reset()', 'reconcile(event)'), "component local clear")
    require('resource_warnings_.clear' not in component and 'services_.clear' not in component and 'recovery_failed_ =' not in component,
            "component clear escaped its owner")
    print("PASS (P6 static): aggregation inverse matches baseline exactly outside component extension; event ordinals/priority/local clear")


def check_transport_and_config():
    ipc = read("src/ipc/ipc_manager.cpp")
    snippets(ipc, ("SO_PEERCRED", "connection.peer->pid = credentials.pid",
                   "connection.peer->accepted_at = accepted_at", "connection.peer->connection_token = next_connection_token_",
                   "client.peer->revoked = true", "service_frames < 16", "length > component_payload_limit",
                   "std::optional<ComponentReply> ready", "std::weak_ptr<ComponentPeer> peer",
                   "client.binding.runtime_epoch", "client.binding.session_id", "client.heartbeat_received_at = Clock::now()",
                   "ingress.received_at = client.heartbeat_received_at", "bool heartbeat_waiting = false",
                   "events->pending.size() > 1024", "client.output.size() + encoded.size() > max_queued_output",
                   "(std::numeric_limits<std::uint64_t>::max() - n) / 10"), "IPC")
    callback = body(ipc, r"void completed\(const ComponentReply& reply\) noexcept override")
    require(not re.search(r'\b(?:send|recv|write)\s*\(', masked_source(callback)), "writer callback does socket I/O")
    handler = body(ipc, r"auto handle = \[&\]\(Client& client, const ipc::Frame& frame\)")
    require(not re.search(r'"[^\n]*APPLIED', handler), "enqueue sends APPLIED")
    for key in ("session_id", "report_seq", "projection_revision", "link_epoch"):
        require('decimal("' + key + '",' in ipc, "noncanonical uint64: " + key)
    main = read("src/ipc/main.cpp")
    require(main.count("load_runtime_file(") == 1, "config loaded twice")
    snippets(main, ("ComponentTransportAdapter>(core)", "core.queryComponentHealth()", "config.services"), "startup wiring")
    config = read("src/config/config_manager.cpp")
    snippets(config, ("health.services.size() > 32", "health.components.size() > 1",
                      'entry.component != "mcu_link"', "!producer.autostart", "producer.heartbeat_timeout < std::chrono::seconds{4}",
                      "auto health_config = health(*object, result)", "std::move(health_config)"), "static registry")
    for signature in (r"class Parser", r"ServiceConfig service\(const Json& value\)",
                      r"MonitoringConfig monitoring\(const Object& root\)",
                      r"void ConfigManager::validate\(const ServiceConfig& config\)",
                      r"void ConfigManager::validate\(const MonitoringConfig& config\)",
                      r"std::chrono::seconds ConfigManager::recoveryTimeout\(const ServiceConfig& config\)"):
        require(body(config, signature) == body(read_baseline_p6("src/config/config_manager.cpp"), signature),
                "old config function changed: " + signature)
    print("PASS (P6 static): peer capture, bounded weak replies/inputs, bound type4, legacy configuration functions and explicit registry")


def check_fixture_and_test_sources():
    fixture = ROOT / "docs/P6/fixtures/health_contract_v1.json"
    require(hashlib.sha256(fixture.read_bytes()).hexdigest() == FIXTURE_SHA, "T1 contract drift")
    data = json.loads(fixture.read_text(encoding="utf-8-sig"))
    require(data["version"] == 1 and data["frozen_by"] == "P6-T1" and len(data["sequences"]) == 9, "fixture shape")
    require(data["transport"]["health_payload_max"] == 4096 and data["transport"]["private_operations_max"] == 32, "fixture bounds")
    for name in ("bind_request", "issue_request", "current_request"):
        message = data["messages"][name]
        schema = data["schema"]["$defs"]["bind_request" if message["op"] == "bind" else "report_request"]
        require(set(message) == set(schema["required"]) == set(schema["properties"]), "fixture schema: " + name)
        require(len(json.dumps(message, ensure_ascii=False).encode("utf-8")) <= 4096, "fixture payload")
        if message["op"] == "report":
            require(message["status"] in data["semantic_validation"][message["reason_code"]], "fixture status/reason")
            require(len(message["reason"].encode("utf-8")) <= 256, "fixture reason bytes")
            for key in ("session_id", "report_seq", "projection_revision", "link_epoch"):
                value = message[key]
                require(re.fullmatch(r'(0|[1-9][0-9]*)', value) and int(value) <= 2**64 - 1, "fixture uint64")
    require(data["messages"]["issue_request"]["peer_boot_id"] == 0, "fixture boot zero")
    for module, count in (("component_health", 17), ("component_ipc", 13)):
        source = read("tests/" + module + "_tests.cpp")
        names = re.findall(r'\bvoid\s+(test_\w+)\s*\(', masked_source(source))
        require(len(names) == count, "P6 test count: " + module)
        for name in names:
            test = body(source, r"\bvoid\s+" + name + r"\([^)]*\)")
            require("require(" in test or ".expect(" in test or "expect_error(" in test, "no assertion: " + name)
            require(name + "();" in source[source.index("int main("):], "not main-wired: " + name)
    extra = read("tests/event_aggregation_tests.cpp")
    require("test_p6_component_candidate_and_independent_resource_sources();" in extra, "combination regression uninvoked")
    plan = read("docs/P6/TEST_PLAN.md")
    review = read("docs/P6/validation_review.md")
    require(set(re.findall(r'\| P6-(\d\d) \|', plan)) == {f"{i:02}" for i in range(1, 39)}, "P6 plan IDs")
    require(set(re.findall(r'\| P6-(\d\d) \|', review)) == {f"{i:02}" for i in range(1, 39)}, "review omits scenario")
    require(set(re.findall(r'\| AC-(\d\d) \|', review)) == {f"{i:02}" for i in range(1, 29)}, "review omits acceptance")
    require(set(re.findall(r'\| H(\d\d) \|', review)) == {f"{i:02}" for i in range(1, 11)}, "review omits hardware evidence gap")
    print("PASS (P6 static): frozen JSON; 30 assertion-bearing P6 functions plus combination regression; 38/28/10 review rows")


def check_visionarm(root):
    root = Path(root).resolve()
    for copy in ("docs/P6/fixtures/health_contract_v1.json",
                 "tools/test_apps/runtime_supervision/fixtures/health_contract_v1.json"):
        require(hashlib.sha256((root / copy).read_bytes()).hexdigest() == FIXTURE_SHA, "VisionArm contract mismatch")
    checker = root / "tools/test_apps/runtime_supervision/tests/phase6_adapter_static_check.py"
    result = subprocess.run([sys.executable, "-B", str(checker)], cwd=root, capture_output=True, text=True, encoding="utf-8")
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    require(result.returncode == 0, "VisionArm T2 static audit failed")
    manifest = json.loads((root / 'tools/test_apps/runtime_supervision/fixtures/p6_t2_source_baseline.json').read_text(encoding='utf-8-sig'))
    for name, expected in manifest['contract_docs'].items():
        require(hashlib.sha256((ROOT / 'docs/P6' / name).read_bytes()).hexdigest() == expected,
                "Runtime/VisionArm design version mismatch: " + name)
    pty_checker = root / "tools/test_apps/runtime_supervision/tests/phase6_pty_static_check.py"
    result = subprocess.run([sys.executable, "-B", str(pty_checker)], cwd=root, capture_output=True, text=True, encoding="utf-8")
    print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    require(result.returncode == 0, "VisionArm T4 PTY source audit failed")
    print("LIMIT (P6): VisionArm has no Git identity; ADR-P6-T2-01 remains open; no combined behavioral acceptance")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--visionarm-root", type=Path, help="optional local VisionArm source package; read-only audit")
    args = parser.parse_args()
    try:
        # Reuse every historical suite; none is replaced by a P6-only shortcut.
        for name in ("phase3_validation_static_check", "phase4_static_check", "phase5_static_check"):
            result = subprocess.run([sys.executable, "-B", str(ROOT / "tests" / (name + ".py"))],
                                    cwd=ROOT, capture_output=True, text=True, encoding="utf-8")
            print(result.stdout, end="")
            if result.stderr:
                print(result.stderr, end="", file=sys.stderr)
            require(result.returncode == 0, "legacy suite failed: " + name)
        check_legacy_and_scope()
        check_admission_and_writer()
        check_aggregation_extension()
        check_transport_and_config()
        check_fixture_and_test_sources()
        for path in list((ROOT / "tests").glob("*.py")) + [ROOT / "docs/P6" / n for n in ("validation_review.md", "review_fixes.md", "validation_review_visionarm.md")]:
            source = path.read_text(encoding="utf-8-sig")
            require(source.endswith("\n") and all(line == line.rstrip() for line in source.splitlines()), "format: " + str(path))
            require(not re.search(r'^(?:<{7}|={7}|>{7})(?:\s|$)', source, re.M), "conflict: " + str(path))
            if path.suffix == ".py":
                ast.parse(source, filename=str(path))
        if args.visionarm_root:
            check_visionarm(args.visionarm_root)
    except (ValueError, OSError, SyntaxError, KeyError, subprocess.CalledProcessError) as error:
        print("FAIL (P6 static): " + str(error), file=sys.stderr)
        return 1
    print("PASS (P6 static): source/contract audit complete")
    print("LIMIT: static evidence only; source presence and assertion wiring do not establish behavior, timing, hardware or P6 DONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
