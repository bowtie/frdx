# DualSense Validation

Use this checklist to verify the native DualSense trigger port after building.

## Local Checks

Run these before hardware testing:

```powershell
cmake --build build --target version --parallel 4
cmake --build build --target frdx_sandbox --parallel 4
ctest --test-dir build --output-on-failure
```

The CTest suite covers HID report layout, Bluetooth CRC, Forza packet parsing,
effect priority, and DualSense config save/load round trips.

## Sandbox Hardware Check

1. Connect a DualSense or DualSense Edge over USB first.
2. Start `build\frdx_sandbox.exe`.
3. Open settings, expand `DualSense`, then enable `Enable Triggers`.
4. Confirm `Controller` lists the pad. Use `Rescan Controllers` if it was connected after launch.
5. Select the controller or leave `Auto (first found)`.
6. Press `Identify`. Both triggers should briefly stiffen.
7. Press `Send Test`. The status should change to `Telemetry: seen`, and the triggers should react to the synthetic high-RPM/braking packet.
8. Toggle individual L2/R2 effects and repeat `Send Test` to confirm settings apply live.
9. Press `Reset DualSense Defaults`, then repeat `Send Test` to confirm the default feel returns without losing the selected controller or UDP port.

## In-Game Check

1. In Forza, enable Data Out.
2. Set the Data Out IP to `127.0.0.1`.
3. Set the port to the overlay's `UDP Port` value, default `5300`.
4. Start driving and confirm the overlay shows `Telemetry: seen`.
5. Verify these effects:
   - L2 brake resistance increases with brake pressure.
   - L2 ABS pulse masks brake resistance when braking hard with tire slip.
   - R2 throttle resistance is light and progressive.
   - R2 redline buzz fires near the configured rev ratio.
   - R2 wheelspin buzz fires on driven-wheel slip.
   - R2 idle buzz fires while stopped with low throttle.
   - L2/R2 gear shift bursts fire on gear changes when enabled.

## Bluetooth Check

Repeat the sandbox `Identify` and `Send Test` checks over Bluetooth. This
specifically validates the Bluetooth report path and CRC on real hardware.

## Failure Notes

- If the controller is missing, close Steam Input, reWASD, DSX, or anything else that may own the HID gamepad interface.
- If `Identify` works but `Send Test` does not, check that `Enable Triggers` is on and the UDP port is not already in use.
- If Forza never changes to `Telemetry: seen`, recheck Data Out IP, Data Out port, and Windows Firewall.
