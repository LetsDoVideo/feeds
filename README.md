# Feeds

**Pull individual Zoom participant video feeds directly into OBS Studio as dedicated sources.**

https://github.com/user-attachments/assets/afe838b2-f571-47ee-a9d1-389b3247d3ed

Feeds is a native Windows plugin for OBS Studio that uses the Zoom Meeting SDK to give broadcasters clean, isolated video feeds from each Zoom participant. No screen capturing, no grid layouts, just raw high-quality video piped directly into your OBS scene.

---

## Requirements

- Windows 10 or later (64-bit)
- OBS Studio 32 or later
- A Zoom account (Pro or higher recommended)
- The Zoom meeting host must approve the livestream request when prompted

--- 

## Installation

1. Download the latest **Feeds.exe** from the [Releases](https://github.com/LetsDoVideo/feeds/releases) page
2. Run the installation file.
3. or
4. Download the latest **Feeds.zip** from the [Releases](https://github.com/LetsDoVideo/feeds/releases) page
5.Extract the ZIP file
6. Copy the extracted contents into your OBS Studio root folder
   - For standard OBS installs: `C:\Program Files\obs-studio\`
   - For portable OBS installs: your portable OBS root folder
7. Restart OBS Studio
8. The plugin is ready. No Zoom Marketplace activation required for the Free tier

---

## Quick Start

1. In OBS, click **+** in the Sources panel and add a **Zoom Participant** source
2. In the source Properties window, click **Not connected to Zoom. Click to Connect...**
3. Enter your Zoom meeting number or link when prompted
4. Enter the meeting password if required
5. The host will see a **"Request to livestream"** popup in Zoom — click **Allow**
6. Once connected, select a participant from the dropdown to display their feed
7. Add additional **Zoom Participant** sources for more feeds
8. Add a **Zoom Screenshare** source to capture active screenshares

> **Note:** When the plugin connects, the Zoom client will open on your desktop. This is expected and useful. Place it on a secondary monitor to keep an eye on all meeting participants, including those not currently in your OBS scene. Audio from the Zoom meeting will automatically appear in OBS as Desktop Audio, ready to use in your stream or recording.

---

## Screenshots

![OBS Source List](assets/source-list.png)

![Participant Properties](assets/properties.png)

---

## Source Types

| Source | Description |
|--------|-------------|
| **Zoom Participant** | Captures an individual participant's camera feed |
| **Zoom Screenshare** | Captures whatever is being shared on screen |

### Special Options
- **[Active Speaker]** - automatically follows whoever is currently talking

---

## Tier Limits

| Tier | Feeds | Resolution |
|------|-------|------------|
| Free | 1 | 720p |
| Basic | 3 | 1080p |
| Streamer | 5 | 1080p |
| Broadcaster | 10 | 1080p |

Upgrade your tier at the [Zoom App Marketplace](https://marketplace.zoom.us).

---

## Known Limitations

- **Windows only** in this release (macOS coming in a future version)
- The meeting host must click **Allow** when prompted with the livestream request. Without host approval the plugin cannot access video feeds

---

## Known Issues

**No current known issues. Please let us know if you experience any.**

---

## Troubleshooting

**Plugin fails to load**

- Disable Smart App Control in Windows (Future verions will allow the plugin to load with Smart App Control enabled).

**Participant list is empty after connecting**

- Click the **Refresh Participant List** button in the source Properties window.

**Black screen / 0x0 pixels on a source**

- Delete the source and re-add it after connecting to the meeting.

**The host saw a "Request to livestream" popup. Is that normal?**

- Yes. Click Allow. This is how the plugin accesses raw video feeds via the Zoom SDK.

**How do I update the plugin?**

- Download the latest release from the [Releases](https://github.com/LetsDoVideo/feeds/releases) page and run the install exe or use the zip to replace the existing files in your OBS folder.

---

## Support

- 📖 [Documentation](https://letsdovideo.com/feeds-documentation/)
- 🛠️ [Support Page](https://letsdovideo.com/feeds-support/)
- 💬 [Let's Do Video Discord](https://discord.com/invite/CXGwwKt)

---

## Legal

Feeds is an independent integration and is not officially endorsed by the OBS Project or Zoom Communications, Inc.
OBS Studio is a trademark of the OBS Project. Zoom is a trademark of Zoom Communications, Inc.

© 2026 Let's Do Video. Licensed under GPL-2.0.
