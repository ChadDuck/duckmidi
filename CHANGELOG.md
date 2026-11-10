# duckmidi changelog

# 0.12.3

## **Midi Input Support**
- Piano Input.
- Remappable MIDI percussion.
- Volume rockers and mute for mixer channels.
- Mappable Misc MIDI inputs in 'Preferences'.
- Still working out how to implement knobs and manufacturer-specific settings.
- Expect bugs (also a weird UI), I haven't had the chance to implement this to my liking.
    - In addition, I only have a single MIDI input device to test this feature with, so there may be missing options, currently.

## Changes

1. Frame rate only lowers while minimized now, so dragging files in stays responsive.
2. Pattern Instrument auto-selection logic modified.
3. In the pattern editor, **Escape** clears both the note and event selections before it closes the editor.
4. Playlist patterns snap to quarter beats when those grid lines are visible.
    - Still working out the kinks on this, but better than having *no* quarter beat snapping.
5. Clicking a freely placed playlist pattern no longer moves it.
6. **Ctrl + Shift + C / X** copies or cuts the selected notes with their pitch bends, and **Ctrl + V** pastes them as expected.
    - Both shortcuts can be changed.
7. The Events Editor now has **Copy** and **Paste** buttons; hold **Shift** to **Copy all** event types.
8. The playlist's channel labels are now wider by default.
9. Event types now follow the application color. Note labels are also aware of the background color, so they don't go invisible.
10. The percussion mapping window has an additional button now, **Listen for MIDI**; remap keys to your liking when playing drums.
11. New event editor style: **Graph** mode.
    - Shows events as vertical bars on a grid
    - Drag to paint bars, right-drag to erase them, Shift + drag to change selected bars
    - Use **Snap bars** or Quantize to set bar heights to the nearest grid line.
    - Convert between point and graph (This is a bit buggy right now, beware).
12. Pressing **E** to open the pattern editor now focuses it automatically.
13. Undo and redo now keep 64 steps instead of 32.
    - I'll be making this customizable in the future.
14. Dragging a pattern onto the playlist no longer shows a small empty box near the mouse.
15. Modified playhead play/pause/stop logic.
16. Note-edge resizing has a smaller clickable area; notes are easier to drag when zoomed out.
17. Right-clicking now allows you to deselect items, as well as delete notes/patterns.
18. Clicking a single pattern/note with multiple selected now unselects all items. This is expected behavior, use 'Shift' to maintain or add to your selection.
19. The playhead now has a drop shadow, so it stays visible.
20. Previews of events in patterns should now align with its actual visual appearance, more or less.
21. The Event Editor's LFO view has been shortened to a single bar. Previously, this would stretch according to the size of the pattern - making it hard to align.

# 0.12.2

1. The brush tool shortcut was always active, regardless of the keyboard piano state. This has now been fixed.

# 0.12.1

1. Added a repository history **Enable/Disable** checkbox, saved with your preferences; when it is off, it won't use the repository history plugin.
    - **Keep original files** still applies with history off; with both off, exports simply overwrite files.
2. Channels that play a custom sample show the sample's name instead of its filename.
3. In Samples, the waveform's tip stays hidden while you drag.

# 0.12.0

## **Custom Samples**
- Bring your own sounds into duckmidi. 
    - **Edit > Samples** imports WAV, AIFF, FLAC, MP3.
    - Import individual samples or entire folders.
    - Store multiple "Sample Sets" to organize your samples.
    - Samples are usable along with loaded SoundFonts, identical in usage.
        - It plays everywhere SoundFont instruments play.
        - Channels that use samples show a tinted label and the sample's name.
    - .SFZ files are generated with imported samples, allowing you to use it outside of duckmidi.

## **Sample Editor**
- The Samples window lets you import and edit these samples as you wish.
    - Parameters:
        - Root note selection and tuning
        - Playback start/end
        - Note hold loops
        - ADSR envelope
        - Gain, pan, filter, vibrato, chorus, reverb
        - Cut / Cut by groups (Most used in percussion; i.e. stopping an open Hi-Hat with a closed Hi-Hat)
    - **Modifying a sample automatically updates playback to preview the change.**
        - Hitting 'Save' finalizes those changes.
    - Waveform and envelope values are modifiable with a visual graph.
        - Click and drag along these graphs in a responsive & intuitive manner
    - Typical sample metadata is also parsed and imported.
        - Things like root note, loop points, playback start & end position; all imported.
        - duckmidi reads filename and metadata information to populate options *for you*.

## **Custom Sample Export**
- Each mapping in the Export window can add a saved sample to the game, or replace one of the game's own samples.
    - Converted automatically to a format the game supports.
    - Sampler parameters are automatically converted into a format the game supports
        - If the game doesn't support certain values, duckmidi bakes it into the export.
        - Anything the game *can't* do is given a clear warning.

## **Repository History**
- 'Export to Repository' now remembers every change it makes to your local repository.
    - The 'History' drawer beside the export window can revert a single export or all of them, no matter how many songs, banks and samples it's modified.
        - **Songs that share samples or memory settings are kept intact.** Say, if you changed a sample during an export, then removed the export. All sequences which use that sample afterwards stay intact.
    - This replaces the old 'Revert repository export' button.
    - **Still in active testing** Please be patient with the tracking tool, and back up your projects accordingly.

## Current Known Considerations/Bugs

- Custom samples for percussion in duckmidi playback don't work as expected.
    - You can still set them as percussion instruments during your export.
- Repository history will refuse to export if it detects a .bkp file for files it edits.
    - If this happens, and the file was not explicitly modified by you, restore the .bkp and re-initialize the repository history.
        - This will reset all history and set the 'source of truth' to your current modifications layout.

## Changes

1. Opening a .duck file in your file manager launches duckmidi and loads the project.
2. New "Background Color" setting in preferences.
3. Text switches between light/dark lettering depending on background color.
4. Moved playlist tools to the right edge of the playlist title bar.
5. BPM shows tempo changes during playback (similar to the mixer volume/pan); still edits only the starting BPM.
6. The Record button turns red when recording is enabled.
7. Modified 'Preferences' window size logic.
8. Allow more mouse buttons to be used in keyboard shortcuts.
9. Opening a file asks about unsaved changes *after* you choose the file, not before.
10. Modified 'Piano Roll' hide/show logic.
11. Modified channel/instrument creation behavior,
    - Use **+ New** in the playlist to choose a free MIDI channel, its type (melody/drums), and its instrument.
12. Right-click a playlist row to edit/remove its channel, with a confirmation if it contains a pattern.
13. Modified keyboard shortcut logic relating to creation/deletion of patterns.
14. Keypad shortcuts step through edited patterns in both pattern categories.
15. MIDI events are now grouped, reducing visual clutter. 
    - A discrete bar holding all events which are contained where previous independent MIDI events were section.
    - Click these bars to show only the events contained in that section.
16. Modified MIDI events visual structure, should be simpler to edit.
17. MIDI import removes redundant copies of MIDI events and merges identical program changes.
18. Instrument selections preview on the playing song while selecting.
19. Playlist automation expanders are visually distinct when they *contain* automation patterns.
21. The mixer only shows a channel's route 0 strip if a channel uses it.
24. Changed export window shown warning & errors, should be simpler and less cluttered.
29. Export plugins now use plugin ABI 11, and any custom export plugin can use the repository history.
32. M64 songs with loop points keep playing in SM64 after the loop start, instead of going silent there.
33. Modified MIDI export staged compression and song simplification
    - You should be able to fit larger songs within the game, the staged implementation minimizing audible losses as duckmidi exports (unless necessary).
    - You should also be less likely to run into a song which won't export *at all*.

## 0.11.3

1. OoT 'Export to Repository' removes songs earlier exports left behind after they were replaced, freeing their soundfonts; 'Revert repository export' brings them back.

## 0.11.2

1. Pattern playback uses the instrument changes, volume, and pan from the pattern's spot in the song instead of the start of the song.
2. A 'Reset' button beside 'Use selected channel' in the piano roll returns the pattern to its channel's instrument.
3. The song event window shows instrument names, controller names, notes and pitch bend values for imported MIDI events instead of generic technical terms.
4. Editing an imported controller in the song event window no longer replaces its value with the channel number.
5. The mixer's volume and pan follow the song's volume and pan changes while it plays.
6. Undoing a pitch bend drawn on a note no longer removes the note too.
7. Drawing a pitch bend on a note no longer opens the Events section.
8. Clicking the playlist no longer places a pattern.
9. "Shift + keypad -" removes a new, unused automation pattern, like "keypad -" does for regular patterns.
10. The LFO view in automation patterns shows the whole pattern without scrolling sideways; pitch bends shown as note pitches still scroll up and down.
11. Switching an automation pattern's controller, such as to Modulation, can now be undone and redone.
12. The piano roll's instrument dropdown shows the instrument playing at the playhead.
13. Mixer volume and pan changes now apply to imported MIDI files that set them just after the start.
14. Moving a mixer volume or pan control while the song plays holds until the song changes it again.
15. The mixer's volume and pan now move as the song plays through its changes, not only when seeking.
16. A pattern card dragged over the playlist hides, so the outline of where it will land stays visible.
17. Shift + drag on a pattern on the playlist drags a linked copy of it, or of all selected patterns, leaving the originals in place.
18. Middle-drag on a note drags a copy of it, or of all selected notes, keeping length, velocity and color.
19. Modified some recording logic relating to patterns.

## 0.11.1

New export plugin/option: OoT (`OoT.dll`): Save an *Ocarina of Time* sequence package, or use 'Export to Repository' to export directly to a [zeldaret/oot](https://github.com/zeldaret/oot) checkout.
    - Still testing it right now, but initial results are promising.

1. 'Revert repository export' also removes the backups and new folders left by repeated exports to the same local repo.
2. The piano roll no longer lags on patterns with thousands of pitch bends.
    - This bug made me laugh. Each frame, the drawing pass would work out every bend point's position.
    - For a single point, this would scan the project's entire event list once, or twice for a curved bend. Single-point bends also rescanned the pattern's notes each time, only to find the same note for every point.
    - Editing made it worse: Alt- or Shift-clicking repeated the work, so one point's position could be worked out **up to four times in a single frame**.
    - So, with each frame, every bend point would make the program scan *all* events in the project; with thousands of bends, that's **millions** of steps per frame.
3. The default layout raises the piano roll to the height of the mixer; the four sections meeting by default.
4. A dragged pattern card stays held at the spot you started dragging from instead of hanging to the bottom-right of the cursor.
5. OoT export maps each General MIDI instrument to the closest OoT equivalent, or uses one of the game's soundfonts, and offers substitutes when an instrument cannot reach the register range.
6. OoT export can enlarge the game's audio memory automatically when a song is too large.
7. The M64 export window has a Target option: 'SM64', 'HackerSM64' or [sm64coopdx](https://github.com/coop-deluxe/sm64coopdx).
    - The program will always export to the right repository, this is mainly for zipped exports.
8. Saved M64 packages follow the Target: HackerSM64 packages explain its sequence memory setting, and sm64coopdx packages are a ready-to-use mod folder.
    - This needs testing from romhack creators.
9. 'Export to Repository' installs sm64coopdx songs as a mod in the checkout's 'mods' folder, with the usual backups and 'Revert repository export'.
    - This may need adjustment, I'm not sure if this is the intended behavior (as sm64coopdx treats romhacks as mods rather than core modifications).
    - Like I said, it needs testing from romhack creators. I'll test it myself, eventually.
10. sm64coopdx songs play with SM64's samples as expected. The export warns where an instrument or drum will sound different.
    - sm64coopdx also has a different way of mapping instruments together so things may not be the same if switching ports.
    - From what I understood, sm64coopdx also doesn't support custom samples?
    - I need to investigate these two claims further and address them in a future release if it's a problem.
11. Plugin sequence names default to the project's name, or the MIDI file's name for an unnamed project.
12. 'Sequence memory' in both exports now offer a 'Stock' option: This exports just the song data.
13. With an 'Enlarge' choice, exports no longer warn that the song exceeds the game's memory pools, since 'Export to Repository' makes room.
14. Shortened 'Loop the complete song'; now just 'Loop song'.
15. The instrument mapping panel has a 'Similar instruments' dropdown, listing instruments close to the default mapping that reach the source's notes; above the full 'Manual selection' list.
16. 'Keep original files' checkbox now shows before an export.
17. 'Export to Repository' asks for the local repository folder once instead of every time; the folder can be set or cleared in Preferences under 'Export plugins'.
18. Mixer channel labels use the color of the pattern on that channel's arrangement row, or the application color.
19. Export plugins now use plugin ABI 5.
20. Imported MIDI files that play drums on channels other than 10 show those channels as percussion, and M64 and OoT plugins export them as drums.
21. Projects save which channels play percussion (project schema 12); older projects keep channel 10 as their drum channel, as do new projects.
22. M64 'Sequence memory' offers 'Enlarge' and 'Stock' for SM64 and HackerSM64; the 'Emulator Only' option is removed, and 'Enlarge' always makes the smallest change needed to fit the sequence.
23. The custom mouse cursor now has a soft drop shadow.

## 0.11.0

1. *duckmidi* can now check for updates on launch or from 'Help' > 'Check for Update'. **Disabled by default.**
2. 'Export to Repository' now works with [HackerSM64](https://github.com/HackerN64/HackerSM64) projects.
3. 'Export to Repository' works again after an export changed local project's sequence memory.
4. The M64 export window has a Sequence memory option: 'Console-Compatible' or 'Emulator Only'.
5. Songs with colored note ports can be exported to M64; ports move to free channels and the window warns when merged ports change the same events.
6. The percussion keyboard mapping window plays drums when you press mapped keys, similar to the instrument window.
7. 'Shift + E' shows or hides the Piano Roll's events, or opens the 'Event Editor' when the piano roll is hidden.
8. The mouse wheel steps through the choices of every dropdown.
9. Event points have a 'Shift + right-click' menu to change their value, curve them toward the next point, or delete them.
10. 'Ctrl + C' and 'Ctrl + V' copy and paste patterns in the pattern section, and right-clicking a pattern card opens its menu.
11. Editing shortcuts no longer change the arrangement while the mixer is focused.
12. Menus show your own keyboard shortcuts.
13. The status bar shows messages for a few seconds without a box rather than staying permanently.
14. Tooltips wrap instead of being truncated.
15. The top of the events grid now meets the ruler.
16. The mixer no longer shifts when its scrollbar appears.
17. The M64 save button has been renamed to 'Save M64 Package'.
18. *duckmidi* GitHub actions enabled; releases will be built for testing, available for macOS, Arch, Ubuntu, and Debian.
19. On macOS and Linux, preferences and layout are saved in your user folder instead of beside the program.
20. Press 'I' in the playlist or piano roll to choose the channel's instrument, which also sets the piano roll's instrument dropdown.
21. Press 'P' anywhere to open 'Preferences'; 'Project settings' default moved from 'P' to '/'.
22. Pattern cards can be dragged to rearrange them in the pattern section.
23. New projects start with channel 1 playing a piano alongside the channel 10 drums.
24. Clicking in an event lane adds a single point and dragging draws a line.
25. 'Shift + click' in an event lane adds a point connected to its nearest neighbor; 'Shift + drag' draws a line - both points connecting to its nearest neighbor.
26. Event lane grid line previews now snap by default while dragging.
27. The 'Events Editor' right-click menu has a 'Curve' option.
    - 'Line' is a straight line to the next point
    - 'Hold' holds the value until it reaches the next point - forming a 90 degree angle.
30. '.duck' format schema is now 11; new value added to track held notes.
31. Fixed an issue with the drum channel not showing with it selected; 'Enter' now applies drum changes, like with the regular 'instruments' window.
32. Added an option to the drums dropdown after clicking 'Drum kit' to return to regular instruments.
33. An unsaved percussion mapping no longer carries over into a new or opened project.
34. The percussion keyboard mapping and 'Instrument' windows fit their contents.
35. Right-dragging the ruler to loop part of a short pattern works again.
36. After recording, the piano roll is automatically focused so its shortcuts work.
37. Beat grid lines are now darker than bar grid lines, and the playlist shows quarter beats when zoomed in.
38. Mouse cursor changed solid instead of an outline.
39. The user guide has been rewritten with imagery to make it less boring.
40. Program changes are placed as single points; dragging in their lane no longer draws a line.
41. Channel pressure and key pressure lanes draw lines, and channel pressure lines play their changing values.

## 0.10.2

1. Backwards-Compatibility has been implemented from v0.10.1 onwards.
2. Project files and MIDI files with editor metadata have been re-worked, resulting in smaller file sizes.
3. MIDI files that use several ports open as one arrangement, with each source port on its own color.
4. Project settings can combine the original source ports, or all ports, into their channels.
5. MIDI export leaves muted channels out and includes colored ports and other source sequences.
6. Patterns can be given one of 16 colors with Alt + C or 'Change color' in the right-click menu.
7. 'Ctrl + D' duplicates the clicked pattern in the pattern browser.
8. The slice tool now works as intended; slicing a pattern cuts it at the selected position, both instances tied to each other.
9. Clicking the events lane adds a connected point, 'Shift + click' or drag draws, and right-drag erases.
10. Program change points can be erased and deleted, and they show the instrument name while you place them.
11. The events section opens taller and grows the piano roll upward instead of shrinking the piano roll.
12. Pitch bends drawn on a note now move along with said note.
13. 'Shift + Keypad +'' and 'Shift + Keypad −' step through automation patterns.
14. 'Shift + click' on a port color button to delete the port.
15. Choosing 'Drum kit' moves a channel to row 10, which lists drum kits.
16. Program changes before a channel's first note follow the Instrument dialog.
17. The mouse wheel scrolls through the event type dropdown when hovered over.
18. The hover information box is back in the status bar.
19. The editor keeps drawing while a file dialog is open.
20. Large songs draw and edit several times faster.
21. The slice and paint tools have new icons.
22. M64 - Loop Start and M64 - Loop End song events make M64 exports repeat part of the song.
23. M64 packages include memory settings for console-compatible and emulator-only builds.
24. Export to Repository installs an M64 package into a local 'SM64' repo, with backups and Revert repository export.
25. M64 sample lists show each sample once without the GM prefix.
26. Older shortcut settings keep their bindings, and new actions use their default keys.
27. Major codebase refactor, cleaner overall presentation.
28. Performance Metrics - v0.10.2 release candidate with Complex MIDI test file (Human Test, running the program & file):
    - RAM Usage: From ~1.1 GiB to ~ goes from ~1.1 GiB to ~150 MiB.
    - CPU Usage (All threads): From ~5% to ~3%.
    - GPU Usage: From ~12% to ~8%.
