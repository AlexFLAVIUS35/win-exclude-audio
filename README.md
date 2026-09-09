# win-exclude-audio

OBS Studio plugin for **exclude-only application audio capture on Windows**.

It captures Windows system audio through process-loopback capture while excluding the executable names you add to the list. No virtual audio cable or external mixer is required.

## Usage

1. In OBS, add **Application Audio Exclusion** as a source.
2. Add the `.exe` names you want excluded from system audio. Wildcards such as `Discord*.exe` are supported.
3. You can also pick a currently active application from the source's **Add from currently active applications** list.
4. Disable OBS's built-in **Desktop Audio** source so the audio is not captured twice.

The source is intentionally exclude-only. There is no include mode, mode selector, or hotkey capture mode.

## Requirements

- OBS Studio 28 or later
- Windows 10 2004 or later / Windows 11

## Building

```text
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config RelWithDebInfo
```

## Credits

Based on Ayrton09's modernized `win-capture-audio` fork and the original work by bozbez.

GPLv2.
