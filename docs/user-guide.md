# Using duckmidi

This guide walks through the editor from your first note to a finished export. Each section stands on its own, so skip ahead if you want.

## A quick tour

![The duckmidi editor with the sample project open](images/overview.png)

The window has five areas:

- **Toolbar** (top): play, stop, record, the song position and tempo, and toggles for the metronome and other recording options.
- **Patterns** (top left): the building blocks of your song. Each card is a pattern of notes or events.
- **Arrangement** (top right): the playlist, where you place patterns on channel rows to build the song.
- **Mixer** (bottom left): volume, pan, mute and solo for each channel.
- **Piano roll** (bottom right): the pattern editor / piano roll, where you draw and edit notes.

Shortcuts act on the area you last clicked, so click the piano roll before pressing **Ctrl + A**, for example.

## Start a song

![The welcome screen](images/welcome.png)

Click **New MIDI** to start from scratch, or **Open MIDI / project** to open a file. You can also drop a `.mid`, `.mus` or `.duck` file onto the window, or use the **File** menu later.

A new project starts with two channels ready to go: 
- Channel 1 shows MIDI Instrument #0, 'Acoustic Grand Piano'
- Channel 10 shows the 'Percussion' channel, a MIDI 1.0 convention

Imported MIDI files can play drums on other channels too, when they set them up as Roland GS rhythm parts or select XG or GM2 drum banks. Those channels also show as **Percussion**, use the drum keyboard mapping, and export as drums.

Open **Project settings** (**/**) to change the song's name, tempo or time signature, then click **Apply** or press **Enter**.

If you can't hear anything, open **Edit > Preferences** (**P**) and choose an SF2/SF3 SoundFont or an external MIDI output. 

Full packages come with a SoundFont and a sample song which opens when you first start *duckmidi*.

## Listen and move around

| Shortcut | Action |
| --- | --- |
| Space | Play / pause |
| Shift + Space | Play from the beginning |
| Left / Right | Move back / forward one second |
| Shift + Left / Right | Move back / forward a quarter bar |
| Home / End | Go to the beginning / end |

Choose **Song** in the toolbar to hear the whole arrangement, or **Pattern** to hear just the pattern you're editing.

Click the ruler above a grid to start playback from that spot. To repeat a section while you work on it, right-drag along the ruler; the highlighted part loops until you press **Escape**. This works in the playlist, the piano roll and the events editors, even past the last note of a short pattern.

Zoom with **Ctrl + mouse wheel**, or the wheel over the ruler. Grids show bars as the lightest lines, beats a shade darker and quarter beats darkest; the playlist adds quarter beats once you zoom in far enough.

### Mix your channels

Use the mixer to set each channel's volume and pan. 
- **M** mutes the selected playlist row
- **S** solos it. 

Click a volume number to type a value from 0 to 127; **Enter** applies it and **Escape** cancels.

Each channel shows one strip per note color (port) it uses, numbered above the strip. The default route, **0**, appears only when the channel has notes, events or mixer settings on it, or when the channel is still empty.

If a note ever gets stuck, **Shift-click on Stop** to silence everything.

## Build a song from patterns

![Pattern cards](images/patterns.png)

1. Click **+ New** in the pattern section, or pick an existing card.
2. Drag its card into a playlist row to place it in the arrangement.
3. Double-click a placement to open its notes in the piano roll.

In the current pattern category, **Keypad +** moves from an edited card to the next edited card, or creates an empty card if there is no next one. Keep pressing **Keypad +** on an empty card to create more. **Keypad −** removes the newest empty card, or moves to the previous edited card when there are no empty cards. Cards with notes, automation, or a playlist placement count as edited. New cards reuse the lowest available number and appear before higher-numbered cards.

Every placement of a pattern shares the same notes, so a change to one changes them all. To make a separate version, click the card and press **Ctrl + D**, or right-click it and choose **Duplicate**. The same menu renames, recolors (**Alt + C**) and deletes patterns. With the pattern section focused, **Ctrl + C** and **Ctrl + V** copy a pattern and paste it as a new one.

**Rearrange the cards** by dragging one within the pattern section. The card follows your mouse, the others close up behind it, and a space opens where it will land. Drop it there to move it. Placements in the song are not affected.

Select several placements to move them together. **Shift + drag** a placement to drag a linked copy of it (or of all selected placements) to another spot; the original stays put. Drag a placement's edge to change how long it plays. The slice tool splits a placement into two linked parts of the same pattern.

The copy drags can be changed in **Keyboard Shortcuts** under **Mouse**: click the gesture, then press the mouse button you want while holding any of Ctrl, Shift or Alt. Left and right drags need one of those keys.

### Edit song events

Click an empty bar in the playlist's **Events** row to open **Song event** in **Add** mode. Choose a type and set its beat within the bar. Click a colored bar to open **Edit** mode, then select an event on the left to change its values or delete it. Use **Mode** to switch between adding and editing within the same bar.

The Events row groups tempo, meter, key signature, track name, text, SMPTE offset, markers, loop points and instrument changes. Open **MIDI Events** for the full list of MIDI messages.

### Choose instruments

Click **+ New** beside **Arrangement** in the playlist to add a channel. Choose a free channel number, select **Melody** or **Drums**, and pick an instrument or drum kit.

![The Instrument window](images/instrument.png)

Press **Up** or **Down** to step through instruments or kits. Click **Apply** or press **Enter** to save the choice. **Cancel** or **Escape** discards the draft.

Right-click a playlist row label, or press **I** with the playlist or piano roll focused, to edit an existing channel. **Remove channel** clears the row and its events. If the row contains placements, confirm to remove them from the arrangement. Their source patterns stay in the browser.

The piano roll's own instrument dropdown only changes what you hear while editing. **Use selected channel** copies the selected row's instrument into it.

## Use your own samples

duckmidi can play your own recordings in place of SoundFont instruments. Import audio in **Edit > Samples**, shape each sample, save the result as a named sample set, then assign it to a channel. The same samples can go into M64 and OoT exports (see [Custom game samples](#custom-game-samples)).

![The Samples window with two imported samples](images/samples.png)

### Import audio

Open **Edit > Samples**. Use **Choose files** for one or several audio files, or **Choose folder** for the audio files directly inside a directory. You can also drop files and folders over the Samples window. WAV, MP3, AAC in M4A, Ogg Vorbis, Opus, FLAC and PCM AIFF are supported. Failed files stay in the list with an error while successful imports remain editable.

Folder imports skip files already in the draft; explicit file imports can add the same file again. Right-click a sample entry and choose **Open file location** to open its folder, or **Delete** and confirm to remove it from the draft. Resize the window vertically to give both panels more room.

### Set the pitch and type

Select a file on the left and name it on the right. Choose **Melody** for a pitched preset, or **Drums** and a trigger key for a drum sample. Each melodic file becomes a selectable preset. Drum files form one kit, with a separate trigger key for each file.

The initial root uses an unambiguous filename note first, then embedded root metadata, then a confident detected pitch. MIDI 60 is C4. A conflict warning means these clues disagree; check the root by ear and edit it when needed. **Offset** corrects the source's measured pitch in cents. **Coarse** and **Fine** transpose playback. Middle-click a tuning knob to reset it. **Reset to import values** restores the name, root, loop and detected settings from the import.

### Shape the sound

Drag the hollow envelope handles horizontally to change delay, attack, hold, decay and release. **Sustain level** controls the held volume. The numeric fields give exact times in seconds.

Saved forward WAV loops are imported automatically. Choose **No loop**, **Continuous** or **Until release** below the waveform, and drag either loop edge to set the range. Nearby matching zero crossings are selected automatically; hold **Alt** while dragging to use the exact position. **Continuous** keeps looping through the release. **Until release** leaves the loop when the note is released and plays the remaining tail.

Hold **Shift** and drag the waveform to move the playback start or end (blue lines), with the same zero-crossing snap. Keep an active loop inside the playback range. Unlooped drums play once even when their key is released.

Open **Gain, pan and ranges**, **Filter**, or **Vibrato and effects** for optional controls. The filter envelope uses the volume envelope's stage times and sustain level. Default pitch tracking is one semitone per key for Melody and zero for Drums; enable **Custom pitch tracking** to change it.

Use **Cut** to stop earlier samples with the matching **Cut by** number in the same preset and MIDI channel. Zero disables either side. For example, give an open hi-hat **Cut by** 1 and a closed hi-hat **Cut** 1 so only the closed hat cuts the open hat.

### Listen while you edit

With Samples focused, **Space** auditions once and **Shift + Space** toggles sustained audition. A looping one-shot audition releases after one second; sustained audition repeats an unlooped sample. Edits to loops, envelope handles or numbers update the playing audition, and any song voices using the sample, immediately. Changing the selection or closing Samples stops the audition.

While Samples has focus, **Ctrl + Z** undoes sample edits, imports and deletions; **Ctrl + Y** or **Ctrl + Shift + Z** redoes them. The **Undo sample change** and **Redo sample change** buttons do the same. A drag is one undo step. History lasts until duckmidi exits, including after saving or switching sets.

### Save a sample set

Enter a name beside the set dropdown and click **Apply** or press **Enter** to save the draft under that name. **Save** writes later edits to the selected set. Saving replaces the set's previous state and updates the open project's assignments right away.

The dropdown at the top switches between saved sets and starts with the most recently modified one; **New set** opens an empty draft. Each set keeps its unsaved draft in memory while you switch.

Each set is a folder in the sample library, the `samples` folder beside your preferences (see [Customize shortcuts](#customize-shortcuts)). Every sample is saved as a WAV named after it, with an SFZ file of the same name for other samplers, and `manifest.json` holds the set's settings:

```text
samples/
  My drums/
    manifest.json
    Kick.wav
    Kick.sfz
    Snare.wav
    Snare.sfz
```

Set and sample names must be valid file names, and sample names must be unique within a set. SFZ players can interpret envelopes and effects differently. The [sample manifest reference](sample-manifest.md) describes the files.

### Play a sample in your song

Open the channel's **Instrument** window. Keep the ordinary MIDI instrument selected, choose the sample set under **Source**, then choose its **Preset**. **Original MIDI bank** identifies the bank being replaced. **Apply** saves the assignment. While the song plays, selecting an instrument or source previews it before **Apply**; **Cancel** restores the previous sound. Selecting a sample source does not create a Program Change. The playlist shades sample-assigned channel labels and shows the original sample filename below the MIDI instrument.

For channels with colored note ports, **Sample route** selects the scope of the assignment. Port 0 supplies the channel default. Other ports can choose their own preset, **SoundFont**, or **Inherit channel source**. Use **Preview sample preset** and the keyboard to audition a draft source before applying it.

In a Program Change's editor, choose **Source** and **Preset**, then click **Apply source** to replace the sound of that event's target patch. The event's bank, program and route are shown beside the source controls. Use the event's own **Apply** action to save changes to its MIDI message.

A later Program Change to an unreplaced patch plays the selected SoundFont, and changing back uses the sample again. If a saved set or preset is unavailable, duckmidi names it and plays the original patch while keeping the assignment. Restore the set and reopen the project, or select an available preset.

Sample assignments are saved in projects and in MIDI editor metadata. Audio exports render the assigned samples. Standard MIDI files and external MIDI output keep their original bank, program and note messages. Keep the sample library with your projects when moving to another computer.

## Draw and edit notes

![The piano roll](images/piano-roll.png)

| Gesture | Action |
| --- | --- |
| Click empty space | Add a note |
| Drag a note | Move it |
| Drag a note's edge | Change its length |
| Ctrl + drag | Select a group of notes |
| Right-click / right-drag | Erase notes |
| Alt + move or resize | Ignore snapping |
| Shift + drag inside a note | Draw a pitch bend |
| Middle-drag a note | Drag a copy of it, or of all selected notes; pitch bends stay with the originals |
| Middle-drag empty space | Pan around |

**Ctrl + C / X / V** copies, cuts and pastes in the focused area. **Ctrl + D** duplicates the selection, **Delete** removes it, and **Ctrl + Z / Y** undoes and redoes. **Ctrl + Q** snaps the selected notes to quarter beats.

## Play and record with your keyboard

Your computer keyboard can play notes like a piano. The highlighted keys below are the piano keys, labelled with the note each one plays at the starting octave.

![Keyboard piano keys](images/keyboard-piano.svg)

1. Open a note pattern and choose an instrument.
2. Press **F1** to turn on the keyboard piano.
3. Play the bottom row (**Z** to **/**) for the lower octave and the top row (**Q** to **]**) for the upper octave. The keys just above each row play the sharps.
4. Press **Keypad 1** or **Keypad 2** to move down or up an octave.
5. Press **F5** to arm recording, then **Space** to start. Press **Space** again to stop.

During song playback, notes land where the playhead is when the playlist is focused and its playhead is on a placement of the pattern. Otherwise a take starts at the pattern's beginning, keeping the beat within the bar where you started playing. In pattern playback, notes land at the pattern's own position.

When a take ends, the piano roll is focused, so **Ctrl + A** and **Ctrl + Q** work on your new notes straight away.

| Toggle | Shortcut | Use |
| --- | --- | --- |
| Keyboard piano | F1 | Play notes with typing keys |
| Metronome | F2 | Hear the beat while playing |
| Wait | F3 | Start recording when you play the first note |
| Overlap | F4 | Replace only the existing notes your new notes overlap |

With Overlap off, a new take replaces the pattern's notes for the active color. Save first, or use Undo, if you want to try another take.

Turn the keyboard piano off when you want its letter keys to work as editor shortcuts again.

### Map drum sounds to keys

On the drum channel, each piano key can play any drum you like. Select a drum pattern or the drum channel, open the piano roll's top-left menu, and choose **Percussion keyboard mapping**.

![The percussion keyboard mapping window](images/percussion-mapping.png)

1. Click a drum in the list to hear it.
2. Click **Listen for key**, then press the piano key you want for it.
3. Play the mapped keys to try your kit.
4. Save the mapping:
   - **Save for current project** keeps it in this project. Save the project file to keep it.
   - **Save to preferences (global)** makes it the default for every project without its own mapping.
   - **Enter** saves to whichever of the two this project currently uses, and closes the window.

**Use global mapping for this project** removes the project's own mapping, and **Reset mappings** clears the draft. Closing the window, or opening another project, discards unsaved changes. **Escape** stops listening for a key before it closes the window.

## Add expression with events

Events change how notes sound over time: velocity, pitch bend, modulation, volume, pan and other controllers.

![The events section under the piano roll](images/events.png)

1. Open a note pattern and click **Events**, or press **Shift + E**.
2. Choose **Velocity**, **Pitch bend**, **Modulation** or another controller from the dropdown. Hover any dropdown and scroll the mouse wheel to step through its choices.
3. For velocity, drag the note stems up or down. For controllers, use the gestures below.

| Gesture | Action |
| --- | --- |
| Click | Add a single point |
| Drag | Draw a line from where you pressed to where you release |
| Shift + click | Add a point connected to the nearest point |
| Shift + drag | Connect the start to the nearest point, then draw the line from there to where you release |
| Alt + click or drag | Place without snapping to the grid; the line preview snaps unless Alt is held |
| Drag a point | Move it |
| Drag a line's hollow handle | Bend the line into a curve |
| Right-click / right-drag | Erase points |
| Shift + right-click a point | Change its value or curve, or delete it |
| Ctrl + drag | Select points |

A short wobble while clicking still counts as a click, so you won't draw a line by accident. **Program change** is always placed as a single point, since an instrument choice has no in-between values.

**Curves between points.** Shift + right-click a point and hover **Curve**:

- **Line** moves smoothly to the next point.
- **Hold** keeps this point's value until the next point, then jumps to it. In the picture above, the flat step is a held point.

Event changes apply to notes on the same channel and color. When a line ends, the value returns to what it was before. For a sustain pedal, choose **Sustain** and drag along the line for as long as you want it held. A pitch bend drawn on a note moves with the note.

Add **Shift** to **Keypad +** or **Keypad −** to use the same shortcuts in the **Automation** category from anywhere in the editor. Automation cards with points, an LFO, or a playlist placement count as edited.

### Make a reusable LFO

An LFO is a repeating wave, such as a vibrato or a slow filter sweep, that you can place like any other pattern.

1. Switch the pattern section's dropdown to **Automation** and click **+ New**. With the piano roll hidden, **Shift + E** opens the Event Editor on an automation pattern, creating one if needed.
2. Choose a controller in the Event Editor and switch **Point** to **LFO**.
3. Set **Center** (the resting value), **Speed** (the rate) and **Amount** (the depth). **Tension**, **Skew**, **Width** and **Invert** shape the wave.
4. Place the automation pattern on the channel it should affect, and resize the placement to set how long it lasts.

Drag the knobs up and down. Middle-click a knob to reset it; hold **Alt** to ignore snapping. How a controller sounds depends on your SoundFont or MIDI device.

## Give notes their own effects

Normally every note on a channel shares the same events. Colors let some notes have their own.

1. Select notes in the piano roll and press **Alt + C**.
2. Pick a color. The notes move to their own port; the default swatch moves them back.
3. Color buttons appear beside the instrument dropdown. Choose one before editing events to affect only those notes.

For example, bend one long note while the rest of the chord stays put. To remove a color, Shift-click its button and choose **Delete port**; its notes return to the default and its events are removed.

Colored notes play independently in duckmidi and in exported MIDI files, although other players may ignore them. M64 export moves them to unused channels, or merges them with a warning.

MIDI files that use several ports open with each port in its own color. To merge them, open **Project settings** and choose **Combine original source ports** or **Combine ALL source ports**. Undo reverses the merge.

## Turn an imported song into patterns

Choose **Edit > Generate Patterns**, then **Generate**. Look over the preview and click **Apply**, or close it to keep the song as it was. Repeated phrases become linked patterns and controllers become automation patterns. **Ctrl + Z** undoes Apply.

## Save and export

**Ctrl + S** saves a `.duck` project, and **Ctrl + Shift + S** saves it under a new name. Projects from earlier duckmidi versions open normally and are saved in the current format.

![The Export window](images/export.png)

### MIDI

1. Press **Ctrl + E** and select **MIDI**.
2. Choose an **Editor metadata** option. Keep duckmidi metadata to reopen the file with its editor details, or strip it for plain MIDI.
3. Click **Export MIDI** and choose a filename.

The file contains the song as you hear it: muted channels are left out.

### Audio

1. Press **Ctrl + E** and select **Digital Audio**.
2. Choose WAV, MP3, AAC, Ogg Vorbis, Opus or FLAC from **Codec**, and set its quality options.
3. Click **Export** and choose a filename.

Audio is rendered with your SoundFont at 48 kHz stereo. A progress bar shows while it renders; **Cancel audio export** stops it, and **Open file location** opens the folder when it's done. Sound from an external MIDI device is not recorded.

### M64

![The Export window with M64 selected](images/export-m64.png)

Choose **M64** in the format dropdown, review the instrument mappings (**Similar instruments** suggests close matches to the default that reach your notes; **Manual selection** lists them all), then click **Save M64 Package** or **Export to Repository** to install it into your SM64, HackerSM64 or sm64coopdx project. **Target** chooses the game of the saved package. The list on the left shows each used channel with its instrument, or the name of the custom sample it plays, and its General MIDI number; select one to change its mapping on the right. If colored ports have to merge, the warning window lists them after the export. Add **M64 - Loop Start** and **M64 - Loop End** song events to repeat part of the song. The [M64 guide](sm64-export.md) covers the details.

### OoT

Choose **OoT** in the format dropdown, pick the **Area** whose song to replace (or add a new song) and a **Sample Set**, review the instrument mappings, then click **Save OoT Package** or **Export to Repository** to install it into a zeldaret/oot checkout. The M64 loop events also set the OoT loop. The [OoT guide](oot-export.md) covers the details.

Until an exporter has a checkout folder, its **Export to Repository** button reads **Choose Folder** and only asks for the folder; click **Export to Repository** afterwards to install. To set or change the folder, open **Edit > Preferences** and use **Browse** under **Export plugins**; **Clear** forgets it, and the button reads **Choose Folder** again. The folders are saved in `duckmidi-plugins.ini` beside your preferences.

### Custom game samples

Save the preset in **Edit > Samples**, then select a source row in the M64 or OoT export mappings and check **Custom** below **Default mapping**. Choose its **Sample Set** and click a preset. The list initially shows presets classified for that source's GM program or drum key; **Show all** also shows unclassified presets. Each row keeps its own choices, and Custom starts off even when playback uses samples.

Choose **Add** to include new native audio with the song. Choose **Replace** to select an existing repository sample by its label and path; inspect the repository first to populate the targets. For a preset with several regions, choose the region to assign. Replace affects every repository instrument or sound effect sharing that physical file and keeps its original as an exact `.aiff.bkp` or `.wav.bkp` sidecar.

OoT custom samples use **Custom** in the exporter's **Sample Set** option. With a stock Sample Set or sm64coopdx, custom selections are ignored and the ordinary instruments export with a warning. Repository export also uses the detected game's mappings when it differs from **Target**.

Sample pan exports for melodic notes and drums. MIDI pan events on the channel, including explicit centre pan, and non-default mixer pan take precedence. **Cut / Cut by** shortens earlier matching notes on the same channel and preset, including pedal-held notes; the game's release tail still plays. Sustain-only loops play as continuous loops. Replacing a sample also changes the shared pan of native drums that use it; **Revert** restores those entries with the sample.

Export continues when a preset has settings the game cannot play. The warning window lists each changed setting by preset. Sample reverb and vibrato become the game's channel effects unless the channel has its own MIDI reverb or modulation. Filters and chorus are baked into the exported sample, so they shift with each note's pitch; the filter envelope is left out. Melodic notes always use 100% key tracking, while custom drums keep theirs. Oversized envelopes are clamped, loud samples have their gain lowered, and samples that cannot fit use the row's ordinary game instrument. An unavailable or incompatible Replace becomes Add. The [native sample reference](native-samples.md) lists what each setting becomes.

### Export warnings

When a game export succeeds but had to adapt the song, the **Export warnings** window lists what changed under short headings: **Omitted** (such as SysEx or a controller the game has no use for), **Converted** (such as timing rounded to the game's grid), **Custom samples**, **Merged ports** and **May not play as expected** (such as memory limits). The headline "export was saved with audible changes or playback limitations" appears only when the exporter had to change the notes themselves as a last resort to fit the song, for example by merging notes or skipping notes that have no free channel.

### Repository history

Click the arrow at the right of **Export format** to open **Repository history** in a drawer beside the Export window. Choose the target, checkout and build version under **Section**; the checkout's folder shows below it. Select a modification to see its paths, or click its **Revert** button. Reverting a shared sample keeps the latest remaining songs and their unrelated edits. Repeated exports update the same current modification.

**Initialize** adopts the checkout's current audio as the baseline and discards that section's previous revert records after confirmation. **Revert all** restores its initialized audio state after confirmation. Outside edits stop an operation with the conflicting path; history retains the originals for recovery. Keep builds stopped while applying or reverting repository changes. Saving a package leaves repository files and history unchanged; follow its included installation guide.

History needs the `db` library (`db.dll` on Windows) that ships in the `plugins` folder beside the M64 and OoT plugins. The history itself is stored in `plugins/history`.

## Check for updates

![The Updates window](images/updates.png)

After you open your first song, duckmidi asks whether to check for updates. Choose **On Launch** and click **Apply** to check each time duckmidi starts, or keep **Never**. Click **Check now** for a one-off check. duckmidi only goes online when you choose one of these.

You can open the window again from **Help > Check for Update**. When an update is available, click **Download update**, then **Restart and install** or **Install when duckmidi closes**. Your settings, layout and projects are kept. Updates are available on Windows.

## Customize shortcuts

Open **Edit > Keyboard shortcuts**, choose an action and press its new keys. **Escape** closes the active window first.

On Windows, preferences and layout are saved beside the program. On macOS they are in `~/Library/Application Support/duckmidi/`, and on Linux in `~/.local/share/duckmidi/`. To edit the INI files by hand, close duckmidi first. Paths and shortcuts are quoted, and action names use underscores:

```ini
soundfont="C:\Music\my-font.sf2"
play_pause="Space"
color_selected_notes="Alt + C"
```

Use `+` between keys, such as `"Shift + Alt + E"`. Percussion entries use drum names, such as `acoustic_snare="Z"`. Shortcuts saved by earlier versions keep working.
