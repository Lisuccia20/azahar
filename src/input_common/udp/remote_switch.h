// SPDX-FileCopyrightText: Copyright 2024 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

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

    void UpdateStatus(bool new_status) {
        if (status == new_status) return;
        status = new_status;
        if (on_change) on_change(status);
    }

    void SetOnChange(std::function<void(bool)> cb) { on_change = std::move(cb); }

private:
    bool status = false;
    std::function<void(bool)> on_change;
};

class RemoteAnalog : public Input::AnalogDevice {
public:
    std::tuple<float, float> GetStatus() const override { return {x, y}; }

    void UpdateStatus(float new_x, float new_y) {
        x = new_x;
        y = new_y;
        if (on_change) on_change(x, y);
    }

    void SetOnChange(std::function<void(float, float)> cb) { on_change = std::move(cb); }

private:
    float x = 0.0f, y = 0.0f;
    std::function<void(float, float)> on_change;
};

class RemoteButtonProxy : public Input::ButtonDevice {
public:
    explicit RemoteButtonProxy(std::shared_ptr<RemoteButton> real)
        : real_device(std::move(real)) {}
    bool GetStatus() const override { return real_device->GetStatus(); }

private:
    std::shared_ptr<RemoteButton> real_device;
};

class RemoteAnalogProxy : public Input::AnalogDevice {
public:
    explicit RemoteAnalogProxy(std::shared_ptr<RemoteAnalog> real)
        : real_device(std::move(real)) {}
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
        // Notifica callback navigazione (chiamato anche se il button non esiste ancora)
        if (nav_callback) nav_callback(id, pressed);
    }

    void SetStickState(float x, float y, int id = 0) {
        if (id >= 0 && id < static_cast<int>(analogs.size()) && analogs[id])
            analogs[id]->UpdateStatus(x, y);
    }

    std::shared_ptr<RemoteButton> GetOrCreateButton(int id) {
        if (id >= static_cast<int>(buttons.size())) buttons.resize(id + 1);
        if (!buttons[id]) buttons[id] = std::make_shared<RemoteButton>();
        return buttons[id];
    }

    std::shared_ptr<RemoteAnalog> GetOrCreateAnalog(int id = 0) {
        if (id >= static_cast<int>(analogs.size())) analogs.resize(id + 1);
        if (!analogs[id]) analogs[id] = std::make_shared<RemoteAnalog>();
        return analogs[id];
    }

    // Callback navigazione: chiamato dal thread UDP su ogni button change
    // ATTENZIONE: usare QMetaObject::invokeMethod per tornare al thread Qt
    void SetNavigationCallback(std::function<void(int, bool)> cb) {
        nav_callback = std::move(cb);
    }

    void SetStickCallback(std::function<void(float, float)> cb) {
        stick_callback = std::move(cb);
    }

private:
    std::vector<std::shared_ptr<RemoteButton>> buttons;
    std::vector<std::shared_ptr<RemoteAnalog>> analogs;
    std::function<void(int, bool)> nav_callback;
    std::function<void(float, float)> stick_callback;
};

class RemoteButtonFactory : public Input::Factory<Input::ButtonDevice>,
                            public Polling::DevicePoller {
public:
    explicit RemoteButtonFactory(std::shared_ptr<RemoteSwitch> parent)
        : switch_ptr(std::move(parent)) {}
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override;

    void Start() override {}
    void Stop() override {}
    Common::ParamPackage GetNextInput() override {
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

class RemoteAnalogFactory : public Input::Factory<Input::AnalogDevice>,
                            public Polling::DevicePoller {
public:
    explicit RemoteAnalogFactory(std::shared_ptr<RemoteSwitch> parent, int analog_id = 0)
        : switch_ptr(std::move(parent)), analog_id(analog_id) {}

    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& params) override;

    void Start() override {}
    void Stop() override {}

    Common::ParamPackage GetNextInput() override {
        auto status = switch_ptr->GetOrCreateAnalog(analog_id)->GetStatus();
        if (std::abs(std::get<0>(status)) > 0.5f || std::abs(std::get<1>(status)) > 0.5f) {
            Common::ParamPackage params;
            params.Set("engine", "remote_switch");
            params.Set("analog_id", analog_id);
            params.Set("axis_x", 0);
            params.Set("axis_y", 1);
            return params;
        }
        return {};
    }

private:
    std::shared_ptr<RemoteSwitch> switch_ptr;
    int analog_id;
};

} // namespace InputCommon