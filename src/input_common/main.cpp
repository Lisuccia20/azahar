#include <memory>
#include <vector>
#include <string>

#include "common/param_package.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
#include "input_common/motion_emu.h"
#include "input_common/analog_from_button.h"
#include "input_common/touch_from_button.h"
#include "input_common/udp/udp.h"
#include "input_common/sdl/sdl.h"

#ifdef ENABLE_GCADAPTER
#include "input_common/gcadapter/gc_adapter.h"
#include "input_common/gcadapter/gc_poller.h"
#endif

#include "input_common/udp/remote_switch.h"

namespace InputCommon {

static std::shared_ptr<Keyboard>              keyboard;
static std::shared_ptr<MotionEmu>             motion_emu;
static std::shared_ptr<RemoteSwitch>          remote_switch;
static std::shared_ptr<RemoteButtonFactory>   remote_button_factory;
static std::shared_ptr<RemoteAnalogFactory>   remote_analog_factory;
static std::unique_ptr<CemuhookUDP::State>    udp;
static std::unique_ptr<SDL::State>            sdl;

#ifdef ENABLE_GCADAPTER
static std::shared_ptr<GCAdapter::Adapter> gcadapter;
static std::shared_ptr<GCButtonFactory>    gc_button_factory;
static std::shared_ptr<GCAnalogFactory>    gc_analog_factory;
#endif

Keyboard* GetKeyboard() { return keyboard.get(); }
MotionEmu* GetMotionEmu() { return motion_emu.get(); }
std::shared_ptr<RemoteSwitch> GetRemoteSwitch() { return remote_switch; }

void Init() {
#ifdef ENABLE_GCADAPTER
    gcadapter = std::make_shared<GCAdapter::Adapter>();
    gc_button_factory = std::make_shared<GCButtonFactory>(gcadapter);
    gc_analog_factory = std::make_shared<GCAnalogFactory>(gcadapter);
    Input::RegisterFactory<Input::ButtonDevice>("gcpad", gc_button_factory);
    Input::RegisterFactory<Input::AnalogDevice>("gcpad", gc_analog_factory);
#endif

    keyboard = std::make_shared<Keyboard>();
    Input::RegisterFactory<Input::ButtonDevice>("keyboard", keyboard);

    remote_switch = std::make_shared<RemoteSwitch>();
    remote_button_factory = std::make_shared<RemoteButtonFactory>(remote_switch);
    remote_analog_factory = std::make_shared<RemoteAnalogFactory>(remote_switch);
    Input::RegisterFactory<Input::ButtonDevice>("remote_switch", remote_button_factory);
    Input::RegisterFactory<Input::AnalogDevice>("remote_switch", remote_analog_factory);

    Input::RegisterFactory<Input::AnalogDevice>("analog_from_button", std::make_shared<AnalogFromButton>());
    motion_emu = std::make_shared<MotionEmu>();
    Input::RegisterFactory<Input::MotionDevice>("motion_emu", motion_emu);
    Input::RegisterFactory<Input::TouchDevice>("touch_from_button", std::make_shared<TouchFromButtonFactory>());

    sdl = SDL::Init();
    udp = CemuhookUDP::Init();
}

void Shutdown() {
#ifdef ENABLE_GCADAPTER
    Input::UnregisterFactory<Input::ButtonDevice>("gcpad");
    Input::UnregisterFactory<Input::AnalogDevice>("gcpad");
    gc_button_factory.reset(); gc_analog_factory.reset(); gcadapter.reset();
#endif
    Input::UnregisterFactory<Input::ButtonDevice>("keyboard");
    keyboard.reset();
    Input::UnregisterFactory<Input::ButtonDevice>("remote_switch");
    Input::UnregisterFactory<Input::AnalogDevice>("remote_switch");
    remote_button_factory.reset(); remote_analog_factory.reset();
    if (remote_switch) remote_switch.reset();

    Input::UnregisterFactory<Input::AnalogDevice>("analog_from_button");
    Input::UnregisterFactory<Input::MotionDevice>("motion_emu");
    motion_emu.reset();
    Input::UnregisterFactory<Input::TouchDevice>("touch_from_button");
    sdl.reset(); udp.reset();
}

// Funzioni necessarie per la UI e il caricamento
std::string GenerateKeyboardParam(int key_code) {
    Common::ParamPackage params;
    params.Set("engine", "keyboard");
    params.Set("code", key_code);
    return params.Serialize();
}

// Questa è la funzione che "legge" i nomi dei bottoni dal file config
Common::ParamPackage GetControllerButtonBinds(const Common::ParamPackage& params, int button) {
    Common::ParamPackage binds;
    if (params.Get("engine", "") == "remote_switch") {
        binds.Set("engine", "remote_switch");
        int id = params.Get("button", 0);
        binds.Set("button", id);
    }
    return binds;
}

Common::ParamPackage GetControllerAnalogBinds(const Common::ParamPackage& params, int analog) {
    Common::ParamPackage binds;
    if (params.Get("engine", "") == "remote_switch") {
        binds.Set("engine", "remote_switch");
    }
    return binds;
}

void ReloadInputDevices() { Shutdown(); Init(); }

// Stub per analogico da tastiera (necessario per compilare)
std::string GenerateAnalogParamFromKeys(int u, int d, int l, int r, int m, float s) { return {}; }

namespace Polling {
std::vector<std::unique_ptr<DevicePoller>> GetPollers(DeviceType type) {
    std::vector<std::unique_ptr<DevicePoller>> pollers;
    if (remote_switch) {
        if (type == DeviceType::Button) pollers.push_back(std::make_unique<RemoteButtonFactory>(remote_switch));
         else if (type == DeviceType::Analog) {
            pollers.push_back(std::make_unique<RemoteAnalogFactory>(remote_switch, 0)); // circle pad
            pollers.push_back(std::make_unique<RemoteAnalogFactory>(remote_switch, 1)); // c-stick
        }
    }
    if (sdl) {
        auto sdl_pollers = sdl->GetPollers(type);
        for (auto& p : sdl_pollers) pollers.push_back(std::move(p));
    }
    return pollers;
}
}
} // namespace InputCommon