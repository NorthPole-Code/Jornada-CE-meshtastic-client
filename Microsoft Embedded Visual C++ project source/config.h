#pragma once

#include <windows.h>

//--------------------------------------------------
// Modem Presets (Meshtastic LoRaConfig.modem_preset enum)
// Values match meshtastic/config.proto for 2.7.x
//--------------------------------------------------
#define MODEM_PRESET_LONG_FAST       0
#define MODEM_PRESET_LONG_SLOW       1
#define MODEM_PRESET_VERY_LONG_SLOW  2
#define MODEM_PRESET_MEDIUM_SLOW     3
#define MODEM_PRESET_MEDIUM_FAST     4
#define MODEM_PRESET_SHORT_SLOW      5
#define MODEM_PRESET_SHORT_FAST      6
#define MODEM_PRESET_LONG_MODERATE   7

// Newer Meshtastic firmwares expose SHORT_TURBO as an additional preset.
// Keep numeric values aligned with the device enum values.
#define MODEM_PRESET_SHORT_TURBO     8

//--------------------------------------------------
// GPS Mode (Meshtastic PositionConfig.gps_mode)
//--------------------------------------------------
#define GPS_MODE_ENABLED      1
#define GPS_MODE_DISABLED     0
#define GPS_MODE_NOT_PRESENT  2


//--------------------------------------------------
// App configuration (persisted to INI)
//--------------------------------------------------
struct AppConfig
{
    TCHAR storagePath[MAX_PATH];
    int comPort;  // 1=COM1, 3=COM3 (IR)

    // Chat timestamp display:
    // 0 = time only (HH:MM)
    // 1 = date + time (DD/MM/YY HH:MM)
    int chatTimestampMode;

    // Chat auto-scroll behavior on new message (send/receive).
    // 0 = Off    : never jump to the end
    // 1 = Follow : jump only if the user was already at the end (IRC-style)
    // 2 = Always : always jump to the end on new messages
    int chatAutoScroll;

    // Jornada front button notification LED options
    // 0/1 (stored in app ini)
    int ledNewMessage;
    int ledUnread;

    // Prevent the Jornada from entering system sleep/suspend while a serial
    // connection is active.
    // 0/1 (stored in app ini)
    int preventSleepWhileConnected;

    // Map viewer: invert map bitmap colors (pseudo dark mode).
    // 0/1 (stored in app ini)
    int mapInvertColors;

    // Serial monitor options
    // serialMonitorEnabled: 0/1
    // serialMonitorMaxLines: <=0 means no limit, otherwise oldest lines are trimmed
    int serialMonitorEnabled;
    int serialMonitorMaxLines;
    
    AppConfig()
    {
        _tcscpy(storagePath, TEXT("\\Storage Card\\Meshtastic"));
        comPort = 1;
        // Default on first run (no config yet): show date+time.
        // Logging persists full date+time regardless of this setting.
        chatTimestampMode = 1;

        // Default: IRC-style follow.
        chatAutoScroll = 1;

        // Default: enable both LED behaviors.
        ledNewMessage = 1;
        ledUnread = 1;

        // Default: allow normal power management.
        preventSleepWhileConnected = 0;

        // Default: normal (non-inverted) map colors.
        mapInvertColors = 0;

        // Default on first run (no config yet): serial monitor disabled (improves performance).
        serialMonitorEnabled = 0;

        // Default: keep serial monitor bounded if enabled.
        // <=0 means unlimited; choose a sensible default.
        serialMonitorMaxLines = 500;
    }
};

//--------------------------------------------------
// Device settings (read from/written to device)
//--------------------------------------------------
struct DeviceSettings
{
    bool hasReceivedConfig;  // Flag to track if we've received config from device
    
    int role;         // Device role (0=Client, 1=Client_Mute, etc.)
    int region;       // 0=unset, 1=US, 2=EU433, 3=EU868, etc
    int modemPreset;  // LoRaConfig.modem_preset enum
    bool modemPresetPresent; // debug: set true if modem_preset field present during fetch
    bool txEnabled;
    bool txEnabledPresent;  // debug: set true if tx_enabled field present during fetch
    int txPower;      // TX power in dBm
    int hopLimit;     // Number of hops
    int freqSlot;     // Frequency slot
    
    int  gpsMode;          // PositionConfig.gps_mode enum: 0=DISABLED, 1=ENABLED, 2=NOT_PRESENT
    bool transmitLocation;  // Broadcast location to network
    
    // Temp storage for detecting preset from radio params
    int bandwidth;
    int spreadFactor;
    int codingRate;
    
    DeviceSettings()
    {
        hasReceivedConfig = false;
        role = 0;  // Default to Client
        region = 0;
        modemPreset = 0;
        modemPresetPresent = false;
        txEnabled = true;
        txEnabledPresent = false;
        txPower = 22;  // Default
        hopLimit = 3;
        freqSlot = 0;
        gpsMode = GPS_MODE_DISABLED;
        transmitLocation = false;
        bandwidth = 0;
        spreadFactor = 0;
        codingRate = 0;
    }
};

void Config_Init();
void Config_LoadApp();
void Config_SaveApp();
AppConfig* Config_GetApp();
DeviceSettings* Config_GetDevice();

// Helper: ensure storage directory exists
bool Config_EnsureStorageExists();
