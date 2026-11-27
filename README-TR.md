[English](README.md) | Türkçe

# AtrisWave

[![Lisans: Apache-2.0](https://img.shields.io/badge/Lisans-Apache--2.0-blue.svg)](LICENSE)
[![CI](https://github.com/merteren97/AtrisWave/actions/workflows/ci.yml/badge.svg)](https://github.com/merteren97/AtrisWave/actions/workflows/ci.yml)
[![Release](https://github.com/merteren97/AtrisWave/actions/workflows/release.yml/badge.svg)](https://github.com/merteren97/AtrisWave/releases)
[![Platform: Windows 10/11](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](#)
[![Teknoloji: .NET 10 WPF + C++20](https://img.shields.io/badge/Teknoloji-.NET%2010%20WPF%20%2B%20C%2B%2B20-512BD4.svg)](#)

**AtrisWave**; oyuncular, yayıncılar, podcaster'lar ve içerik üreticileri için geliştirilen yerel (native), açık kaynaklı, hepsi-bir-arada Windows ses paketidir. **Sanal kanal ayırma**, **uygulama bazlı ses yönlendirme**, **gerçek zamanlı mikrofon DSP'si**, **düşük gecikmeli çift yollu (dual-path) monitörleme** ve **süreç dışı korumalı CLAP/VST3 eklenti barındırmayı** sade bir masaüstü stüdyosunda birleştirir.

---

## Temel Özellikler

- **Dinamik 16 Kanallı Raf (`+ Kanal Ekle`)**: Varsayılan *System* (genel yakalama) ve *Microphone* kanallarıyla başlayın veya sürükle-bırak uygulama bağlama, fader ve mute/solo kontrolleriyle 16 adede kadar sanal kanalı dinamik olarak ekleyin/yeniden adlandırın/silin.
- **`A1 / A2 / B1 / B2` Donanım ve Sanal Çıkış Matrisi**: Her kanalı 4 ana çıkış veriyoluna (*A1/A2* fiziksel kulaklık/hoparlör, *B1/B2* sanal yayın/sohbet) bağımsız yönlendirin; kanal başı `-0.5 dBFS` lookahead limiter, **Geri Besleme Döngüsü Engelleyici** ve **Process Loopback `A1/A2` Yankı Kilidi** ile güvende kalın.
- **Adanmış Yüksek Çözünürlüklü Ana Saat ve Otomatik Watchdog**: Tam stereo `AdaptivePllResampler` ile 10 ms `CreateWaitableTimerExW` Ana Saat ve motorla otomatik başlatılan `AtrisWaveWatchdog.exe` (3 katmanlı varsayılan cihaz failsafe koruması) sayesinde Windows sesi asla sanal cihazda mahsur kalmaz.
- **12 Tuşlu Gerçek Zamanlı WAV Soundboard**: Özel `.wav` kliplerini doğrudan herhangi bir sanal mikse veya çıkış veriyoluna gerçek zamanlı tetikleyin.
- **WASAPI `IAudioClient3` ve Uygulama Bazlı Yönlendirme**: **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`) ve kalıcı uygulama yönlendirme kuralları ile düşük gecikmeli Windows ses altyapısı.
- **Tam Çift Yollu (Dual-Path) Mikrofon DSP'si**:
  - **Doğrudan Monitör (Sidetone)**: Ağır spektral/reverb aşamalarını atlayarak kendi sesinizi **~6–12 ms** (WASAPI `IAudioClient3`) gecikmeyle duyun.
  - **Yayın Zinciri**: Spektral Gürültü Engelleyici, Noise Gate, 5 Bantlı Cytomic/Simper ZDF SVF Parametrik EQ (**AutoEq `.txt` içe aktarma** ve **Canlı 32 Bantlı FFT Spektrum Analizörü** ile), Kompresör + **Kanallar Arası Sidechain Ducking**, Algoritmik + **FFT Konvolüsyon Reverb** (*Studio Booth*, *Broadcast Room*, *Concert Hall*), Gerçek Zamanlı **Pitch Shifter** (`-12 .. +12 st`) ve **True-Peak Limiter**.
- **Süreç Dışı Eklenti Koruması (`plugin-host/`)**: **CLAP** ve **VST3 (3.8+ MIT)** eklentilerini **5 ms çökme-baypas (crash-bypass)** korumalı izole `AtrisWavePluginHost.exe` sürecinde çalıştırın.
- **Otomasyon, Genel Kısayollar ve Otomatik Güncelleyici**: Yerel Win32 genel kısayolları (`Ctrl+Alt+M` Mikrofon Sessiz, `Ctrl+Alt+S` Sidetone), yalnızca loopback üzerinden çalışan Stream Deck / Touch Portal **JSON-RPC 2.0 API** (`http://127.0.0.1:18923/rpc`) ve **SHA-256** (`SHA256SUMS.txt`) doğrulamalı yerleşik **GitHub Releases Otomatik Güncelleyici**.
- **60 FPS Obsidian Stüdyo Konsol Arayüzü (`v0.1.3`)**: Sade Varsayılan Mikser (açılışta motoru otomatik başlatır ve Hoparlör + Mikrofonu bağlar; `A1/A2/B1/B2` için isteğe bağlı Yayıncı Modu), **5 Tek Tıklamalı Stüdyo Ses Ön Ayarı** ve **A/B Karşılaştırma** özellikli Basit/Pro Mikrofon Stüdyosu, kristal netliğinde **44.1 / 48 / 96 / 192 kHz** örnekleme hızı PLL eşleme, net **SVG vektör ikon seti**, özel 60 FPS `SkiaSharp` Döner Düğmeler / Konsol Fader'ları / VU Metreler, 12 Tuşlu WAV Soundboard, Hızlı Miks Tepsi Penceresi ve Türkçe / İngilizce dil desteği.

---

## 4 Bileşenli Hibrit Mimari

AtrisWave; arayüz çizimini, gerçek zamanlı DSP işlemlerini, üçüncü parti eklenti yürütmesini ve çekirdek (kernel) ses uç noktalarını birbirinden ayırır (ayrıntılı teknik özellikler için [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) dosyasına bakın):

```text
  [ C# .NET 10 WPF Arayüzü ] <── 832B Seqlock SHM V3 (60 FPS) ── [ C++20 Ses Motoru ]
   (ui/ — Wpf.Ui + Skia)      ── Named Pipe JSON Komutları ─────>  (engine/ — Session 1+)
                                                                     │       │       │
                                           5ms Çökme-Baypas SHM ─────┘       │       └── Doğrudan Ring / IOCTL
                                                   v                         v                  v
                                      [ AtrisWavePluginHost.exe ]     [ Watchdog.exe ]  [ AtrisWaveVAD.sys ]
                                      (plugin-host/ — CLAP/VST3)      (Oto-Failsafe)    (driver/ — 16 Slot Havuzu)
```

1. **Masaüstü Arayüzü (`ui/`)**: Sol Fluent Gezinme Çubuğu, özel 60 FPS `SkiaSharp` Döner Düğmeler / Konsol Fader'ları / VU Metreler / 32 bantlı FFT Spektrum EQ, Yandan Açılan Kanal DSP Çekmecesi, EarTrumpet tarzı Hızlı Miks Tepsi Penceresi, özel `atriswave.ico`, genel kısayollar, loopback JSON-RPC sunucusu ve GitHub Releases güncelleme denetleyicisi içeren C# `.NET 10` WPF Stüdyo Konsolu (`AtrisWave.UI.exe`).
2. **Ses Motoru ve Failsafe (`engine/`)**: MMCSS `"Pro Audio"` öncelikli `CreateWaitableTimerExW` Ana Saati, FTZ/DAZ denormal temizleme, `AdaptivePllResampler`, kilitsiz SPSC halka tamponları ve `AtrisWaveWatchdog.exe` varsayılan cihaz failsafe koruması ile çalışan başsız **C++20** Session 1+ motoru (`AtrisWaveEngine.exe`).
3. **Sanal Ses Sürücüsü (`driver/`)**: **16 slotlu `KSJACK_DESCRIPTION.IsConnected` havuzlu uç nokta** mimarisine ve RFC 3161 test imzalama / Attestation Signing betiklerine (`driver/scripts/build-and-testsign.ps1`) sahip C++ WDK **WaveRT / SysVAD** çekirdek sürücüsü (`AtrisWaveVAD.sys`).
4. **Süreç Dışı Eklenti Barındırıcı (`plugin-host/`)**: Kilitsiz paylaşımlı bellek ses aktarımı ve otomatik 5 ms çökme-baypas koruması sunan izole C++20 **CLAP** ve **VST3 (3.8+)** eklenti barındırıcısı (`AtrisWavePluginHost.exe`).

---

## Aşamalı Teslimat ve Sürücü İmzalama Stratejisi

Windows 10/11 64-bit sistemlerde çekirdek sürücülerinin Test-Signing modu açılmadan yüklenebilmesi için **EV Kod İmzalama Sertifikası** ve **Microsoft Hardware Dev Center Attestation Signing** gerekir. AtrisWave her iki modu da sorunsuz destekler:

- **Faz 0 — Temel Altyapı ve Gerçek Zamanlı İskelet**: Ortak C-ABI IPC (`Local\AtrisWaveTelemetryV1` seqlock SHM + geçerli kullanıcı DACL'li `\\.\pipe\AtrisWaveEngine` Named Pipe), C++20 RT temel bileşenleri ve .NET 10 WPF Stüdyo arayüzü.
- **Faz 1 — Sürücüsüz Yönlendirme ve Tam Mikrofon DSP'si**: Windows 10/11 **WASAPI Process Loopback** (`AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`), kalıcı uygulama bazlı yönlendirme, `IAudioClient3` ve tam Çift Yollu Mikrofon DSP zinciri.
- **Faz 2 — Yerel Sanal Ses Sürücüsü (`driver/`)**: Doğrulanmış paylaşımlı halka tamponları ve otomatik test imzalama / Attestation paketleme betikleriyle 6 sanal uç nokta sunan açık kaynak SysVAD/WaveRT sürücüsü (`AtrisWaveVAD.sys`).
- **Faz 3 — Süreç Dışı Eklenti Koruması, Otomasyon ve Dağıtım (`plugin-host/`, `installer/`)**: 5 ms çökme baypaslı korumalı CLAP/VST3 barındırma, AutoEq `.txt` içe aktarma, FFT konvolüsyon reverb, pitch shifter, genel kısayollar, Stream Deck JSON-RPC API, Inno Setup 6 kurulum sihirbazı ve WinGet manifesti.

---

## Derleme, Test ve Paketleme

### Gereksinimler
- Windows 10 (2004+) veya Windows 11 (x64)
- **C++ ile Masaüstü Geliştirme** iş yükü kurulu Visual Studio 2022/2026, CMake 3.25+, Ninja
- [.NET 10 SDK](https://dotnet.microsoft.com/download/dotnet/10.0) (`10.0.x`)
- *(İsteğe bağlı)* Kurulum paketi için Inno Setup 6 (`iscc.exe`) ve `driver/` için Windows WDK

### 1. C++20 Ses Motoru (`engine/`)
**x64 Native Tools Command Prompt for VS** üzerinden (veya `vcvars64.bat` çalıştırdıktan sonra):

```powershell
cd engine
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure
```

### 2. C++20 Süreç Dışı Eklenti Barındırıcı (`plugin-host/`)

```powershell
cd plugin-host
cmake --preset msvc-release
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure
```

### 3. .NET 10 WPF Arayüzü ve Testler (`ui/`)

```powershell
dotnet restore ui/AtrisWave.sln
dotnet build ui/AtrisWave.sln --configuration Release --no-restore
dotnet test ui/AtrisWave.sln --configuration Release --no-build
```

### 4. Sürüm Paketleme ve Kurulum (`installer/`)

```powershell
pwsh -File installer/scripts/build-release.ps1
iscc installer/AtrisWave.iss
```

WinGet paketleme ayrıntıları için [installer/README.md](installer/README.md) ve [installer/winget/AtrisWave.yaml](installer/winget/AtrisWave.yaml) dosyalarına bakın.

---

## Katkıda Bulunma, Güvenlik ve Lisans

- **Katkıda Bulunma**: Katkılarınızı bekliyoruz! Başlamadan önce [CONTRIBUTING.md](CONTRIBUTING.md) ve [Davranış Kurallarımızı (Code of Conduct)](CODE_OF_CONDUCT.md) inceleyin.
- **Güvenlik**: Güvenlik açıklarını lütfen [SECURITY.md](SECURITY.md) dosyasında belirtildiği şekilde GitHub Security Advisories üzerinden gizli olarak bildirin.
- **Lisans**: AtrisWave, [Apache License 2.0](LICENSE) altında lisanslanmıştır.
