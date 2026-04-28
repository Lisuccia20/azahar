#pragma once

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "common/param_package.h"
#include "core/frontend/input.h"
#include "input_common/main.h"

namespace InputCommon {

class RemoteButton : public Input::ButtonDevice {
public:
    bool GetStatus() const override { return status; }
    void UpdateStatus(bool new_status) { status = new_status; }
private:
    bool status = false;
};

class RemoteAnalog : public Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override { return {x, y}; }
    void UpdateStatus(float new_x, float new_y) { x = new_x; y = new_y; }
private:
    float x = 0.0f, y = 0.0f;
};

class RemoteButtonProxy : public Input::ButtonDevice {
public:
    explicit RemoteButtonProxy(std::shared_ptr<RemoteButton> real) : real_device(std::move(real)) {}
    bool GetStatus() const override { return real_device->GetStatus(); }
private:
    std::shared_ptr<RemoteButton> real_device;
};

class RemoteAnalogProxy : public Input::AnalogDevice {
public:
    explicit RemoteAnalogProxy(std::shared_ptr<RemoteAnalog> real) : real_device(std::move(real)) {}
    std::tuple<float, float> GetStatus() const override { return real_device->GetStatus(); }
private:
    std::shared_ptr<RemoteAnalog> real_device;
};

class RemoteSwitch {
public:
    RemoteSwitch() = default;
    void SetButtonState(int id, bool pressed) {
        if (id >= 0 && id < static_cast<int>(buttons.size()) && buttons[id])
            buttons[id]->UpdateStatus(pressed);
    }
    void SetStickState(float x, float y) {
        if (analog) analog->UpdateStatus(x, y);
    }
    std::shared_ptr<RemoteButton> GetOrCreateButton(int id) {
        if (id >= static_cast<int>(buttons.size())) buttons.resize(id + 1);
        if (!buttons[id]) buttons[id] = std::make_shared<RemoteButton>();
        return buttons[id];
    }
    std::shared_ptr<RemoteAnalog> GetOrCreateAnalog() {
        if (!analog) analog = std::make_shared<RemoteAnalog>();
        return analog;
    }
private:
    std::vector<std::shared_ptr<RemoteButton>> buttons;
    std::shared_ptr<RemoteAnalog> analog;
};

class RemoteButtonFactory : public Input::Factory<Input::ButtonDevice>, public Polling::DevicePoller {
public:
    explicit RemoteButtonFactory(std::shared_ptr<RemoteSwitch> parent) : switch_ptr(std::move(parent)) {}
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override;

    void Start() override {}
    void Stop() override {}
    Common::ParamPackage GetNextInput() override {
        // Scansiona i bottoni: se uno è premuto, "spara" l'input al mapper
        for (int i = 0; i < 20; ++i) {
            if (switch_ptr->GetOrCreateButton(i)->GetStatus()) {
                Common::ParamPackage params;
                params.Set("engine", "remote_switch");
                params.Set("button", i);
                return params;
            }
        }
        return {};
    }
private:
    std::shared_ptr<RemoteSwitch> switch_ptr;
};

class RemoteAnalogFactory : public Input::Factory<Input::AnalogDevice>, public Polling::DevicePoller {
public:
    explicit RemoteAnalogFactory(std::shared_ptr<RemoteSwitch> parent) : switch_ptr(std::move(parent)) {}
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override;

    void Start() override {}
    void Stop() override {}
    Common::ParamPackage GetNextInput() override {
        auto status = switch_ptr->GetOrCreateAnalog()->GetStatus();
        if (std::abs(std::get<0>(status)) > 0.5f || std::abs(std::get<1>(status)) > 0.5f) {
            Common::ParamPackage params;
            params.Set("engine", "remote_switch");
            params.Set("axis_x", 0);
            params.Set("axis_y", 1);
            return params;
        }
        return {};
    }
private:
    std::shared_ptr<RemoteSwitch> switch_ptr;
};

} // namespace InputCommon