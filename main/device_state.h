#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateThinking,
    kDeviceStateSpeaking,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError,
    kDeviceStateError,
    // P5.1 XiaoClaw thin-client states
    kDeviceStateWakeupDetected,
    kDeviceStateUploadingAudio,
    kDeviceStateRecognizing,
    kDeviceStateSynthesizing,
    kDeviceStateReconnecting,
};

#endif // _DEVICE_STATE_H_ 
