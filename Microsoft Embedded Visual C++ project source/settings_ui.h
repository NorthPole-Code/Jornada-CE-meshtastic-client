#pragma once

#include <windows.h>

// Initialize settings UI with current config values
void Settings_LoadToUI();

// Save settings from UI to config
void Settings_SaveFromUI();

// Handle connect button
void Settings_OnConnect();

// Handle apply button
void Settings_OnApply();

// Handle save button
void Settings_OnSave();

// Handle get settings button
void Settings_OnGetSettings();

// Show About popup
void Settings_OnAbout();

// Called when device config is received
void Settings_OnDeviceConfigReceived();

// Enable/disable device settings fields
void Settings_EnableDeviceFields(bool enable);
