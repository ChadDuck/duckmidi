# duckmidi changelog

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
11. The events section opens taller and grows the pattern editor upward instead of shrinking the piano roll.
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