[![中文](https://img.shields.io/badge/中文-README-red?style=flat-square)](./README.md)

# AIMP GSF Plugin

> **Disclaimer**: I am totally new to programming. This project was built by AI based on my requirements, with multiple AIs collaborating (due to limited free points).<br>
> Descriptions may be incomplete or inaccurate. Sorry for the inconvenience.<br>
>
> **Caution**: Issues and Pull Requests are disabled as I cannot handle them. Thank you for your understanding.<br>
> 
> **How to use**: 1.Place aimp_gsf.dll under AIMP\Plugins\aimp_gsf folder.
2.Place zlib1.dll in the root directory of AIMP.<br>
> 
> **Project description**: In the early development, I used music from *Super Robot Taisen OG2* for debugging and by somehow hit a usable memory address, which misled me into thinking my approach was correct. This led to many failed attempts to extend support to other games. <br> 
> After refactoring the logic, the plugin now **only plays `.minigsf` music from certain games** (sometimes with noise/crash the player). See my personal test list below.<br>
> Trying to ask AI to fix， mainly depends on the luck...<br>
>
> **Developers are welcome to fork and fix it! Thanks!**

---

## Introduction

A GBA music format (`.minigsf`) decoder plugin for the AIMP player, based on the principles of foobar2000's `foo_input_gsf`.

---

## Demo video (Chinese)

[Bilibili](https://www.bilibili.com/video/BV1Q2Nu68Eqk)

---
## Features

- Parsing of `.minigsf` format (only partially working in practice)
- Reading PSF tag information (title, artist, album, duration, etc.)
- Stereo 16‑bit audio output (44.1 kHz)
- Integrated GBA Audio Processing Unit (APU) and ARM7TDMI emulator

---

## Known Limitations / Issues

- External `gsflib` library dependencies are not fully handled, which may affect compatibility.
- The seek backward/forward would stuck the player for a while.
- Currently only music from certain games is playable. My personal test results are as follows (**not a complete list** – I only selected a few of my favorite tracks, so it is possible that some tracks from the same game work while others do not):<br>
<details>
  <summary>Supported game music</summary>
  1. <i>Yggdra Union</i> – works<br>
  2. <i>The Legend of Zelda: The Minish Cap</i>– works<br>
  3. <i>Pokémon Sapphire</i>– works<br>
  4. <i>Black Matrix Zero</i> – works<br>
  5. <i>Riviera: The Promised Land</i>– works<br>
  6. <i>Super Robot Taisen OG2</i> – works 7.13 fixed<br>
  7. <i>Gyakuten Saiban</i> – works<br>
  8. <i>Lunar Legend</i>– works 7.14 fixed<br>
  9. <i>Oriental Blue: Ao no Tengai</i>– works<br>
  10. <i>Breath of Fire</i>– works<br>
  11. <i>Ninja Five-O</i>– works<br>
  12. <i>Advance Wars</i>– works<br>
  13. <i>Sonic Advance</i>– works<br>
  14. <i>Mario and Luigi - Superstar Saga</i>– works<br>
  15. <i>Final Fantasy V Advance</i>– works<br>
  16. <i>Mega Man Zero 2</i>– works<br>
  17. <i>Kirby and the Amazing Mirror</i>– works<br>
  18. <i>Super Circuit</i>– works<br>
  19. <i>Final Fantasy Tactics Advance</i>– works<br>
  20. <i>Tales of Phantasia</i>– works<br>
</details>
<details>
  <summary>Unsupported game music</summary>
  1. <i>Summon Night: Swordcraft Story</i> – no sound<br>
  2. <i>Summon Night: Swordcraft Story 3</i>– no sound<br> 
  3. <i>Castlevania - Aria of Sorrow</i>– <b>crash the player</b><br>
  4. <i>Mother  3</i>– no sound<br>
  5. <i>Donkey Kong Country 2</i> – no sound<br>
  6. <i>Donkey Kong Country 3</i> – no sound<br>
  7. <i>Golden Sun</i> – noise only<br>
  8. <i>Metroid Fusion</i> – no sound<br>
  9. <i>Iridion II</i> – no sound<br>
  10. <i>Mario vs Donkey Kong</i> – noise only<br>
  11. <i>Kingdom Hearts Chain of Memories</i> – partically work<br>
</details>
---

## Quick Start

### Build Environment

- Windows 10 / 11
- Visual Studio 2019 or 2022 (with "Desktop development with C++" workload)
- Windows SDK 10.0

### Build Steps

1. Open the project file `aimp_gsf.vcxproj` in Visual Studio.
2. Select configuration: `Release`, platform: `x86` (**mandatory**, because AIMP is a 32‑bit application).
3. Build the solution (`Ctrl+Shift+B` or via the "Build" menu).
4. Copy the compiled `aimp_gsf.dll` to the AIMP plugins directory (e.g., `C:\Program Files (x86)\AIMP\Plugins\aimp_gsf`).
5. Restart AIMP. If the plugin is not detected, manually enable it in the plugin management interface.

### Runtime Dependencies

- The system requires `zlib.dll` (loaded dynamically by the plugin). If missing, obtain it from [zlib.net](https://zlib.net/) and place it in a directory listed in the system `PATH` environment variable or in the AIMP installation folder.

---

## Project Structure
```txt
aimp_gsf_plugin/
├── src/ # Plugin source code
│ ├── Plugin.h/cpp # AIMP plugin main interface
│ ├── GsfDecoder.h/cpp # GSF audio decoder
│ ├── GsfFormat.h/cpp # GSF/PSF file format parsing
│ ├── GbaApu.h/cpp # GBA APU emulation
│ ├── GbaEmulator.h/cpp # GBA emulator (ARM7TDMI + peripherals)
│ ├── miniz.h/cpp # zlib decompression wrapper (loads zlib.dll at runtime)
│ ├── aimp_gsf.def # DLL export definitions
│ ├── dllmain.cpp # DLL entry point
│ └── DebugLog.h # Debug logging helpers
│
├── sdk/AIMP/ # AIMP SDK headers
├── aimp_gsf.slnx # Visual Studio solution file
├── aimp_gsf.vcxproj # Project file
│
├── aimp_gsf.dll # Pre‑compiled DLL (can be dropped into plugins folder for test)
├── GBA精选.zip # My favorite GBA music collection
├── gsf_plugin_src-v21-仅支持OG2.zip # The initial version (only supports OG2), with pre‑compiled DLL
├── README.md # Chinese version
├── README_EN.md # English version (translation support: DeepSeek)
└── LICENSE # License file
```

---

## Version History

### v3 (2026-07-14)
- fixed Lunar Legend play issue.

### v2 (2026-07-13)
- fixed the OG2 play issue.

### v1 (2026-07-12)
- Realised the original approach was flawed and refactored the emulator logic. In practice, only some game music works; many fix attempts failed.

### v47 (2026-07-11)
- Attempted to expand support for other `.minigsf` game music but failed. The accidentally hit address `08002DAC` misled the direction; all fixes during this period were useless.
- Mainly modified `GbaEmulator.cpp/.h`; other files nearly remained the same.

### v21 (2026-07-09)
- Fixed noise issues during repeate/song switching (root cause: static variable `s_m4aInjected` was not reset in `Reset()` – now fixed).
- Only supports OG2.

### v1 (2026-07-04)
- Initial version, ported from `foo_input_gsf` code to the AIMP. (I doubt it as I don't have the source code but only told AI to create project base on `foo_input_gsf`)

---

## Technical Details

- **Audio parameters**: 44.1 kHz / stereo / 16‑bit PCM
- **GSF format**: Belongs to the PSF family, designed for compressed storage of GBA audio drivers. miniGSF is a subset of GSF and typically relies on the external `gsflib` library for full functionality; this plugin's integration with that library is not yet complete.
- **Emulation core**: Includes an ARM7TDMI interpreter and a GBA APU mixer.

---

## Acknowledgements

- [kode54](https://www.foobar2000.org/components/view/foo_input_gsf) – author of the foobar2000 `foo_gsf` plugin
- [Artem Izmaylov](https://www.aimp.ru/) – AIMP player and SDK provider
- Neill Corlett – GSF format specification (public domain documentation)
- ChatGPT, Claude, DeepSeek, Manus, 秒哒, 语构 (in alphabetical order, no ranking implied)

---

## Contact

- For issues or copyright concerns, please contact Bilibili user: **西藏狗粮**

---

## License

The source code of this project is released under the **GNU General Public License v2** (consistent with `foo_input_gsf`).  
The GSF format specification and documentation are in the public domain.
