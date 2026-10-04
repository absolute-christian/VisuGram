# VisuGram

[Русский](README-RU.md) · [Windows downloads](https://github.com/absolute-christian/VisuGram/releases) · [Build status](https://github.com/absolute-christian/VisuGram/actions)

VisuGram is an experimental fork of [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop), based on Telegram Desktop. It adds local visual profile customization on top of the existing client.

- Visual Stars switch with an infinity balance display.
- Public gift catalog, collectible models, patterns and backdrops from Telegram's API.
- Local gifting, including gifts to Saved Messages, native gift rendering and confetti.
- Local profile gifts, hiding/deleting and up to six pinned collectible gifts.
- Editable visual phone number and multiple visual NFT usernames.
- Russian and English labels for the new controls.

Visual gifts and profile values exist only in this client. They do not change actual ownership, Telegram account data or the real Stars balance.

## Run on Windows

Download the Windows x64 ZIP from this repository's Releases, extract it into its own folder and run `VisuGram.exe`. Keep the bundled `TelegramForcePortable` directory beside the executable. Account data is stored in this folder; do not upload or commit it.

A [Windows x64 Debug preview](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5) has successfully compiled and been packaged. Login, gifts, animations and persistence still need manual testing. [Feature guide](docs/visual-mode.md).

## Cloud build

Open Actions → Windows visual preview → Run workflow on the `dev` branch. The workflow prepares dependencies and builds Windows x64 Debug on GitHub, uploads the portable ZIP and can publish a prerelease. [Cloud build details](docs/cloud-build.md).

## Upstream and license

AyuGram's existing features and upstream attribution are retained. See the [AyuGram README](https://github.com/AyuGram/AyuGramDesktop), [Telegram Desktop](https://github.com/telegramdesktop/tdesktop), [LEGAL](LEGAL) and [LICENSE](LICENSE).
