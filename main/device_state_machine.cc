#include "device_state_machine.h"

#include <algorithm>
#include <esp_log.h>

static const char* TAG = "StateMachine";

// State name strings for logging
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "connecting",
    "listening",
    "thinking",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "error",
    "wakeup_detected",
    "uploading_audio",
    "recognizing",
    "synthesizing",
    "reconnecting",
    "invalid_state"
};

DeviceStateMachine::DeviceStateMachine() {
}

const char* DeviceStateMachine::GetStateName(DeviceState state) {
    int idx = static_cast<int>(state);
    if (idx >= 0 && idx < static_cast<int>(sizeof(STATE_STRINGS) / sizeof(STATE_STRINGS[0]))) {
        return STATE_STRINGS[idx];
    }
    return STATE_STRINGS[(sizeof(STATE_STRINGS) / sizeof(STATE_STRINGS[0])) - 1];
}

bool DeviceStateMachine::IsValidTransition(DeviceState from, DeviceState to) const {
    // Allow transition to the same state (no-op)
    if (from == to) {
        return true;
    }

    if (to == kDeviceStateError && from != kDeviceStateFatalError) {
        return true;
    }

    // Define valid state transitions based on the state diagram
    switch (from) {
        case kDeviceStateUnknown:
            // Can only go to starting
            return to == kDeviceStateStarting;

        case kDeviceStateStarting:
            // Can go to wifi configuring or activating
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateActivating;

        case kDeviceStateWifiConfiguring:
            // Can go to activating (after wifi connected) or audio testing
            return to == kDeviceStateActivating ||
                   to == kDeviceStateAudioTesting;

        case kDeviceStateAudioTesting:
            // Can go back to wifi configuring
            return to == kDeviceStateWifiConfiguring;

        case kDeviceStateActivating:
            // Can go to upgrading, idle, or back to wifi configuring (on error)
            return to == kDeviceStateUpgrading ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateUpgrading:
            // Can go to idle (upgrade failed) or activating
            return to == kDeviceStateIdle ||
                   to == kDeviceStateActivating;

        case kDeviceStateIdle:
            return to == kDeviceStateConnecting ||
                   to == kDeviceStateWakeupDetected ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateSynthesizing ||
                   to == kDeviceStateActivating ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateReconnecting;

        case kDeviceStateConnecting:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateWakeupDetected;

        case kDeviceStateListening:
            return to == kDeviceStateUploadingAudio ||
                   to == kDeviceStateRecognizing ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateThinking ||
                   to == kDeviceStateIdle;

        case kDeviceStateThinking:
            return to == kDeviceStateSynthesizing ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateSpeaking:
            return to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateFatalError:
            return false;

        case kDeviceStateError:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateReconnecting ||
                   to == kDeviceStateFatalError;

        case kDeviceStateWakeupDetected:
            return to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateUploadingAudio:
            return to == kDeviceStateRecognizing ||
                   to == kDeviceStateThinking ||
                   to == kDeviceStateSynthesizing ||
                   to == kDeviceStateIdle;

        case kDeviceStateRecognizing:
            return to == kDeviceStateThinking ||
                   to == kDeviceStateSynthesizing ||
                   to == kDeviceStateIdle;

        case kDeviceStateSynthesizing:
            return to == kDeviceStateSpeaking ||
                   to == kDeviceStateIdle;

        case kDeviceStateReconnecting:
            return to == kDeviceStateIdle ||
                   to == kDeviceStateFatalError;

        default:
            return to == kDeviceStateIdle || to == kDeviceStateFatalError;
    }
}

bool DeviceStateMachine::CanTransitionTo(DeviceState target) const {
    return IsValidTransition(current_state_.load(), target);
}

bool DeviceStateMachine::TransitionTo(DeviceState new_state) {
    DeviceState old_state = current_state_.load();
    
    // No-op if already in the target state
    if (old_state == new_state) {
        return true;
    }

    // Validate transition
    if (!IsValidTransition(old_state, new_state)) {
        ESP_LOGW(TAG, "Invalid state transition: %s -> %s",
                 GetStateName(old_state), GetStateName(new_state));
        return false;
    }

    // Perform transition
    current_state_.store(new_state);
    ESP_LOGI(TAG, "State: %s -> %s",
             GetStateName(old_state), GetStateName(new_state));

    // Notify callback
    NotifyStateChange(old_state, new_state);
    return true;
}

int DeviceStateMachine::AddStateChangeListener(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(callback));
    return id;
}

void DeviceStateMachine::RemoveStateChangeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
            [listener_id](const auto& p) { return p.first == listener_id; }),
        listeners_.end());
}

void DeviceStateMachine::NotifyStateChange(DeviceState old_state, DeviceState new_state) {
    std::vector<StateCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            callbacks_copy.push_back(cb);
        }
    }
    
    for (const auto& cb : callbacks_copy) {
        cb(old_state, new_state);
    }
}
