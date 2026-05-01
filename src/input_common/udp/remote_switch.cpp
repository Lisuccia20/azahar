#include "input_common/udp/remote_switch.h"
#include <iostream>

namespace InputCommon {

std::unique_ptr<Input::ButtonDevice> RemoteButtonFactory::Create(const Common::ParamPackage& params) {
    int button_id = params.Get("button", 0);
    auto real_button = switch_ptr->GetOrCreateButton(button_id);
    return std::make_unique<RemoteButtonProxy>(real_button);
}

std::unique_ptr<Input::AnalogDevice> RemoteAnalogFactory::Create(const Common::ParamPackage& params) {
    int analog_id = params.Get("analog_id", 0);
    std::cerr << "[RemoteAnalog] Create chiamato, analog_id=" << analog_id
              << " params=" << params.Serialize() << "\n";
    auto real_analog = switch_ptr->GetOrCreateAnalog(analog_id);
    return std::make_unique<RemoteAnalogProxy>(real_analog);
}

} // namespace InputCommon