[English](README.md) | Türkçe

# AtrisWave

[![Lisans: Apache-2.0](https://img.shields.io/badge/Lisans-Apache--2.0-blue.svg)](LICENSE)
[![Platform: Windows 10/11](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](#)
[![Teknoloji: .NET 10 WPF + C++20](https://img.shields.io/badge/Teknoloji-.NET%2010%20WPF%20%2B%20C%2B%2B20-512BD4.svg)](#)

**AtrisWave**; oyuncular, yayıncılar, podcaster'lar ve içerik üreticileri için geliştirilen yerel (native), açık kaynaklı, hepsi-bir-arada Windows ses paketidir. **Sanal kanal ayırma**, **uygulama bazlı ses yönlendirme**, **gerçek zamanlı mikrofon DSP'si** ve **düşük gecikmeli çift yollu (dual-path) monitörlemeyi** sade ve yüksek performanslı bir masaüstü stüdyosunda birleştirir.

---

## Vizyon ve Temel Özellikler

- **Sanal Kanal Ayırma**: *Master*, *Game*, *Chat*, *Media*, *Aux* ve *Microphone* kanallarını bağımsız fader'lar, sessize alma (mute) / solo kontrolleri ve yayın/monitör alt miksleriyle yönetin.
- **Uygulama Bazlı Yönlendirme**: Windows uygulamalarının sesini kırılgan yöntemlere başvurmadan temiz bir şekilde ayırın ve yönlendirin.
- **Gerçek Zamanlı Mikrofon DSP'si**: Yapay zeka gürültü engelleme, noise gate, parametrik EQ (Cytomic/Simper ZDF SVF), kompresör, reverb ve limiter içeren düşük gecikmeli ses işleme zinciri.
- **Çift Yollu Düşük Gecikmeli Monitörleme**: Kulaklığınızda kendi sesinizi **~6–12 ms** (WASAPI `IAudioClient3`) gecikmeyle duyarken, yapay zeka gürültü engellemeli tam yayın zincirini OBS veya Discord'a gönderin.
- **Genişletilebilir Eklenti Desteği (Planlanan)**: Süreç dışı (out-of-process) korumalı **CLAP** ve **VST3 (3.8+)** eklenti barındırma.
- **60 FPS Stüdyo Arayüzü**: Bellek ayırmasız 60 FPS `SkiaSharp` VU metreler, sistem tepsisi Hızlı Miks (Quick-Mix) menüsü ve anlık Türkçe / İngilizce dil desteği sunan modern Windows 11 koyu Fluent arayüz (`Wpf.Ui`).

---

## 3 Katmanlı Hibrit Mimari

AtrisWave; arayüz çizimini, gerçek zamanlı DSP işlemlerini ve çekirdek (kernel) ses uç noktalarını üç katmana ayırır (ayrıntılı teknik özellikler için [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) dosyasına bakın):

```text
  [ C# .NET 10 WPF Arayüzü ] <── 576B Seqlock SHM (60 FPS) ── [ C++20 Ses Motoru ]
   (ui/ — Wpf.Ui + Skia)      ── Named Pipe JSON Komutları ─>  (engine/ — Session 1+)
                                                                       │
                                                       Faz 2: Doğrudan Ring / IOCTL
                                                                       v
                                                       [ AtrisWaveVAD.sys (WDK) ]
                                                        (driver/ — WaveRT/SysVAD)
```

1. **Masaüstü Arayüzü (`ui/`)**: `Wpf.Ui` Fluent teması, `SkiaSharp.Views.WPF` (`SKElement`) 60 FPS metreler ve `H.NotifyIcon.Wpf` sistem tepsisi entegrasyonuna sahip C# `.NET 10` WPF uygulaması.
2. **Ses Motoru (`engine/`)**: MMCSS `"Pro Audio"` önceliği, FTZ/DAZ denormal temizleme, kilitsiz (lock-free) SPSC halka tamponları ve sıfır bellek ayırmalı DSP ile çalışan başsız **C++20** Session 1+ arka plan motoru (`AtrisWaveEngine.exe`).
3. **Sanal Ses Sürücüsü (`driver/`)**: Doğal sanal oynatma ve kayıt uç noktaları sunan, Faz 2 için planlanmış C++ WDK **WaveRT / SysVAD** çekirdek sürücüsü (`AtrisWaveVAD.sys`).

---

## Aşamalı Yol Haritası ve Sürücü İmzalama Stratejisi

Windows 10/11 64-bit sistemlerde çekirdek sürücülerinin Test-Signing modu açılmadan yüklenebilmesi için **EV Kod İmzalama Sertifikası** ve **Microsoft Hardware Dev Center Attestation Signing** gerekir. AtrisWave, kullanıcıların ilk günden değer elde edebilmesi için aşamalı bir strateji izler:

- **Faz 0 — Temel Altyapı ve Gerçek Zamanlı İskelet (Mevcut)**
  - Ortak C-ABI IPC sözleşmesi (`Local\AtrisWaveTelemetryV1` seqlock paylaşımlı bellek + geçerli kullanıcı DACL'li `\\.\pipe\AtrisWaveEngine` Named Pipe).
  - C++20 motor iskeleti (`RtSafety`, `SpscRingBuffer`, `SimperSvf`, `TelemetryPublisher`, `ControlServer`).
  - 6 kanallı mikser, 60 FPS SkiaSharp VU metreler, Sistem Tepsisi ve TR/EN yerelleştirme içeren .NET 10 WPF Stüdyo arayüzü.
- **Faz 1 — Sürücüsüz Yönlendirme ve Mikrofon DSP'si**
  - Windows 10/11 **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) ile uygulama bazlı ses yakalama + isteğe bağlı imzalı sanal kablo köprüsü (örn. VB-Cable).
  - Tam Çift Yollu Mikrofon DSP zinciri (DeepFilterNet3-LL AI gürültü engelleme, gate, EQ, kompresör, reverb, limiter).
- **Faz 2 — Yerel Sanal Ses Sürücüsü (`AtrisWaveVAD.sys`)**
  - `AtrisWaveEngine.exe` ile doğrudan paylaşımlı bellek halka tamponu üzerinden haberleşen, Attestation imzalı açık kaynak SysVAD/WaveRT sürücüsü.
- **Faz 3 — Süreç Dışı Eklenti Barındırıcı (`plugin-host/`)**
  - Çökme izolasyonlu korumalı **CLAP** ve **VST3 (3.8+)** eklenti desteği.

---

## Derleme ve Test Talimatları

### Gereksinimler
- Windows 10 (2004+) veya Windows 11 (x64)
- **C++ ile Masaüstü Geliştirme** iş yükü kurulu Visual Studio 2022/2026, CMake 3.25+, Ninja
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (`10.0.x`)

### C++20 Ses Motorunu Derleme ve Test Etme (`engine/`)
**x64 Native Tools Command Prompt for VS** üzerinden (veya `vcvars64.bat` çalıştırdıktan sonra):

```powershell
cd engine
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure
```

*(Release derlemeleri için `msvc-debug` yerine `msvc-release` kullanın.)*

### .NET 10 WPF Arayüzünü Derleme ve Test Etme (`ui/`)

```powershell
dotnet build ui/AtrisWave.sln --configuration Release
dotnet test ui/AtrisWave.sln --configuration Release
```

---

## Katkıda Bulunma, Güvenlik ve Lisans

- **Katkıda Bulunma**: Katkılarınızı bekliyoruz! Başlamadan önce [CONTRIBUTING.md](CONTRIBUTING.md) ve [Davranış Kurallarımızı (Code of Conduct)](CODE_OF_CONDUCT.md) inceleyin.
- **Güvenlik**: Güvenlik açıklarını lütfen [SECURITY.md](SECURITY.md) dosyasında belirtildiği şekilde GitHub Security Advisories üzerinden gizli olarak bildirin.
- **Lisans**: AtrisWave, [Apache License 2.0](LICENSE) altında lisanslanmıştır.
