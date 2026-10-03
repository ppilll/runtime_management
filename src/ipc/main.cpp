#include "ipc_manager.hpp"
#include "runtime/config_manager.hpp"
#include "runtime/runtime_manager.hpp"
#include <exception>
#include <iostream>
#include <utility>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: runtime_manager <config.json> <control_socket> <service_socket>\n";
        return 2;
    }
    try {
        const auto config = runtime::ConfigManager::load_runtime_file(argv[1]);
        runtime::IpcManager::DeviceStateSink device_changes;
        runtime::RuntimeManager core(config, {}, [&](const runtime::DeviceStateSnapshot& state) {
            if (device_changes) device_changes(state);
        });
        runtime::IpcManager ipc(argv[2], argv[3],
            [&](runtime::Event event) { core.post(std::move(event)); },
            [&](const std::string& name) { return core.query(name); },
            config.services, [&] { return core.queryDeviceState(); });
        device_changes = ipc.device_state_sink();
        ipc.start();
        core.run();
        ipc.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "runtime_manager: " << error.what() << '\n';
        return 1;
    }
}
