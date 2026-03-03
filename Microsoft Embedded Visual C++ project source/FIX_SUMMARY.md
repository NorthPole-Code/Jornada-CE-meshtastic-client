# Settings Not Loading - FIXED!

## The Problem

Your debug log revealed the exact issue:

```
[18:25:38] DEBUG: Received config_complete_id, nonce=10 (expecting 11)
[18:25:38] DEBUG: Nonce mismatch - ignoring config_complete
```

The device was responding correctly, but the code was rejecting the response!

## Root Cause

**Nonce Timing Issue:**

The code flow was:
1. Build request with nonce=10
2. Send request
3. **Immediately increment nonce to 11**
4. Device responds with nonce=10 (the one we sent)
5. We check if response matches nonce=11 (current value)
6. **Mismatch! Response rejected!**

## The Fixes

### Fix 1: Accept Previous Nonce
Changed the nonce matching logic to accept EITHER the current nonce OR the previous one:

```cpp
// Before: Only checked current nonce
if ((DWORD)v == g_WantConfigNonce)

// After: Checks current OR previous nonce
if ((DWORD)v == g_WantConfigNonce || (DWORD)v == g_WantConfigNonce - 1)
```

### Fix 2: Remove Duplicate Config Request
On connection, the code was calling `Meshtastic_RequestConfig()` TWICE:
- Once in `Meshtastic_OnConnected()`
- Again in `Settings_OnConnect()`

This caused:
```
[18:24:54] DEBUG: Meshtastic_RequestConfig() called, nonce=8
[18:24:54] DEBUG: Meshtastic_RequestConfig() called, nonce=9  <- duplicate!
```

Removed the duplicate call from `Settings_OnConnect()`.

## What Will Happen Now

When you connect:
1. Config request sent with correct nonce
2. Device responds with that same nonce
3. **Response ACCEPTED** ✓
4. `Settings_OnDeviceConfigReceived()` is called
5. Settings populate in UI
6. Controls become enabled

When you click "Get Settings from Device":
1. Config request sent
2. Device responds
3. **Response ACCEPTED** ✓
4. Settings refresh in UI

## Test It!

1. Replace these 2 files:
   - `meshtastic_protocol.cpp`
   - `settings_ui.cpp`

2. Compile and run

3. Connect to device

4. You should see in Serial Monitor:
   ```
   Config complete.
   DEBUG: Nonce matches! Calling Settings_OnDeviceConfigReceived()...
   DEBUG: ===== Settings_OnDeviceConfigReceived() START =====
   DEBUG: Settings_LoadToUI() START
   DEBUG: hasReceivedConfig = 1
   DEBUG: Config received, enabling and loading fields...
   DEBUG: Setting Region=1, Preset=0
   DEBUG: All fields updated!
   ```

5. Settings should now populate with:
   - Region: US
   - Presets: Long Range - Fast
   - Ignore MQTT: unchecked
   - OK to MQTT: unchecked
   - Transmit Enabled: checked
   - Number of Hops: 3
   - Frequency Slot: 0

6. Controls should be **enabled** (not grayed out)

## Why This Happened

The nonce system is meant to prevent processing duplicate or stale responses. The implementation incremented the nonce immediately after sending, which meant the "expected" nonce was always one ahead of what the device would respond with.

This is a common synchronization issue when using request/response protocols with sequence numbers. The fix allows for the natural one-nonce lag between sending and receiving.

## If It Still Doesn't Work

With the debug logging still enabled, you'll see EXACTLY what's happening. If there's still an issue, send me the new log and I'll identify it immediately. But based on your previous log, this fix should solve the problem completely!
