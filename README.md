# Pocket

**A quieter pocket.** Pocket Classic is a calm e-ink device for notes, lists, music and reading. It works on its own, and the phone Companion adds sync, backup and linking when you want them.

[Product page](https://northwork.app/pocket) · [Get Companion](https://northwork.app/pocket/app/get-companion) · [Developers](https://northwork.app/pocket/app/developer) · [Support](https://northwork.app/support)

---

## About this repository

This is the source for the whole Pocket platform: the device firmware, the Companion phone app and the Pocket Cloud service. It is published for transparency, so anyone can read how Pocket works.

Pocket is **source-available, not open source**. The platform is controlled. Official updates and recovery go through the guided path, and extensions go through the approved [developer program](https://northwork.app/pocket/app/developer), not unrestricted flashing.

| Path | What it is |
| --- | --- |
| [`firmware/`](firmware/) | Device software for Pocket Classic (ESP32-S3, 3.97" e-paper, ESP-IDF) |
| [`companion/`](companion/) | The phone-only Companion app and the Pocket web pages |
| [`cloud/`](cloud/) | The Pocket Cloud API: sync, backup, device pairing and billing |
| [`packages/shared/`](packages/shared/) | Types shared between the packages |
| [`docs/`](docs/) | Deployment and billing setup notes |

Each package has its own README with setup and test instructions.

## For Pocket owners

1. Set up your Pocket Classic on the device itself. It's fully usable offline.
2. Install [Companion](https://northwork.app/pocket/app/get-companion) on your phone to pair it.
3. Optionally, add **Pocket Cloud** to keep Notes, Lists, Music and Reading in sync everywhere.

Updates arrive over the air from **Home → Update**. If you ever need an official firmware package for recovery, use the [`firmware-latest`](https://github.com/NorthworkTech/Pocket/releases/tag/firmware-latest) release, never a third-party image.

## Building from source

| Package | Requirements | Start here |
| --- | --- | --- |
| Firmware | ESP-IDF v5.2, CMake | [`firmware/README.md`](firmware/README.md). The host simulator in `firmware/host` runs the UI without hardware. |
| Companion | Node 20+ | [`companion/README.md`](companion/README.md) |
| Cloud | Node 20+ | [`cloud/README.md`](cloud/README.md) |

Every push to `main` is built and checked automatically: firmware host tests, cloud tests and the Companion build.

## Security

Please report security issues privately to **support@northwork.app**, not in a public issue. Include steps to reproduce, and we'll get back to you.

## License

Proprietary. All rights reserved © Northwork. The source is visible for transparency; it is not licensed for redistribution or modified builds.
