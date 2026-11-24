# AtrisWaveVAD Signing, Hyper-V Test VM & Kernel Debugging Runbook

This guide covers building `AtrisWaveVAD.sys` with the Windows 11 WDK, testing safely inside a Hyper-V Windows 11 VM with WinDbg kernel debugging, and submitting for Microsoft Partner Center Attestation Signing.

## 1. Prerequisites (Build Machine)
1. Install **Visual Studio 2022 / 2026** with **Desktop development with C++** (MSVC v143+, Windows 11 SDK).
2. Install the **Windows 11 WDK** matching the installed Windows 11 SDK version.
3. Verify `shared/include/atriswave/driver_ioctl.h` and `ring_buffer_validate.h` pass user-mode unit tests via `ctest --preset msvc-release` from `engine/`.

## 2. Local Build & Test-Signing
From an elevated PowerShell prompt in `driver\scripts`:
```powershell
.\build-and-testsign.ps1 -Configuration Release -Platform x64
```
This builds `driver\build\x64\Release\AtrisWaveVAD.sys`, runs `inf2cat.exe` to generate `AtrisWaveVAD.cat`, and signs both with a local self-signed test certificate (`CN=AtrisWave Test Signing Certificate`).

## 3. Hyper-V Windows 11 Test VM & WinDbg Setup
Never test an unsigned/test-signed kernel driver on your daily host OS.

1. Create a **Generation 2 Hyper-V VM** running Windows 11 (disable Secure Boot in VM Firmware settings while `testsigning` is enabled).
2. Inside the Test VM (elevated Command Prompt):
   ```cmd
   bcdedit /set testsigning on
   bcdedit /debug on
   bcdedit /dbgsettings net hostip:<HOST_IP> port:50000 key:1.2.3.4
   ```
   Reboot the VM.
3. On the host machine, launch **WinDbg (Preview)** -> *Attach to kernel* -> *Net* (`port: 50000`, `key: 1.2.3.4`).
4. Enable **Driver Verifier** on `AtrisWaveVAD.sys` inside the VM before loading:
   ```cmd
   verifier /standard /driver AtrisWaveVAD.sys
   ```
5. Copy `driver\build\x64\Release\` to the VM and install:
   ```powershell
   .\build-and-testsign.ps1 -Configuration Release -Platform x64 -Install
   ```
   Or create the root-enumerated devnode with `devgen.exe /add /bus ROOT /hardwareid ROOT\AtrisWaveVAD` (WDK) and run `pnputil /add-driver AtrisWaveVAD.inf /install`.

## 4. Production Distribution (EV + Partner Center Attestation Signing)
Windows 10/11 with Secure Boot enabled requires Microsoft-signed kernel binaries.
1. Acquire an **EV Code Signing Certificate** (hardware token / HSM) and register your organization in **Microsoft Partner Center (Hardware Dev Center)**.
2. Build `Release` for `x64` (and `ARM64` if targeting Copilot+ PCs).
3. Generate and EV-sign the `.cab` submission package:
   ```powershell
   .\package-attestation-cab.ps1 -Platform x64 -CertThumbprint "<EV_CERT_THUMBPRINT>"
   ```
4. Upload `driver\build\attestation\x64\AtrisWaveVAD-x64.cab` to Partner Center -> *Hardware* -> *Submit new hardware* (Attestation Signing for Windows 10/11).
5. Download the Microsoft-signed `.sys` + `.cat` bundle from Partner Center and ship it in `installer/`.
