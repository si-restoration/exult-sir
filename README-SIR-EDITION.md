# Exult: Serpent Isle Restored Edition

A fork of [Exult](https://github.com/exult/exult), the Ultima VII engine, with a modern interface and other improvements. It plays Ultima VII: The Black Gate (with Forge of Virtue) and Ultima VII Part Two: Serpent Isle (with The Silver Seed) like stock Exult, and the [Serpent Isle Restored](https://github.com/si-restoration) mod.

This branch, `sir-edition`, is stock Exult plus the changes below. They are also offered to the Exult project as pull requests; this edition exists so players can use them now. It is not an official Exult release.

## What it adds

- **Conversations you can read.** With the Ultima or Modern style, talk opens a panel with both portraits, the speaker's name, larger text and clear answers. Page Up scrolls back through everything said.
- **Three looks.** Classic (exactly as before), Ultima (parchment and gold) or Modern, for the game menu, Options, the Journey Log, the cheat window and conversations. Options has a search box.
- **Containers as a list** (optional): a tidy list of what is inside, instead of a pile of sprites.
- **Key bindings editor** on the Input tab of Options.
- **Shaded light** (optional): light and shadow from torches, lamps and spells. Sun shadows follow the time of day, with daylight spilling into rooms through doors and windows, and lamps and fires glow in colour over the scene. The Light and Great Light spells and the Helm of Light give the Avatar a lamp-sized glow. A lamp under a roof no longer lights that roof. Weather lighting (overcast, fog, lightning, lit rain and snow) is off by default.
- **Troubleshooting** (needs cheats on): find a lost item anywhere you have been, browse NPCs and flags, and check a game for stuck quest states.
- **Fixes:** weather is saved with your game, play time is recorded in saves, and several crashes are fixed, including one in the coloured light layer when a scaler was in use.

## Getting started

You need your own copy of the games (the GOG "Ultima 7 Complete" works). No game files are included.

### Windows (64-bit)

1. Download `ExultSIR-Windows-x64.zip` from the [releases page](https://github.com/si-restoration/exult-sir/releases) and unzip it somewhere you can write to, such as Documents (not Program Files).
2. Copy your Black Gate files into the `blackgate` folder inside it, and your Serpent Isle files into `serpentisle`.
3. Run `Exult.exe`.

This build is portable: its settings (`exult.cfg`) and saves stay in its own folder, so a stock Exult on the same PC is not affected. Windows SmartScreen may warn that the app is unrecognised: choose "More info", then "Run anyway".

### macOS (Apple Silicon)

1. Download `ExultSIR-macOS-AppleSilicon.zip` from the [releases page](https://github.com/si-restoration/exult-sir/releases), unzip it, and drag **Exult SIR Edition** to Applications.
2. Copy your games to the shared Exult folder (note: the top-level `/Library`, not the one in your home folder):
   - Black Gate: `/Library/Application Support/Exult/blackgate`
   - Serpent Isle: `/Library/Application Support/Exult/serpentisle`
3. Open the app. The first time, macOS says it cannot check the app for malicious software (it is not notarised by Apple). Open **System Settings → Privacy & Security**, scroll down and click **Open Anyway**, then confirm.

The app shares its settings (`~/Library/Preferences/exult.cfg`) and saves with any stock Exult on the same Mac. Stock Exult ignores the edition's extra settings. Intel Macs are not built yet.

### Linux, Steam Deck, iPad

Not built yet. The Serpent Isle Restored mod works on stock Exult 1.12.1.

### Sound and music

- **Sound effects** work out of the box: the download includes Exult's MIDI sound effects (`midisfx.flx`).
- **Better sound effects and the digital soundtrack** (recordings of the original music) come from Exult's audio packs on the [Exult download page](https://exult.sourceforge.io/download.html). Put the `.flx` sound files and the `music` folder in the `data` folder beside `Exult.exe` (Windows) or in `/Library/Application Support/Exult/data` (Mac), then turn on Sound effects and Digital music in Audio options.
- For MIDI music, MT-32 emulation is the most faithful (Ultima VII was written for the Roland MT-32) but needs MT-32 ROM files you supply yourself; FluidSynth with a good General MIDI soundfont is the next best.

### In the game

- Press **Esc** for the game menu. The line **Menu style: Classic** cycles to Ultima and Modern; conversations follow the menu style.
- Lighting, containers as a list and key bindings are in **Options** (Ultima or Modern style). The Display tab has the shaded light rows (sun, roof cover, coloured light, weather). The UI tab has **Conversation size** and **Conversation text**.

### Mods

Mods go in a `mods` folder inside the game's folder, for example `serpentisle/mods` (Windows, beside Exult.exe) or `/Library/Application Support/Exult/serpentisle/mods` (macOS).

## Source and licence

GPL version 2 or later, like Exult. Each release names the commit it was built from. Exult is by the Exult team; see AUTHORS.
