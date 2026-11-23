# REAPER Import and Export Plugin for Adobe Audition Sessions

A native C++ plugin to handle the importing of Audition .sesx files as Reaper projects, and the exporting of Reaper projects as Audition .sesx files.

Import and export work on a shared project model provided by the C++ library [ReaConvert](https://github.com/atmosfar/ReaConvert). This plugin supersedes the import-only [REAPER Import Plugin for Adobe Audition Sessions](https://github.com/atmosfar/reaper_sesx_import_plugin) and includes all of its features.

## Usage

Import a session through the regular Reaper media insertion path: `File` / `Open project`, `Insert` / `Media file`, or drag and drop onto the arrange view.

Export runs as an action, `SESX Export: Export current project`. Add it to a menu entry or a keyboard shortcut in the Actions list.

## Supported features

### Import (.sesx to Reaper)

Project:
- Sample rate
- Tempo and time signature
- Timecode frame rate and drop frame
- Markers / Regions

Master:
- Volume
- Mute / Solo / Sum to mono
- FX chain
- Volume envelope
- Pan envelope

Track:
- Name
- Colour
- Mute / Volume / Pan
- Solo / Record / Monitor
- Show in arrange
- Automation mode
- Volume envelope
- Pan envelope
- Mute envelope
- FX chain
- FX parameter envelopes

Clip:
- File path and source format
- Position and length
- Source offset
- Crossfading
- Fades
- Volume / Pan / Gain
- Loop
- Volume envelope
- Pan envelope
- Take FX chain
- Take FX parameter envelopes

Clip groups:
- Group membership
- Group colour

Routing:
- Bus output routing and sends
- Main send on / off
- Send Volume / Pan
- Send Volume envelope
- Send Pan envelope
- Send Mute envelope

### Export (Reaper to .sesx)

Project:
- Sample rate
- Duration
- Tempo and time signature
- Markers / Regions

Master:
- Name
- Volume
- Mute / Solo / Sum to mono
- Automation mode
- FX chain
- FX parameter envelopes
- Volume envelope
- Pan envelope

Track:
- Name
- Colour
- Mute / Volume / Pan
- Solo / Record
- Show in arrange
- Automation mode
- Automation lane open state
- FX chain
- FX parameter envelopes
- Volume envelope
- Pan envelope
- Mute envelope

Clip:
- File path and media handler
- Position and length
- Source offset
- Crossfading
- Fades
- Volume / Pan / Gain
- Mute
- Loop
- Volume envelope
- Pan envelope
- Mute envelope
- Take FX chain
- Take FX parameter envelopes

Clip groups:
- Group membership
- Group colour

Routing:
- Bus output routing and sends
- Main send on / off
- Send Volume / Pan
- Send pre-fader / post-fader
- Send Volume envelope
- Send Pan envelope
- Send Mute envelope

## Plugin formats

Handled in both directions, with plugin identification and preset chunk data:
- VST2
- VST3
- Audio Unit

Enabled / bypassed state is carried on export.

## Installation

Copy the built library to the Reaper UserPlugins folder:

### Mac:
- `reaper_sesx_convert-arm64.dylib` or `reaper_sesx_convert-x86_64.dylib` to `~/Library/Application Support/REAPER/UserPlugins`

### Windows:
- `reaper_sesx_convert-x64.dll` to `%APPDATA%\REAPER\UserPlugins`

### Linux:
- `reaper_sesx_convert-x86_64.so` to `~/.config/REAPER/UserPlugins`

## Building

Fetch the submodules first. Both build targets depend on them.
```
git submodule update --init --recursive
```

The compiled library binary is automatically copied to the plugin path defined in `CMakeLists.txt`.

### Mac:
```
mkdir build
cmake --build build
```

### Windows:

You will need Visual Studio 2022 or similar installed for MSVC compilation, as MINGW64 is not supported by the Reaper SDK.
```
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

### Linux:
```
mkdir build
cd build
cmake ..
cmake --build .
```

## Acknowledgements

This plugin builds on top of the "Hello World" example from @nukethebees. Read their blogpost [here](https://nukethebees.github.io/programming/cpp/reaper/2025-08-02/reaper-hello-world.html).

The C++ library [pugixml](https://github.com/zeux/pugixml) is used for parsing and writing the XML in the Audition session files.

The REAPER side of the conversion, the project model and the SDK function loading are provided by [ReaConvert](https://github.com/atmosfar/ReaConvert), which uses [WDL](https://github.com/justinfrankel/WDL) and the [REAPER SDK](https://github.com/justinfrankel/reaper-sdk).
