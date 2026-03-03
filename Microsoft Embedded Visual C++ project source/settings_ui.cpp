#include "stdafx.h"
#include "settings_ui.h"
#include "app.h"
#include "config.h"
#include "serial.h"
#include "msgstore.h"
#include "nodes.h"

// The Presets combobox stores the Meshtastic enum value in CB_SETITEMDATA.
// This allows us to show presets in any UI order while keeping device values intact.
static int FindComboIndexByItemData(HWND hCombo, int itemData)
{
    if (!hCombo) return -1;
    int count = (int)SendMessage(hCombo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++)
    {
        LRESULT d = SendMessage(hCombo, CB_GETITEMDATA, (WPARAM)i, 0);
        if ((int)d == itemData) return i;
    }
    return -1;
}
#include "meshtastic_protocol.h"


//------------------------------
// Device role -> display string
//------------------------------
static const TCHAR* RoleToString(int role)
{
    // Strings match the role list used by Meshtastic firmware/app terminology.
    switch (role)
    {
        case 0:  return TEXT("Client");
        case 1:  return TEXT("Client Mute");
        case 2:  return TEXT("Router");
        case 3:  return TEXT("Router Client");
        case 4:  return TEXT("Repeater");
        case 5:  return TEXT("Tracker");
        case 6:  return TEXT("Sensor");
        case 7:  return TEXT("TAK");
        case 8:  return TEXT("Client Hidden");
        case 9:  return TEXT("Lost And Found");
        case 10: return TEXT("TAK Tracker");
        case 11: return TEXT("Router Late");
        case 12: return TEXT("Client Base");
        default: return TEXT("(unknown)");
    }
}

//------------------------------
// GPS mode -> display string
//------------------------------
static const TCHAR* GpsModeToString(int gpsMode)
{
    switch (gpsMode)
    {
        case GPS_MODE_ENABLED:     return TEXT("Enabled");
        case GPS_MODE_NOT_PRESENT: return TEXT("Not Present");
        case GPS_MODE_DISABLED:
        default:                   return TEXT("Disabled");
    }
}


//-------------------------------
// Hourglass / fetch indicator
//-------------------------------
static void Settings_SetFetching(bool fetching)
{
    if (fetching)
    {
        if (!g_App.isFetchingSettings)
        {
            g_App.isFetchingSettings = true;
            g_App.hOldCursor = SetCursor(LoadCursor(NULL, IDC_WAIT));
            // Force cursor update on CE
            SetCursor(LoadCursor(NULL, IDC_WAIT));

            // Disable buttons while fetching to prevent multiple outstanding requests
            if (g_App.hSetGetSettings) EnableWindow(g_App.hSetGetSettings, FALSE);
            if (g_App.hSetApply)       EnableWindow(g_App.hSetApply, FALSE);
            if (g_App.hSetSave)        EnableWindow(g_App.hSetSave, FALSE);

        }
    }
    else
    {
        if (g_App.isFetchingSettings)
        {
            g_App.isFetchingSettings = false;
            if (g_App.hOldCursor)
                SetCursor(g_App.hOldCursor);
            else
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            // Re-enable buttons
            if (g_App.hSetGetSettings) EnableWindow(g_App.hSetGetSettings, TRUE);
            if (g_App.hSetApply)       EnableWindow(g_App.hSetApply, TRUE);
            if (g_App.hSetSave)        EnableWindow(g_App.hSetSave, TRUE);

            g_App.hOldCursor = NULL;
        }
    }
}

static void Settings_SetCheckbox(HWND hChk, bool checked)
{
    if (!hChk) return;
    SendMessage(hChk, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(hChk, NULL, TRUE);
    UpdateWindow(hChk);
 UpdateWindow(hChk);
}


void Settings_EnableDeviceFields(bool enable)
{
    char dbgMsg[128];
    sprintf(dbgMsg, "DEBUG: Settings_EnableDeviceFields(%s)", enable ? "TRUE" : "FALSE");
    AppendSerialLog(dbgMsg, false);
    
    // Check if handles are valid
    if (!g_App.hSetRole || !g_App.hSetRegion || !g_App.hSetModemPreset || 
        !g_App.hSetTxEnabled || !g_App.hSetTxPower || !g_App.hSetHopLimit || 
        !g_App.hSetFreqSlot || !g_App.hSetGpsMode)
    {
        AppendSerialLog("DEBUG: ERROR - One or more control handles are NULL!", false);
        sprintf(dbgMsg, "DEBUG: Role=%p Region=%p Preset=%p TxEn=%p TxPwr=%p Hops=%p Freq=%p GPS=%p",
                g_App.hSetRole, g_App.hSetRegion, g_App.hSetModemPreset,
                g_App.hSetTxEnabled, g_App.hSetTxPower, g_App.hSetHopLimit, 
                g_App.hSetFreqSlot, g_App.hSetGpsMode);
        AppendSerialLog(dbgMsg, false);
        return;
    }    EnableWindow(g_App.hSetRegion, enable);
    EnableWindow(g_App.hSetModemPreset, enable);
    EnableWindow(g_App.hSetTxEnabled, enable);
    EnableWindow(g_App.hSetTxPower, enable);
    EnableWindow(g_App.hSetHopLimit, enable);
    EnableWindow(g_App.hSetFreqSlot, enable);
    EnableWindow(g_App.hSetGpsMode, enable);    AppendSerialLog("DEBUG: All controls enabled/disabled successfully", false);
}

void Settings_LoadToUI()
{
    AppendSerialLog("DEBUG: Settings_LoadToUI() START", false);
    
    AppConfig* appCfg = Config_GetApp();
    DeviceSettings* devCfg = Config_GetDevice();

    if (!appCfg || !devCfg)
    {
        AppendSerialLog("DEBUG: ERROR - Config pointers NULL!", false);
        return;
    }

    // Always load app settings
    SetWindowText(g_App.hSetStoragePath, appCfg->storagePath);

    // Set COM port dropdown
    // Index 0 = COM1, Index 1 = COM3 (IR)
    int comboIndex = 0;
    if (appCfg->comPort == 1)
        comboIndex = 0;
    else if (appCfg->comPort == 3)
        comboIndex = 1;
    SendMessage(g_App.hSetComPort, CB_SETCURSEL, comboIndex, 0);

    // Chat timestamp mode
    if (g_App.hSetChatTsMode)
    {
        int tsSel = 0;
        if (appCfg->chatTimestampMode == 1) tsSel = 1;
        SendMessage(g_App.hSetChatTsMode, CB_SETCURSEL, tsSel, 0);
    }

    // Chat auto-scroll behavior (dropdown)
    if (g_App.hSetChatFollow)
    {
        int sel = appCfg->chatAutoScroll;
        if (sel < 0 || sel > 2) sel = 1;
        SendMessage(g_App.hSetChatFollow, CB_SETCURSEL, (WPARAM)sel, 0);
    }

    // Jornada LED checkboxes
    if (g_App.hSetLedNewMsg)
        SendMessage(g_App.hSetLedNewMsg, BM_SETCHECK, appCfg->ledNewMessage ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_App.hSetLedUnread)
        SendMessage(g_App.hSetLedUnread, BM_SETCHECK, appCfg->ledUnread ? BST_CHECKED : BST_UNCHECKED, 0);

    // Power management
    if (g_App.hSetPreventSleep)
        SendMessage(g_App.hSetPreventSleep, BM_SETCHECK, appCfg->preventSleepWhileConnected ? BST_CHECKED : BST_UNCHECKED, 0);

    // Serial monitor options
    if (g_App.hSetSerialMonitorEnable)
        SendMessage(g_App.hSetSerialMonitorEnable, BM_SETCHECK, appCfg->serialMonitorEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_App.hSetSerialMaxLines)
    {
        TCHAR buf[32];
        _stprintf(buf, TEXT("%d"), appCfg->serialMonitorMaxLines);
        SetWindowText(g_App.hSetSerialMaxLines, buf);
    }

    // Ensure the serial monitor tab reflects the current enable/disable setting.
    UI_RefreshSerialMonitorState();

    char dbg[128];
    sprintf(dbg, "DEBUG: hasReceivedConfig = %d", devCfg->hasReceivedConfig ? 1 : 0);
    AppendSerialLog(dbg, false);

    // Only load device settings if we've received config from device
    if (devCfg->hasReceivedConfig)
    {
        AppendSerialLog("DEBUG: Config received, enabling and loading fields...", false);
        
        Settings_EnableDeviceFields(true);
        
        sprintf(dbg, "DEBUG: Setting Role=%d, Region=%d, Preset=%d", devCfg->role, devCfg->region, devCfg->modemPreset);
        AppendSerialLog(dbg, false);
        
        SetWindowText(g_App.hSetRole, RoleToString(devCfg->role));
        SendMessage(g_App.hSetRegion, CB_SETCURSEL, devCfg->region, 0);

        // Presets combobox uses item-data (Meshtastic enum value) instead of relying on list order.
        int presetIdx = FindComboIndexByItemData(g_App.hSetModemPreset, devCfg->modemPreset);
        if (presetIdx >= 0)
        {
            SendMessage(g_App.hSetModemPreset, CB_SETCURSEL, (WPARAM)presetIdx, 0);
        }
        else
        {
            // Unknown/unsupported preset value from device
            SendMessage(g_App.hSetModemPreset, CB_SETCURSEL, -1, 0);
            char msgU[96];
            sprintf(msgU, "DEBUG: Unknown modemPreset=%d (no matching UI entry)", devCfg->modemPreset);
            AppendSerialLog(msgU, false);
        }

        // Mirror the current modem preset into the main public chat window indicator.
        UI_UpdateChatPresetIndicator();
        
        {
            char m2[64];
            sprintf(m2, "DEBUG: UI set txEnabled=%d", devCfg->txEnabled ? 1 : 0);
            AppendSerialLog(m2, false);
            Settings_SetCheckbox(g_App.hSetTxEnabled, devCfg->txEnabled);
        }

        TCHAR buf[32];
        _stprintf(buf, TEXT("%d"), devCfg->txPower);
        SetWindowText(g_App.hSetTxPower, buf);
        
        _stprintf(buf, TEXT("%d"), devCfg->hopLimit);
        SetWindowText(g_App.hSetHopLimit, buf);
        
        _stprintf(buf, TEXT("%d"), devCfg->freqSlot);
        SetWindowText(g_App.hSetFreqSlot, buf);
        
        {
            char m3[64];
            sprintf(m3, "DEBUG: UI set gpsMode=%d", devCfg->gpsMode);
            AppendSerialLog(m3, false);
            // Read-only indicator
            SetWindowText(g_App.hSetGpsMode, GpsModeToString(devCfg->gpsMode));
        }

        AppendSerialLog("DEBUG: All fields updated!", false);
    }
    else
    {
        AppendSerialLog("DEBUG: Config NOT received, disabling fields", false);
        
        // No config received yet - disable and clear device fields
        Settings_EnableDeviceFields(false);
        
        SetWindowText(g_App.hSetRole, TEXT(""));
        SendMessage(g_App.hSetRegion, CB_SETCURSEL, -1, 0);
        SendMessage(g_App.hSetModemPreset, CB_SETCURSEL, -1, 0);
        SendMessage(g_App.hSetTxEnabled, BM_SETCHECK, BST_UNCHECKED, 0);
        SetWindowText(g_App.hSetTxPower, TEXT(""));
        SetWindowText(g_App.hSetHopLimit, TEXT(""));
        SetWindowText(g_App.hSetFreqSlot, TEXT(""));
        SetWindowText(g_App.hSetGpsMode, TEXT(""));
        // Clear the chat preset indicator until we have config from device.
        UI_UpdateChatPresetIndicator();
            }
    
    AppendSerialLog("DEBUG: Settings_LoadToUI() END", false);
}

void Settings_OnAbout()
{
    // Simple modal popup. Works well on WinCE and doesn't require dialog resources.
    const TCHAR* title = TEXT("About");
    const TCHAR* body =
        TEXT("Meshtastic for Jornada 720 (Windows CE)\r\n")
        TEXT("\r\n")
        TEXT("Made by: NorthPolePoint\r\n")
        TEXT("Date: 05.Jan.2026\r\n")
        TEXT("\r\n")
        TEXT("A lightweight Meshtastic client for classic HP Jornada/WinCE devices.\r\n")
        TEXT("Features chat, direct messages, node list, map view, settings sync,\r\n")
        TEXT("and a serial monitor for debugging and development.");

    HWND hOwner = g_App.hMain ? g_App.hMain : NULL;
    MessageBox(hOwner, body, title, MB_OK | MB_ICONINFORMATION);
}

void Settings_SaveFromUI()
{
    AppConfig* appCfg = Config_GetApp();
    DeviceSettings* devCfg = Config_GetDevice();

    int prevTsMode = appCfg ? appCfg->chatTimestampMode : 0;

    // Always save app settings
    GetWindowText(g_App.hSetStoragePath, appCfg->storagePath, MAX_PATH);

    // Get COM port from dropdown
    // Index 0 = COM1, Index 1 = COM3 (IR)
    int comboIndex = (int)SendMessage(g_App.hSetComPort, CB_GETCURSEL, 0, 0);
    if (comboIndex == 0)
        appCfg->comPort = 1; // COM1
    else if (comboIndex == 1)
        appCfg->comPort = 3; // COM3 (IR)
    else
        appCfg->comPort = 1; // Default

    // Chat timestamp mode
    if (g_App.hSetChatTsMode)
    {
        int selTs = (int)SendMessage(g_App.hSetChatTsMode, CB_GETCURSEL, 0, 0);
        appCfg->chatTimestampMode = (selTs == 1) ? 1 : 0;
    }

    // Chat auto-scroll behavior (dropdown)
    if (g_App.hSetChatFollow)
    {
        int selScroll = (int)SendMessage(g_App.hSetChatFollow, CB_GETCURSEL, 0, 0);
        if (selScroll < 0 || selScroll > 2) selScroll = 1;
        appCfg->chatAutoScroll = selScroll;
    }


    // Jornada button LED options
    if (g_App.hSetLedNewMsg)
        appCfg->ledNewMessage = (SendMessage(g_App.hSetLedNewMsg, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    if (g_App.hSetLedUnread)
        appCfg->ledUnread = (SendMessage(g_App.hSetLedUnread, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

    // Prevent sleep while connected
    if (g_App.hSetPreventSleep)
        appCfg->preventSleepWhileConnected = (SendMessage(g_App.hSetPreventSleep, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;

    // Serial monitor options
    if (g_App.hSetSerialMonitorEnable)
        appCfg->serialMonitorEnabled = (SendMessage(g_App.hSetSerialMonitorEnable, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    if (g_App.hSetSerialMaxLines)
    {
        TCHAR buf[32];
        GetWindowText(g_App.hSetSerialMaxLines, buf, 32);
        appCfg->serialMonitorMaxLines = _ttoi(buf);
    }

    // Apply serial monitor UI state immediately
    UI_RefreshSerialMonitorState();

    // Apply LED changes immediately
    LED_UpdateFromConfig();
    // If the user changed timestamp display mode, immediately re-render
    // existing histories (main chat + currently-selected DM).
    if (appCfg && appCfg->chatTimestampMode != prevTsMode)
    {
        if (g_App.hChatHistory)
            MsgStore_LoadChatHistory(g_App.hChatHistory);

        Direct_RefreshTimestampDisplay();
    }

    // Only save device settings if we've received config from device
    if (devCfg->hasReceivedConfig)
    {
        devCfg->region = (int)SendMessage(g_App.hSetRegion, CB_GETCURSEL, 0, 0);
        // Preset value is stored in item-data
        int selPreset = (int)SendMessage(g_App.hSetModemPreset, CB_GETCURSEL, 0, 0);
        if (selPreset >= 0)
            devCfg->modemPreset = (int)SendMessage(g_App.hSetModemPreset, CB_GETITEMDATA, (WPARAM)selPreset, 0);
        
        devCfg->txEnabled = (SendMessage(g_App.hSetTxEnabled, BM_GETCHECK, 0, 0) == BST_CHECKED);

        TCHAR buf[32];
        GetWindowText(g_App.hSetTxPower, buf, 32);
        devCfg->txPower = _ttoi(buf);
        
        GetWindowText(g_App.hSetHopLimit, buf, 32);
        devCfg->hopLimit = _ttoi(buf);
        
        GetWindowText(g_App.hSetFreqSlot, buf, 32);
        devCfg->freqSlot = _ttoi(buf);

        // GPS mode is a read-only indicator (value comes from the device)
        // so we do not overwrite devCfg->gpsMode from the UI.
    }
}

void Settings_OnConnect()
{
    if (Serial_IsOpen())
    {
        Serial_Close();
        Meshtastic_OnDisconnected();
                Settings_SetFetching(false);
SetWindowText(g_App.hSetConnect, TEXT("Connect"));
        UpdateWindow(g_App.hSetConnect);

        // Keep top bar button in sync
        UI_UpdateTopConnectButton();
        
        // Log disconnection
        AppendSerialLog("=== DISCONNECTED ===", false);
    }
    else
    {
        Settings_SaveFromUI();
        AppConfig* appCfg = Config_GetApp();
        
        // Show attempt message
        char attempt[128];
        sprintf(attempt, "=== Attempting to open COM%d ===", appCfg->comPort);
        AppendSerialLog(attempt, false);

        if (Serial_Open(appCfg->comPort))
        {
            SetWindowText(g_App.hSetConnect, TEXT("Disconnect"));
            UpdateWindow(g_App.hSetConnect);

            // Keep top bar button in sync
            UI_UpdateTopConnectButton();
            
            // Log success
            char msg[128];
            sprintf(msg, "=== CONNECTED to COM%d at 115200 baud ===", appCfg->comPort);
            AppendSerialLog(msg, false);
            AppendSerialLog("Waiting for data...", false);

            // Config fetch starts immediately
            Settings_SetFetching(true);

            Meshtastic_OnConnected();
            
            // Note: Meshtastic_OnConnected() automatically requests config
        }
        else
        {
            // Ensure UI reflects failure
            UI_UpdateTopConnectButton();
            // Log failure too
            char err[128];
            sprintf(err, "=== FAILED to open COM%d ===", appCfg->comPort);
            AppendSerialLog(err, false);
            
            MessageBox(g_App.hMain, TEXT("Failed to open COM port!\r\nCheck COM port number in Settings."),
                      TEXT("Connection Error"), MB_OK | MB_ICONERROR);
        }
    }
}

void Settings_OnApply()
{
    if (!Serial_IsOpen())
    {
        MessageBox(g_App.hMain, TEXT("Please connect to device first!"),
                  TEXT("Not Connected"), MB_OK | MB_ICONWARNING);
        return;
    }

    // Save UI values to device settings structure first
    Settings_SaveFromUI();

    AppendSerialLog("Applying settings to device...", false);

    DeviceSettings* devCfg = Config_GetDevice();
    if (!devCfg || !devCfg->hasReceivedConfig)
    {
        MessageBox(g_App.hMain,
                  TEXT("Device settings have not been read yet.\r\n\r\n")
                  TEXT("Click 'Get settings from device' first."),
                  TEXT("No Device Config"), MB_OK | MB_ICONWARNING);
        return;
    }

    if (!Meshtastic_SendDeviceSettings(devCfg))
    {
        MessageBox(g_App.hMain,
                  TEXT("Failed to send settings to device (serial not ready)."),
                  TEXT("Send Failed"), MB_OK | MB_ICONERROR);
        return;
    }

    MessageBox(g_App.hMain,
              TEXT("Settings were sent to the device.\r\n\r\n")
              TEXT("Note: The radio may briefly reboot to apply changes."),
              TEXT("Settings Sent"), MB_OK | MB_ICONINFORMATION);
}

void Settings_OnSave()
{
    Settings_SaveFromUI();
    Config_SaveApp();  // Only save app settings (storage path, COM port) to INI

    MessageBox(g_App.hMain, TEXT("App settings saved successfully!\r\n\r\nNote: Device settings are read from the Meshtastic device."),
              TEXT("Saved"), MB_OK | MB_ICONINFORMATION);
}

void Settings_OnGetSettings()
{
    AppendSerialLog("DEBUG: Settings_OnGetSettings() called", false);

    // Prevent spamming requests; only one config fetch at a time
    if (g_App.isFetchingSettings)
    {
        AppendSerialLog("DEBUG: Ignoring GetSettings - already fetching", false);
        return;
    }
    
    if (!Serial_IsOpen())
    {
        AppendSerialLog("DEBUG: Serial is NOT open", false);
        MessageBox(g_App.hMain, TEXT("Not connected to device.\r\nClick Connect first."),
                  TEXT("Not Connected"), MB_OK | MB_ICONWARNING);
        return;
    }

    AppendSerialLog("DEBUG: Serial IS open, requesting config", false);
    
    // Show hourglass cursor while fetching settings (async)
    
    // Reset device config snapshot so a fresh fetch always overwrites any unsaved UI edits
    // and so missing fields in the new response don't leave stale values.
    {
        DeviceSettings* devCfg = Config_GetDevice();
        if (devCfg)
        {
            devCfg->hasReceivedConfig = false;
            devCfg->role = 0;
            devCfg->region = 0;
            devCfg->modemPreset = MODEM_PRESET_LONG_FAST;
            devCfg->modemPresetPresent = false;
            devCfg->txEnabled = false;
            devCfg->txEnabledPresent = false;
            devCfg->txPower = 0;
            devCfg->hopLimit = 3;
            devCfg->freqSlot = 0;
            devCfg->gpsMode = GPS_MODE_DISABLED;
            devCfg->transmitLocation = false;
            devCfg->bandwidth = 0;
            devCfg->spreadFactor = 0;
            devCfg->codingRate = 0;
        }
    }

Settings_SetFetching(true);

AppendSerialLog("Requesting config (want_config)...", false);
    
    bool result = Meshtastic_RequestConfig();
    if (result)
        AppendSerialLog("DEBUG: Meshtastic_RequestConfig() returned TRUE", false);
    else
        AppendSerialLog("DEBUG: Meshtastic_RequestConfig() returned FALSE", false);


    if (!result)
        Settings_SetFetching(false);
}

void Settings_OnDeviceConfigReceived()
{
    AppendSerialLog("DEBUG: ===== Settings_OnDeviceConfigReceived() START =====", false);
    
    // This is called when config_complete is received from device
    DeviceSettings* devCfg = Config_GetDevice();
    
    if (!devCfg)
    {
        AppendSerialLog("DEBUG: ERROR - devCfg is NULL!", false);
        return;
    }
    
    AppendSerialLog("DEBUG: devCfg pointer valid, setting hasReceivedConfig=true", false);
    
    // Mark that we've received config
    devCfg->hasReceivedConfig = true;
    
    // Config values have already been parsed from the protobuf messages
    // by HandleConfig() as they arrived. We just need to update the UI now.
    
    char msg[256];
    sprintf(msg, "DEBUG: Final config values: role=%d, region=%d, preset=%d, hop=%d, tx=%d, txPwr=%d, freq=%d, gps=%d, txLoc=%d",
            devCfg->role, devCfg->region, devCfg->modemPreset, devCfg->hopLimit, 
            devCfg->txEnabled ? 1 : 0, devCfg->txPower, devCfg->freqSlot,
            devCfg->gpsMode, devCfg->transmitLocation ? 1 : 0);
    AppendSerialLog(msg, false);
    
    AppendSerialLog("DEBUG: Calling Settings_LoadToUI()...", false);
    
    // Update the UI with the received settings
    Settings_LoadToUI();
    
    AppendSerialLog("DEBUG: Settings_LoadToUI() completed", false);

    // Stop hourglass
    Settings_SetFetching(false);

    AppendSerialLog("Device settings received and loaded to UI.", false);
    AppendSerialLog("DEBUG: ===== Settings_OnDeviceConfigReceived() END =====", false);
}
