/* MIDI values shared by duckmidi and its export plugins.

   Channel messages combine a status from the first group with a channel number (0-15):
   MIDI_NOTE_ON | channel. MIDI_STATUS_MASK and MIDI_CHANNEL_MASK split them again. */
#ifndef DUCKMIDI_MIDI_H
#define DUCKMIDI_MIDI_H

/* Channel message status, upper four bits of the first byte */

#define MIDI_NOTE_OFF 0x80
#define MIDI_NOTE_ON 0x90
#define MIDI_KEY_PRESSURE 0xa0
#define MIDI_CONTROL 0xb0
#define MIDI_PROGRAM 0xc0
#define MIDI_CHANNEL_PRESSURE 0xd0
#define MIDI_PITCH_BEND 0xe0

/* Program change and channel pressure carry one data byte; the others carry two. Both statuses
   match (status & MIDI_SHORT_MASK) == MIDI_PROGRAM. */
#define MIDI_SHORT_MASK 0xe0

/* System and file messages */

#define MIDI_STATUS 0x80 /* First status byte; smaller bytes are data */
#define MIDI_SYSTEM 0xf0 /* First status that is not a channel message */
#define MIDI_SYSEX 0xf0
#define MIDI_SYSEX_END 0xf7
#define MIDI_META 0xff

/* Meta event types, the byte after MIDI_META */

#define MIDI_META_TEXT 0x01
#define MIDI_META_LYRIC 0x05
#define MIDI_META_MARKER 0x06
#define MIDI_META_PORT 0x21
#define MIDI_META_END 0x2f
#define MIDI_META_TEMPO 0x51
#define MIDI_META_TIME 0x58
#define MIDI_META_KEY 0x59
#define MIDI_META_SEQUENCER 0x7f

/* Masks and ranges */

#define MIDI_STATUS_MASK 0xf0
#define MIDI_CHANNEL_MASK 0x0f
#define MIDI_DATA_MASK 0x7f
#define MIDI_DATA_MAX 127
#define MIDI_CHANNELS 16
#define MIDI_KEYS 128
#define MIDI_DRUM_CHANNEL 9 /* General MIDI channel 10, counted from zero */

/* Pitch bend: two 7-bit bytes, least significant first */

#define MIDI_BEND_CENTER 8192
#define MIDI_BEND_MAX 16383

/* Controller numbers */

#define MIDI_CC_BANK 0
#define MIDI_CC_MODULATION 1
#define MIDI_CC_PORTAMENTO_TIME 5
#define MIDI_CC_DATA 6
#define MIDI_CC_VOLUME 7
#define MIDI_CC_PAN 10
#define MIDI_CC_EXPRESSION 11
#define MIDI_CC_BANK_LSB 32
#define MIDI_CC_DATA_LSB 38
#define MIDI_CC_SUSTAIN 64
#define MIDI_CC_PORTAMENTO 65
#define MIDI_CC_SOSTENUTO 66
#define MIDI_CC_REVERB 91
#define MIDI_CC_CHORUS 93
#define MIDI_CC_INCREMENT 96
#define MIDI_CC_DECREMENT 97
#define MIDI_CC_NRPN_LSB 98
#define MIDI_CC_NRPN_MSB 99
#define MIDI_CC_RPN_LSB 100
#define MIDI_CC_RPN_MSB 101
#define MIDI_CC_SOUND_OFF 120
#define MIDI_CC_RESET 121
#define MIDI_CC_NOTES_OFF 123

#endif
