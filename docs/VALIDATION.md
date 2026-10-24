# Validation scope

Version 0.1.0 uses unchanged Vdevice SDK and signed driver packages. The app does not install a new kernel component.

- C++17 x64 Release builds: public demo SDK and private production SDK override.
- Compiler warnings treated as errors; Control Flow Guard and normal Windows image mitigations enabled.
- Two offline CTest suites pass for both SDK variants and under AddressSanitizer.
- 21,302 assertions cover descriptors, invalid CDC controls, line-state updates, strict byte queue bounds, explicit FIFO clearing, and 10,000 binary FIFO roundtrips.
- The UI screenshot is an explicitly offline preview. No host driver was opened, no physical COM port was used, and no host input was captured or sent during these checks.

The separate VM diagnostic is designed to exercise real Windows `usbser.sys` binding and two 4,096-byte application-level serial transfers through owned virtual devices. The first Windows 10 VM campaign on 2026-10-24 **did not pass**: Windows selected `usbser.inf`, but only one of the two COM names appeared before the bounded enumeration timeout. The other node remained in configuration-pending state. No binary transfer was verified. Normal guest shutdown also failed after the test; its saved memory was preserved privately, and the new Off baseline was restored without forced power-off. All nine historical checkpoints and the restored disk chain were verified. Do not distribute this build as a verified working serial cable until that problem is resolved and the live diagnostic passes.

Detailed JSON, SetupAPI output, the installed Microsoft-signed driver hash and VM checkpoint recovery metadata are retained privately by the maintainer. The original test harness's numeric process exit result was unreliable; the explicit failure JSON is authoritative, and the harness now requires a successful JSON report. Offline test success does not establish live serial compatibility.

This is not a certification test, long-duration hardware stress result, guarantee for every Windows installation, UART electrical test, or game/input latency benchmark. Physical equipment and production integrations require their own acceptance tests.

COM discovery requires an exact owned USB serial ancestor, a started Windows devnode and zero PnP problem code. It does not require the USB device to remain powered while idle: opening a started usbser port can wake selective suspend. This readiness correction has offline build coverage only and does not establish that the observed startup stall is fixed.
