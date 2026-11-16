# Pocket — working notes for Claude

Calm e-ink device (Pocket Classic, ESP32-S3 + Waveshare 3.97" e-paper), phone-only
Companion PWA, and Pocket Cloud API. See README.md and each package README.

## Deploy policy (owner's standing instruction: "Always deploy")

- Finished, tested work goes to `main` — no need to ask first. Push to `main` runs
  `.github/workflows/deploy.yml`: builds firmware with ESP-IDF, republishes the
  `firmware-latest` release (devices pick it up via Home → Update / OTA), and deploys
  Companion to GitHub Pages.
- Before pushing: host tests green (`firmware/host`: `cmake -B build && cmake --build build
  && ctest --test-dir build`), `cloud`: `npm test`, `companion`: `npm run build`.
- After pushing, watch the Deploy run (esp. the `firmware-esp32` job — it is the only real
  device compile) and fix forward if it fails.

## Firmware conventions

- Every firmware release bumps all three: `fw_version` + `kConsumerVersion` in
  `components/pocket_ui/include/pocket/config.hpp`, and `kBuildId` (`POCKET-LIVE-vNNN-slug`)
  in `main/app_main.cpp`. OTA compares build IDs, so an unchanged `kBuildId` ships nothing.
- Commit subject: `Firmware X.Y.Z (vNNN): summary`.
- The panel is **1-bit**: `epd.cpp` thresholds G0/G1 → black, G2/G3 → white. Never rely on
  mid grays; use dithering (`Canvas::fill_tone`, `screensaver.cpp`). Preview what the device
  shows with `host/dump_gallery` + `tools/gallery.py` (1-bit by default).
- Draw screens with the UI kit (`pocket/ui.hpp`): `ui::header`, `ui::row`, `ui::button`,
  `ui::tabs`, `ui::empty_state`, `ui::icon`. Don't hand-roll rows or focus tiles.
- Fonts: `tools/gen_fonts.py` → `src/font_ui.inc` (DM Sans + Source Serif 4, 1bpp).
- OTA app slot is 0x1E0000 (~1.9 MB); keep an eye on image size. The partition table
  cannot change over OTA.
- `DeviceConfig` persistence is versioned (`config_store.cpp` `kVersion`); add fields at
  the tail behind a new version and keep older blobs loading.
- Partial-refresh dirty rects are tied to layout constants (`grayscale.hpp`, PIN slot
  geometry in `app.cpp`); change geometry and dirty marking together.

## Cloud

- `cloud/src` (TypeScript, tests) and the production Neon Function
  `cloud/neon-fn/scram-api.mjs` are separate implementations — API changes usually
  need both.

## Brand

- Subscription is always "Pocket Cloud" (never "Connect"). Companion is phone-only.
