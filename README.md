# VisuGram

[Русский](README-RU.md) · [Download](https://github.com/absolute-christian/VisuGram/releases)

A fork of [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) with visual gifts, collectible phone numbers and usernames.

Visual changes are local by default. An optional shared server synchronizes them between VisuGram users. They do not change your Telegram account, real Stars balance or NFT ownership.

## Features

- Visual Stars switch in My Stars, with an infinity balance display.
- Public gift catalog with collectible models, patterns and backdrops.
- Local gifting, including gifts to yourself in Saved Messages.
- Profile gift collection with hiding, deletion and up to six pinned collectibles.
- Existing +888 collectible numbers and secondary NFT usernames with purchase information.
- Shared visual profiles and gifts through a configurable sync server.
- Collectible details with attributes, wear status, transfer and visual sale.
- English and Russian labels for the added controls.

## Installation

1. Download the Windows x64 ZIP from [Releases](https://github.com/absolute-christian/VisuGram/releases).
2. Extract it into a separate folder.
3. Run `VisuGram.exe`. Keep `TelegramForcePortable` beside it — this folder contains your account data.

Enable **My Stars → Visual Stars** to use local gifts. Visual numbers and usernames are configured in **Edit Profile**.

For shared profiles and collectible gifts, set **Edit Profile → Visual sync server** to `https://visugram-api-production.up.railway.app`. Each participant needs to connect once. See the [data policy draft](docs/privacy.md) before connecting.

The current release is an experimental Debug preview; runtime behavior has not yet been manually verified.

## Documentation

[Visual mode](docs/visual-mode.md) · [Cloud builds](docs/cloud-build.md) · [Sync server](docs/synchronization-plan.md)

## Credits

Based on [AyuGram](https://github.com/AyuGram/AyuGramDesktop) and [Telegram Desktop](https://github.com/telegramdesktop/tdesktop). Upstream licensing and attribution are retained: [LICENSE](LICENSE), [LEGAL](LEGAL).
