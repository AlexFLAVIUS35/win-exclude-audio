# win-exclude-audio

OBS Studio plugin for **exclude-only application audio capture on Windows**.

It captures the system's Windows audio through process-loopback capture while excluding the executable names you add to the list. No virtual audio cable or external mixer is required.

## Usage

1. Add **Application Audio Output Capture** in OBS.
2. Add the `.exe` names you want excluded. Wildcards such as `Discord*.exe` are supported.
3. Disable OBS's built-in **Desktop Audio** source so the audio is not captured twice.

The source is intentionally exclude-only: there is no include mode or hotkey capture mode.

## Requirements

- OBS Studio 28 or later
- Windows 10 2004 or later / Windows 11

## Credits

Based on Ayrton09's modernized `win-capture-audio` fork and the original work by bozbez.

GPLv2.
# win-exclude-audio

OBS Studio plugin for **exclude-only application audio capture on Windows**.

It captures the system's Windows audio through process-loopback capture while excluding the executable names you add to the list. No virtual audio cable or external mixer is required.

## Usage

1. Add **Application Audio Output Capture** in OBS.
2. Add the `.exe` names you want excluded. Wildcards such as `Discord*.exe` are supported.
3. Disable OBS's built-in **Desktop Audio** source so the audio is not captured twice.

The source is intentionally exclude-only: there is no include mode or hotkey capture mode.

## Requirements

- OBS Studio 28 or later
- Windows 10 2004 or later / Windows 11

## Credits

Based on Ayrton09's modernized `win-capture-audio` fork and the original work by bozbez.

GPLv2.
