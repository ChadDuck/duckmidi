# duckmidi changelog

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
26. The 'Events Editor' right-click menu has a 'Curve' option.
    - 'Line' is a straight line to the next point
    - 'Hold' holds the value until it reaches the next point - forming a 90 degree angle.
27. '.duck' format schema is now 11; new value added to track held notes.
28. Fixed an issue with the drum channel not showing with it selected; 'Enter' now applies drum changes, like with the regular 'instruments' window.
29. Added an option to the drums dropdown after clicking 'Drum kit' to return to regular instruments.
30. An unsaved percussion mapping no longer carries over into a new or opened project.
31. The percussion keyboard mapping and 'Instrument' windows fit their contents.
32. Right-dragging the ruler to loop part of a short pattern works again.
33. After recording, the piano roll is automatically focused so its shortcuts work.
34. Beat grid lines are now darker than bar grid lines, and the playlist shows quarter beats when zoomed in.
35. Mouse cursor changed solid instead of an outline.
37. The user guide has been rewritten with imagery to make it less boring.
38. Program changes are placed as single points; dragging in their lane no longer draws a line.
39. Channel pressure and key pressure lanes draw lines, and channel pressure lines play their changing values.

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