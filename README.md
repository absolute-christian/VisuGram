<div align="center">

# ✦ VisuGram

### Your profile. Your visual style.

Local gifts, collectible usernames and an infinite Stars display.<br>
Built on **AyuGram Desktop** — with Telegram's familiar interface.

[![Windows x64](https://img.shields.io/badge/Windows-x64-202938?style=for-the-badge&logo=windows&logoColor=white)](https://github.com/absolute-christian/VisuGram/releases)
[![Preview](https://img.shields.io/badge/Channel-Debug%20Preview-8b5cf6?style=for-the-badge)](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5)
[![AyuGram](https://img.shields.io/badge/Based%20on-AyuGram-229ed9?style=for-the-badge&logo=telegram&logoColor=white)](https://github.com/AyuGram/AyuGramDesktop)
[![Languages](https://img.shields.io/badge/Visual%20UI-EN%20%2F%20RU-202938?style=for-the-badge)](README-RU.md)

**[Download for Windows](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5)** · **[Русский](README-RU.md)** · **[Feature guide](docs/visual-mode.md)**

[Features](#-features) · [Quick start](#-quick-start) · [Visual mode](#-visual-mode) · [Documentation](#-documentation) · [FAQ](#-faq)

</div>

---

> [!NOTE]
> **Visual customization is local to VisuGram.** It changes what this client displays. Actual Stars balances, gift ownership, Telegram phone numbers and usernames remain unchanged. Other people do not receive these local gifts or see these profile changes in their clients.

## ✨ Features

| Feature | What it adds |
| :--- | :--- |
| ⭐ **Visual Stars** | A switch in **My Stars** that displays **∞** and opens gift sending in visual mode. |
| 🎁 **Gift catalog** | Public gifts and collectible variants from Telegram's API, with their models, patterns, colors and backdrops. |
| 💌 **Local gifting** | A gift caption, anonymous sending and confetti; gifts sent to yourself appear locally in **Saved Messages**. |
| 💎 **Profile gifts** | A local gift collection with hiding, deleting and up to **six pinned collectibles** beside the avatar. |
| ☎️ **Visual phone number** | An editable display number in **Edit Profile**. |
| 🪪 **Collectible usernames** | Multiple local visual NFT usernames, with the first one used as the primary displayed username. |
| 🌐 **Two languages** | English and Russian labels for the added visual controls. |
| 🦋 **AyuGram foundation** | Visual features extend the existing client and retain its upstream features. |

## 🚀 Quick start

### 1 · Download

Get the **Windows x64 ZIP** from [Releases](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5).

### 2 · Extract and launch

Extract the complete archive into its own folder and launch **`VisuGram.exe`**. Keep the bundled **`TelegramForcePortable`** folder beside it.

```text
VisuGram/
├── VisuGram.exe
├── TelegramForcePortable/
├── README.txt
├── LICENSE
└── LEGAL
```

### 3 · Customize

Open **My Stars → Visual Stars** to enable visual gifting. Open **Edit Profile** to set your visual phone number and usernames.

> [!TIP]
> No compilation is needed to use the published ZIP. Your account data stays in the portable folder; keep that folder private and back it up before replacing your installation.

## 🎨 Visual mode

| Where | What to do |
| :--- | :--- |
| **My Stars** | Turn on **Visual Stars** to display an infinite balance. |
| **Send a Gift** | Choose a public gift or collectible variant, add a caption and send locally. |
| **Saved Messages** | Select yourself as the recipient to keep a local gift here. |
| **Profile → Gifts** | Open a local gift to pin, hide or delete it. |
| **Edit Profile** | Enter a visual number and your list of collectible usernames. |

The gift renderer uses Telegram's gift resources, including collectible models, backdrop colors and patterns. You can also load a public collectible using its **`t.me/nft/...`** link.

Turning off Visual Stars hides the local gift layer without deleting the saved collection. Clear a visual phone number or username list to return to the account's original display values.

## 📚 Documentation

| Guide | Contents |
| :--- | :--- |
| [Visual mode](docs/visual-mode.md) | Controls, local storage and the implementation map. |
| [Cloud builds](docs/cloud-build.md) | GitHub Actions setup and Windows preview packaging. |
| [Portable installation](docs/portable-readme.txt) | Launching the client and keeping account data. |
| [Release notes](docs/release-notes.md) | Preview scope and current limitations. |
| [Build history](https://github.com/absolute-christian/VisuGram/actions/workflows/windows-visual.yml) | Windows preview workflow runs and their logs. |

<details>
<summary><b>☁️ Build a preview on GitHub</b></summary>

Open **Actions → Windows visual preview → Run workflow**, select the **`dev`** branch and start the workflow. GitHub prepares dependencies, builds Windows x64 Debug and uploads a portable ZIP. Enable **Publish release** to publish a prerelease as well.

See [cloud-build.md](docs/cloud-build.md) for the full setup. The build runs on GitHub's runner, so your computer can be turned off after the workflow has started.

</details>

## ❓ FAQ

<details>
<summary><b>Do visual gifts give me real NFTs or Stars?</b></summary>

They provide a local visual collection. Real collectible ownership and the Stars balance are managed by Telegram and remain unchanged.

</details>

<details>
<summary><b>Can other people see my visual profile?</b></summary>

Visual gifts, numbers and usernames are displayed only in this VisuGram installation. They are not synchronized to other Telegram clients.

</details>

<details>
<summary><b>Where is the visual collection stored?</b></summary>

Each account has its own local JSON file under <code>tdata/ayu/visual/</code>. In the portable installation, account data lives inside <code>TelegramForcePortable</code>. Keep the complete portable folder when moving the client.

</details>

<details>
<summary><b>What is the current release status?</b></summary>

The [Windows x64 preview 5](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5) successfully compiled and was packaged. It is an experimental **Debug preview**. Login, gifting, animations and persistence have not yet been manually verified.

</details>

---

## 🤝 Credits & license

VisuGram extends [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop), which is based on [Telegram Desktop](https://github.com/telegramdesktop/tdesktop). Gift resources come from Telegram's public [gift API](https://core.telegram.org/api/gifts).

Upstream attribution and licensing are retained. See [LICENSE](LICENSE) and [LEGAL](LEGAL).

<div align="center">

**✦ VisuGram**<br>
<sub>A familiar client. A profile styled your way.</sub>

</div>
