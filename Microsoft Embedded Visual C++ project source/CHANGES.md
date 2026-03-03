# Meshtastic Jornada Client - Settings Update

## Summary of Changes

This update reorganizes the settings system to separate app-specific settings from device settings, updates the UI to match standard Meshtastic presets, and implements automatic device configuration retrieval.

## Key Changes

### 1. COM Port Configuration
- **Removed**: COM2, COM4-COM9 from dropdown
- **Kept**: COM1 (Serial) and COM3 (Infrared)
- COM3 is the infrared port on HP Jornada 720/728 devices
- Settings now correctly map to physical ports

### 2. Settings Separation (config.h / config.cpp)
**App Settings (persisted to INI file):**
- Storage path
- COM port selection

**Device Settings (read from Meshtastic device):**
- GPS enabled
- Channel index
- Channel name/preset
- Region
- Modem preset
- Node name
- Router mode

### 3. Channel Name / Modem Preset Updates
Both Channel Name and Modem Preset are now dropdowns with the 8 standard Meshtastic options:
1. Long Range - Fast (LONG_FAST)
2. Long Range - Moderate (LONG_MODERATE) - **NEW**
3. Long Range - Slow (LONG_SLOW)
4. Medium Range - Fast (MEDIUM_FAST)
5. Medium Range - Slow (MEDIUM_SLOW)
6. Short Range - Turbo (SHORT_TURBO) - **NEW**
7. Short Range - Fast (SHORT_FAST)
8. Short Range - Slow (SHORT_SLOW)

### 4. Automatic Device Configuration
- Device settings are now automatically requested after successful connection
- Settings are read from the Meshtastic device, not from INI file
- "Get Settings from Device" button explicitly requests current device configuration
- "Apply to Device" button prepared for future implementation of config write

### 5. Settings UI Behavior
- **On Connect**: Automatically requests device settings
- **Save Settings**: Only saves app settings (storage path, COM port) to INI
- **Get Settings from Device**: Manually requests current device configuration
- **Apply to Device**: Shows "not yet implemented" message (prepared for future update)

## Modified Files

1. **config.h**
   - Split `MeshtasticConfig` into `AppConfig` and `DeviceSettings`
   - Added modem preset constants
   - Updated function prototypes

2. **config.cpp**
   - Renamed `Config_Load()` to `Config_LoadApp()`
   - Renamed `Config_Save()` to `Config_SaveApp()`
   - Removed device settings from INI file operations
   - Updated accessor functions

3. **settings_ui.cpp**
   - Updated to use separated config structures
   - Modified COM port mapping (index 0=COM1, index 1=COM3)
   - Changed channel name handling for dropdown
   - Added automatic config request on connect
   - Updated save behavior to only persist app settings

4. **ui_main.cpp**
   - Reduced COM port dropdown to 2 options
   - Converted Channel Name from EDIT to COMBOBOX
   - Updated Modem Preset dropdown with all 8 options
   - Both dropdowns now use consistent naming

5. **MESHTASTIC.cpp**
   - Updated initialization to call `Config_LoadApp()`

## How It Works Now

### First Connection
1. User selects COM port (COM1 or COM3)
2. User clicks "Connect"
3. App opens serial connection
4. App automatically requests device settings
5. Device responds with current configuration
6. UI updates with device settings

### Settings Management
- **App Settings**: Stored in INI file, persists between sessions
- **Device Settings**: Read from device, reflects actual device state
- User can manually refresh device settings with "Get Settings from Device"
- Future: "Apply to Device" will write settings back to device

## Backwards Compatibility

- Existing INI files will load correctly (only app settings are read)
- Device settings will be populated from device on first connection
- Old device settings in INI files are ignored

## Future Enhancements

- Implement "Apply to Device" functionality via Meshtastic protobuf protocol
- Add validation for device setting changes
- Consider caching device settings between connections
