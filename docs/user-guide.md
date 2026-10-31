# Using duckmidi

## Open a song

Use **File > Load MIDI / MUS / project**, or drop a file onto the editor. Use **File > New MIDI** to start from scratch. Open **Project settings** to change the song's name, tempo, or source view; click **Apply** or press **Enter** when finished.

If you cannot hear anything, open **Edit > Preferences** and select an SF2/SF3 SoundFont or an external MIDI output. Production packages supply a default SoundFont and open a sample project on first launch.

## Play and navigate

| Shortcut | Action |
| --- | --- |
| Space | Play / pause |
| Shift + Space | Play from the beginning |
| Left / Right | Move back / forward one second |
| Shift + Left / Right | Move back / forward a quarter bar |
| Home / End | Go to the beginning / end |

Choose **Song** in the toolbar to hear the arrangement, or **Pattern** to hear the selected pattern. Click the ruler to choose a starting position; right-drag along it to select a loop. **Escape** clears the loop after dismissing any active selection or window.

Use the mixer to adjust volume and pan, mute a part, or listen to it alone with Solo. Colored ports appear side by side within their channel, each with independent controls. Click the volume number to type a value from 0 to 127; **Enter** applies it and **Escape** discards it. If a note gets stuck, **Shift-click Stop** to silence held notes and release pedals.

## Arrange and edit notes

1. Click **+ New** in the pattern browser, or choose an existing pattern.
2. Click a playlist row to place it, or drag its card into the arrangement.
3. Double-click a placement to open its notes in the piano roll.
4. Click the playlist row's instrument label to choose the channel's instrument. Choose **Drum kit** to make it the drum channel (General MIDI channel 10); on channel 10, the dropdown lists drum kits.

The piano roll's instrument dropdown changes its preview sound. **Use selected channel** copies the selected row's instrument into that preview.

Placements of the same pattern share its notes. To make a separate version, click its browser card and press **Ctrl + D**, or right-click it and choose **Duplicate**. Rename patterns from the same menu.

New patterns use the application color. Press **Alt + C** with a pattern card or placements selected, or choose **Change color** from their menu (Shift + right-click), to pick one of 16 colors. Notes use the application color, or their port's color.

The slice tool splits a placement into two linked placements of the same pattern, each showing its part. Slicing again, or resizing a slice's right edge, keeps each part's position in the pattern.

| Gesture | Action |
| --- | --- |
| Click empty piano-roll space | Add a note |
| Drag a note's body | Move it |
| Drag a note's edge | Change its length |
| Ctrl + drag | Select a group of notes |
| Right-click / right-drag | Erase notes |
| Delete | Remove the selection |
| Alt + move or resize | Bypass snapping |
| Shift + drag inside a note | Draw a pitch bend |
| Middle-drag | Pan the editor |

Select several playlist placements to move them together. Drag a placement's edge to change how long it plays; extending it does not repeat its notes automatically.

**Ctrl + C / X / V** copies, cuts, and pastes in the focused editor. **Ctrl + D** duplicates the selection; **Ctrl + Z / Y** undoes and redoes edits. **Ctrl + Q** quantizes selected notes to quarter-beat positions.

## Play and record with your keyboard

1. Open a note pattern and choose an instrument.
2. Press **F1** to enable the computer keyboard piano.
3. Play the lower row with **Z X C V B N M**, or the upper row with **Q W E R T Y U**. The keys between them play sharps/flats: **S D G H J** and **2 3 5 6 7**.
4. Use **Keypad 1 / 2** to lower or raise the keyboard octave.
5. Press **F5** to enable Record, then **Space** to start the take. Press Space to stop, and F5 to turn Record off.

| Toggle | Shortcut | Use |
| --- | --- | --- |
| Keyboard piano | F1 | Play notes with typing keys |
| Metronome | F2 | Hear the beat while playing |
| Wait | F3 | Start an armed recording when you play the first note |
| Overlap | F4 | Replace only existing notes that overlap newly recorded notes |

With Overlap off, a new take replaces the pattern's notes for the active color/port. Save first or use Undo if you want to try another take.

Turn keyboard piano off when you want its letter keys to operate editor shortcuts instead.

### Assign percussion keys

Choose percussion, open the pattern editor's top-left menu, and select **Percussion keyboard mapping**. Choose a drum, click **Listen for key**, and press a piano key. Click a drum to audition it.

- **Save for current project** applies the mapping to this project. Save the project file to keep it.
- **Save to preferences (global)** makes it the default for projects without their own mapping.
- **Use global mapping for this project** removes the project's override.

**Reset mappings** restores the default draft. Close the window to discard unsaved changes; Escape stops key listening before closing it.

## Add automation

### Change velocity or draw a controller effect

1. Open a note pattern and click **Events**. The pattern editor grows upward to make room, so the piano roll keeps its size.
2. Choose **Velocity**, **Pitch bend**, **Modulation**, or another controller from the dropdown. Hover the dropdown and use the mouse wheel to step through the types.
3. For velocity, drag the note stems up or down. For a controller, use the gestures below.
4. Drag points to adjust their time and value. Drag a section's curve handle to shape the transition.

| Gesture | Action |
| --- | --- |
| Click | Add a point at the cursor, connected to the nearest point |
| Click while points are selected | Deselect them without adding a point |
| Shift + click or Shift + drag | Add a single point, or draw a section |
| Right-click / right-drag | Erase points |
| Ctrl + drag | Select points |

Controller sections affect notes on the same channel and active color/port. The underlying value resumes when a section ends. For a sustained pedal, choose **Sustain** and drag along the line for the interval you want held. A bend drawn on a note moves with the note. Program change points show the instrument name while you place them.

Press **Shift + Keypad +** or **Shift + Keypad −** anywhere to step through automation patterns, creating one after the last, just as **Keypad + / −** steps through note patterns.

### Make a reusable LFO

1. Change the pattern browser's category to **Automation**, then click **+ New**.
2. Choose the controller in the Event Editor and switch **Point** to **LFO**.
3. Adjust **Center** for the resting value, **Speed** for the rate, and **Amount** for the depth. Tension, Skew, Width, and Invert change the waveform.
4. Place the automation pattern on the channel you want it to affect. Resize its playlist placement to set the effect's duration.

Drag LFO knobs vertically. Middle-click resets a knob; **Alt** bypasses snapping. Use **Point** mode for a hand-drawn effect instead. The controller's response depends on the chosen SoundFont or MIDI device.

## Give notes independent effects

Select notes in the piano roll and press **Alt + C**. Choose a color to give them a separate port, or the default swatch to remove the color.

Color buttons then appear beside the instrument dropdown across that channel's patterns. Choose a button before editing events to affect that color's notes. For example, give one sustained note a pitch bend while another note on the same channel stays steady.

Independent ports work in duckmidi playback and MIDI export. Other players may ignore them, and one external MIDI connection cannot keep their controllers separate. M64 export does not support colored ports.

To remove a port, Shift + click its color button and choose **Delete port**. Its notes return to the default port and its events are discarded.

MIDI files that use several ports open as one arrangement, with each source port on its own color. To merge them, open **Project settings** and choose **Combine original source ports** (only the file's ports) or **Combine ALL source ports** (also ports you colored). A warning lists channels that more than one port uses; Undo reverses the merge.

## Turn an imported song into patterns

Choose **Edit > Generate Patterns**, then **Generate**. Review the preview and click **Apply**, or close it to discard the result.

Generation uses the current arrangement, including your edits. Repeated phrases become linked patterns, while controllers become separate automation patterns. Existing curves and LFOs stay editable. **Ctrl + Z** reverses Apply.

## Save and export

**Ctrl + S** saves a `.duck` project. Use **Ctrl + Shift + S** to choose another name or location.

### MIDI

1. Press **Ctrl + E** and select **MIDI**.
2. Choose an **Editor metadata** option. Keep duckmidi metadata if you want to reopen the MIDI with its editor information; strip it for a file containing only musical MIDI data.
3. Click **Export MIDI** and choose a filename.

The file contains the arrangement as you hear it: muted channels are left out, and colored ports and other source sequences are included.

### Audio

1. Press **Ctrl + E** and select **Digital Audio**.
2. Choose WAV (PCM), MP3, AAC, Ogg Vorbis, Opus, or FLAC from **Codec**.
3. For WAV/FLAC, choose 16-bit or 24-bit audio. For lossy codecs, choose constant or variable bitrate, then set the bitrate or quality. FLAC also offers a compression level.
4. Click **Export** and choose a filename. The suggested name uses the project name, falling back to the source filename.

The active view is rendered using the selected SoundFont as 48 kHz stereo, with a two-second release tail. A progress bar appears at the bottom of Export while the worker renders. **Cancel audio export** stops it without replacing an existing output file. After rendering, **Open file location** opens its folder. External-device audio is not recorded.

### M64

Choose **M64** in Export's format dropdown, review the instrument mappings, then save the ZIP package or click **Export to Repository** to install it into your SM64 checkout. Add **M64 - Loop Start** / **M64 - Loop End** song events to repeat part of the song. See the [M64 guide](sm64-export.md) for loop points, mapping warnings and installation.

## Customize shortcuts

Open **Edit > Keyboard shortcuts**, choose an action, and enter its new binding. Shortcuts act on the focused editor; click the notes, events, or playlist before using an editing shortcut. Escape closes the active popup/window first.

Preferences and layout are saved beside the executable. For manual INI edits, close duckmidi first. Paths and shortcuts are quoted strings; action labels use underscores:

```ini
soundfont="C:\Music\my-font.sf2"
play_pause="Space"
color_selected_notes="Alt + C"
```

Shortcuts saved by an earlier version keep working; actions added since then use their default keys. Keep entries in their existing sections. Use `+` between modifiers, such as `"Shift + Alt + E"`; `-` and `=` refer to those physical keys. Percussion entries use drum names, such as `acoustic_snare="Z"`.
