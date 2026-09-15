# CrossInk with AO3 Library

This personal fork combines the current [CrossInk](https://github.com/uxjulia/CrossInk) reading experience with the AO3 library concepts pioneered by [AvesO3](https://github.com/SiliconAves/AvesO3). CrossInk remains the base and continues to own EPUB rendering, progress, reading statistics, bookmarks, clippings, themes, controls, file management, and networking lifecycle. The AvesO3-inspired layer adds AO3 detection, metadata browsing, fic status, indexing, and on-device updates without replacing those newer CrossInk systems.

CrossInk itself is based on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). Thanks to the CrossPoint, CrossInk, and AvesO3 contributors whose work made this integration possible.

### Supported Devices

- Xteink X3
- Xteink X4
- Xteink X4 Pro
- Seeed Studio Sticky

The prebuilt AO3 release binary is currently produced for the X3/X4 target. Sticky remains available as a source build target but is not covered by the AO3 hardware validation yet.

## Project Lineage

| Project | Role in this fork |
|---|---|
| [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) | Original open-source reader firmware and hardware foundation. |
| [CrossInk](https://github.com/uxjulia/CrossInk) | Base codebase: modern reader, typography, statistics, themes, bookmarks, clippings, sync, controls, and file management. |
| [AvesO3](https://github.com/SiliconAves/AvesO3) | Reference implementation and design inspiration for the AO3 library, indexing, fic statuses, pinning, and chapter updates. |
| [This fork](https://github.com/maddymichaela/CrossInkO3) | Selectively ports and adapts the AO3 features to current CrossInk while preserving CrossInk behavior. |

## What's Different From CrossPoint

This fork includes the broader CrossInk feature set described below, plus a dedicated AO3 subsystem. Compared with standard CrossPoint Reader, the major additions are:

- An AO3-style library browser with summaries, metadata squares, filters, sorting, status symbols, and up to 400 indexed works.
- Native AO3, Calibre/FanFicFare, and Kobo-converted `.kepub.epub` recognition.
- Direct AO3 update checks and chapter downloads from the reader.
- Five-state fic tracking in the AO3 Library and File Browser.
- Automatic and manual AO3 indexing, ignored folders, and configurable batches up to 50 EPUBs.
- Longfic Home pinning, AO3 series continuation, and AO3-aware finished-book archiving.
- CrossInk typography, themes, reader controls, bookmarks, clippings, reading statistics, nearby sync, and reader customization.

## AO3 Features

### AO3-Style Library

- Displays each indexed work using an AO3-inspired metadata square, title, author, word count, fandom, relationships, tags, and multi-line summary.
- Supports up to 400 indexed works using a compact disk index and page-sized metadata loading to limit RAM use.
- Offers two browsing modes:
  - **Automatic:** fandom and relationship values come from EPUB metadata.
  - **Folder Tree:** folders beneath the selected AO3 root act as the browsing hierarchy.
- Sorts by title, author, word count, date added, or series, in the appropriate ascending or descending order.
- Filters by fandom, relationship, and any combination of AO3 content ratings: General, Teen, Mature, Explicit, and Not Rated.
- Includes a **Hide Finished Fics** filter toggle.
- Returns from the normal CrossInk reader to the same AO3 Library position.
- Suggests later indexed works from the same AO3 series at the end of a book.

### EPUB Compatibility and Indexing

- Recognizes EPUBs downloaded directly from AO3.
- Reads AO3 work ID, update date, and completion status from Calibre/FanFicFare OPF metadata without requiring a modified metadata spine.
- Recognizes Kobo-converted `.kepub.epub` AO3 exports and handles their nested summary/tag markup.
- Cleans empty punctuation-only tags and decoded entities such as `&amp;` from displayed metadata.
- Indexes only the selected AO3 folder and supports multi-select **Ignored Folders**.
- Indexes in configurable batches of 10, 20, 30, 40, or 50 works, with cancellation and a Continue prompt between batches.
- Can begin auto-indexing whenever the AO3 Library opens.
- Supports refreshing metadata for already indexed works.

### AO3 Library Quick Start

1. Open **AO3 Library** from the Home screen.
2. Hold **Up** to open **Manage AO3 Library**.
3. Choose **AO3 Folder** and confirm that the selected path appears beneath the setting.
4. Optionally choose **Ignored Folders**, the index batch size, browsing mode, and auto-index behavior.
5. Select **Index New Books**. Indexed works then appear in the AO3 Library.

Within the library, short Up/Down presses move by page, short Left/Right presses move by work, and holding Left/Right repeatedly skips pages. Hold Up for library management, hold Down for sorting and filters, hold Confirm to change the selected fic's status, and press Back to return Home.

### Fic Status and Pinning

AO3 works use five display states, shown in both the AO3 Library and File Browser:

| Status | Meaning |
|---|---|
| Unread | The work has not been started. |
| Reading | Reading is in progress. |
| Waiting for Chapter | All currently downloaded chapters of a WIP have been read. |
| New Chapter Available | An AO3 update was found. |
| Finished | The downloaded work is marked complete and finished. |

- Statuses are derived from CrossInk progress and completion data where possible, with AO3-specific Waiting and Update Available state stored alongside the book cache.
- A status can also be changed manually from the File Browser, AO3 Library, or reader menu.
- Reaching the end of an unfinished AO3 work automatically assigns **Waiting for Chapter**.
- Longfics can be pinned to the Home screen so newly read one-shots do not push them out of view.

### On-Device Chapter Updates

- A WIP-specific end-of-book prompt can check AO3 for updates over Wi-Fi.
- **Check AO3 Updates** is also available from the reader menu.
- When a newer version exists, the current AO3 EPUB can be replaced on-device.
- Locked AO3 works are not currently supported.

Updating intentionally overwrites the local EPUB, but the replacement flow is safer than the original direct-overwrite approach: it downloads to a temporary file, validates that it is an EPUB for the expected AO3 work ID, keeps a backup, installs the new file, rebuilds AO3 metadata, preserves CrossInk user state, and restores the backup if installation fails.

### AO3-Aware Read Folder

- **Settings > System > Read Folder** selects one finished-book destination for ordinary EPUBs and AO3 fics; existing installs continue to default to `/Read`.
- The selected Read folder and all of its subfolders are excluded from end-of-book next-book and AO3 series recommendations. The device explains this before opening the folder picker.
- Finished AO3 works can be moved explicitly or through CrossInk's **Move Finished Books to Read Folder** setting.
- The path beneath the configured AO3 folder is mirrored under the selected Read folder, so `/AO3/Fandom/Series/book.epub` becomes `/Finished/AO3/Fandom/Series/book.epub` when `/Finished` is selected.
- **Restore Original Folder** recreates the original directory tree and returns the EPUB without overwriting an existing file.
- Progress, statistics, bookmarks, clippings, Home pinning/Recents data, resume state, AO3 status, metadata, and index order follow the moved file.
- Marking an archived AO3 fic unfinished through CrossInk's completion control restores it to its original folder.

## Improvements Over AvesO3

This is a selective adaptation rather than a wholesale merge of the older firmware fork. Notable improvements include:

- Current CrossInk EPUB rendering, status bar, page counts, time-left estimates, reader settings, themes, controls, bookmarks, clippings, and statistics remain authoritative.
- AO3 metadata parsing is lazy and indexing-driven, avoiding a full AO3 scrape whenever an ordinary EPUB is opened.
- The compact AO3 browser loads rich metadata only for the visible page instead of retaining every full record in RAM.
- Chapter downloads use temporary-file validation, work-ID matching, backup/rollback, and state-preserving cache rebuilds.
- Moving a fic rekeys its AO3 index record in place, preserving date-added order and avoiding a mandatory reindex.
- Original locations are kept in a separate archive registry, so cache cleanup does not destroy restore information.
- Nested AO3 folders are retained when archiving instead of flattening every finished fic into one directory.
- Multi-select rating filters, Hide Finished Fics, auto-index on open, ignored-folder multi-select, Kobo export handling, and series continuation are integrated into the current CrossInk UI.
- AO3 navigation uses current CrossInk activity/input handling, including normal Back behavior and restoration of the selected library row.

No AvesO3 reader engine or old CrossPoint shared files were copied wholesale. AO3 books remain normal EPUBs opened by the standard CrossInk reader.

## AO3 Fork Releases

| Version | Status | Highlights |
|---|---|---|
| `1.5.1-ao3.11.4` | Current | Restores AO3 series recommendations and removes redundant end-screen refreshes and retained UI state from next-book/Home transitions. |
| `1.5.1-ao3.11.3` | Previous stable | Adds a global selectable Read folder for normal and AO3 EPUBs and excludes its contents from end-of-book recommendations. |
| `1.5.1-ao3.11.2` | Previous stable | Speeds reader-to-Home returns, AO3 metadata refresh batches, end-of-book series lookup, and next-book handoff without changing CrossInk reading behavior or the AO3 index format. |
| `1.5.1-ao3.11.1` | Previous stable | Makes Settings > Check for Firmware Updates use this CrossInkO3 repository, recognize fork-style firmware asset names, and compare AO3 patch versions correctly. |
| `1.5.1-ao3.11` | Previous stable | Updates the base to CrossInk 1.5.1 while retaining the complete AO3 library, status, pinning, archive, series, and update workflow. Adds X4 Pro/X4 Classic support from upstream, the newer touch reader menu, visible Wi-Fi password entry, and the v1.5.1 KOReader Sync, EPUB, input, sleep, and memory fixes. |
| `1.5.0-ao3.10.3` | Superseded notes-only release | Records the transition from the 1.5.0 line to the upstream merge. Its tag references the last validated 1.5.0 source, but it has no distinct firmware asset; the completed compatibility work ships in `ao3.11`. |
| `1.5.0-ao3.10.2` | Previous stable | Speeds AO3 series continuation by reading the compact index instead of opening every cached EPUB sidecar, and releases the previous rendered section before opening the selected next work. |
| `1.5.0-ao3.10.1` | Previous hotfix | Keeps AO3 Sort & Filter rows evenly spaced and leaves the relationship guidance visible before a fandom is selected. |
| `1.5.0-ao3.10` | Previous feature release | Adds AO3-aware move-to-Read and restore behavior with original nested-folder preservation. |

Version 10 and 11 release files are retained in the `release/` folder. For an X3 or standard X4, flash the `x3-x4.bin` asset whose version matches the release you intend to install. `ao3.11.4` is the recommended build; the `ao3.10.3` Release page is a historical transition record and intentionally has no binary.

Devices running `ao3.11` or older must flash `ao3.11.1` manually once because those builds still contact the upstream CrossInk release feed. After that one-time upgrade, **Check for Firmware Updates** follows releases from this CrossInkO3 repository.

## CrossInk Highlights

The AO3 subsystem sits on top of the existing CrossInk feature set:

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: 12 pt, Margin: 15" /><br/>
      <em>Font: Bitter, Size: 12 pt, Margin: 15</em>
    </td>
    <td align="center">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats with custom front button mapping shown" /><br/>
      <em>Reading Stats with custom front button mapping shown</em>
    </td>
  </tr>
</table>

- New reader fonts: Lexend Deca and Bitter.
- Music notation and selected supplemental Unicode glyph support to be able to render Project Hail Mary accurately.
- Added a custom `Minimal` theme and sleep screen option for the minimalists out there.
- Added a custom `Dashboard` theme and sleep screen option for reading stats enthusiasts.
- Reader font sizes: 10 pt, 12 pt, 14 pt, and 16 pt.
- Added ~~strikethrough~~ support.
- Made <u>underlines</u> thicker for better visibility.
- Added support for `<hr>` section breaks.
- Added support for "redaction" style rendering.
- Added improved support for tables with simple markup.
- Added ability to add bookmarks.
- Added ability to remap front buttons that only applies in the reader.
- Added Focus Reading and Guide Dots as optional reader modes.
- Added Force Paragraph Indents for books that render as one giant wall of text.
- Added ability to pin a sleep image as a favorite. The favorited image will always be displayed when your sleep settings are set to `Custom` or `Cover + Custom` (when no cover is available).
- Added more in-reader control remapping options for side buttons, short power button clicks, and long-press menu actions, and more.
- Added ability to mark a book as finished from the in-book menu. A pop-up will also display once 99% of the book is reached. This status allows tracking of total books read.
- Added ability to move finished books to "Read" folder.
- In-book menu to quickly adjust reader options without having to exit the book.
- Reading stats: total books read, total reading time, number of sessions, pages turned, average session time, pages turned per minute. You can also set your reading stats as your sleep screen.
- All-time reading stats [syncing](./docs/reading-stats-sync.md) between two CrossInk devices.
- Reading [progress sync](./docs/nearby-position-sync.md) between two CrossInk devices.
- Added customizable Auto Page Turn Interval (anything between 5-120 seconds).
- Added ability to view Recent Books as a 3x3 grid view.
- To view a more detailed list for each version, visit this fork's [releases](https://github.com/maddymichaela/CrossInkO3/releases) page.

---

### Reader Fonts

The default fonts have been replaced with Lexend Deca and Bitter. These fonts have been chosen specifically to improve reading fluency and e-ink performance. These 'sturdier' typefaces feature uniform stroke weights and open geometries, allowing the X4/X3 to render crisp, high-contrast text with font-aliasing on while significantly reducing ghosting and artifacts.

- [Lexend Deca](https://fonts.google.com/specimen/Lexend+Deca) - A research-backed sans-serif typeface designed to improve reading fluency. Lexend was engineered based on the theory that reading issues are often a design problem (visual crowding) rather than a cognitive one.
- [Bitter](https://fonts.google.com/specimen/Bitter) - A "contemporary" slab serif typeface for text, it is specially designed for comfortably reading on digital screens. The consistent stroke weight of Bitter helps it render particularly well on e-ink devices. The medium weight has been chosen specifically for improved rendering on the X4/X3.

The UI now uses [Inter](https://fonts.google.com/specimen/Inter) as the display font which has improved readability at smaller sizes.

### Music and Supplemental Glyphs

- Built-in reader fonts include music notation, selected Cyrillic glyphs, and the Project Hail Mary CJK fallback ranges. Additional SD-card fonts retain emoji fallback support.

---

### Font Sizes

CrossInk includes 10 pt, 12 pt, 14 pt, and 16 pt built-in reader font sizes.

See [SD Card Fonts](./docs/sd-card-fonts.md) for installing additional font families and size ranges.

---

### Reader features

Reader Options, Focus Reading, Guide Dots, Force Paragraph Indents, reading stats, and finished-book behavior are documented in [Reader Features](./docs/reader-features.md).

### Custom button actions

CrossInk adds configurable button shortcuts.

See [Controls](./docs/controls.md) for the full action list and defaults.

---

## Tips for the best reading experience

CrossInk runs on an ESP32-C3 with limited RAM, so very large folders or complex EPUBs can be slower than they would be on a phone, tablet, or desktop app.

- Keep folders under about 200 files. For the smoothest browsing, aim for 50-100 files per folder.
- Having 1000+ books on the SD card is fine if they are split into smaller folders, such as by author, series, genre, or read/unread status.
- Avoid putting every book in the SD card root. The file browser has to scan and sort the current folder before it can show it.
- Text-first EPUBs are the best fit. Large image-heavy EPUBs, scanned books, comics, and omnibus files with thousands of sections may load slowly or fail under memory pressure.
- As a rough target, EPUBs under 20 MB tend to work the best. Files over 50 MB may still work, but they are more likely to be slow or memory-sensitive, especially if they contain many large images.
- If an EPUB is unusually slow, try [optimizing](./docs/webserver.md#epub-optimization) it with the built-in web optimizer (via File Transfer) before copying it to the SD card: remove unused high-resolution images, split very large omnibus files, and avoid embedding multiple full font families when possible.
- Use a reliable SD card and leave some free space. CrossInk stores settings, reading progress, cache files, stats, and generated book data on the card.

---

## Installation

The fastest way to install CrossInk is by using [Inky](https://inky.crossink.dev/#flash-tools), CrossInk's web companion app.

Download the X3/X4 `.bin` from [this fork's releases](https://github.com/maddymichaela/CrossInkO3/releases), then flash it with the web installer or command line. The current boot screen should report `1.5.1-ao3.11.4`, rather than `dev+main`.

See [Installation](./docs/installation.md) for step-by-step flashing and revert instructions.

---

## Guides & Documentation

Visit [https://www.crossink.dev](https://www.crossink.dev) for more user guides and additional documentation.

---

## Development quick start

CrossInk uses PlatformIO for building and flashing firmware. See [Getting Started](./docs/development/getting-started.md) for prerequisites, clone setup, and validation commands.

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

Connect your device to your computer via a USB cable. Before the first build, initialize the repository's submodules (including `freeink-sdk`):

```sh
git submodule update --init --recursive
```

Then flash the firmware using the correct environment for the device. The `default` environment is for the X3/X4 devices. ESP32-S3 devices have their own named environments.

```sh
pio run -e default --target upload
```

If PlatformIO reports `PackageException: Can not create a symbolic link for freeink-sdk/libs/hardware/BatteryMonitor, not a directory`, the `freeink-sdk` submodule is not initialized. Run the submodule command above and retry.

See [Testing and Debugging](./docs/development/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

---

## Notice on Contributions

This repository is a personal integration fork. General CrossInk questions belong in [CrossInk discussions](https://github.com/uxjulia/CrossInk/discussions), while major firmware features requiring upstream support should be directed to [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). For the original AO3-oriented firmware and its documentation, see [AvesO3](https://github.com/SiliconAves/AvesO3).
