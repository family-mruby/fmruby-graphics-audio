#!/usr/bin/env ruby
# Generate the RubyKaigi intro fanfare as an FMSQ file.
# About 2 seconds (~120 frames @ 60Hz), no loop.
# Structure:
#   pulse1 ascending arpeggio C5 -> E5 -> G5
#   triangle bass C3 sustain from the arpeggio into the chord
#   pulse2 joins for C5+E5+G5 chord on the flash
#   pulse1 moves to C6 for the bright finish
#   all channels silence at the end

CPU_FREQ = 1789773 # NTSC

REG_PULSE1_VOL  = 0x00
REG_PULSE1_LO   = 0x02
REG_PULSE1_HI   = 0x03
REG_PULSE2_VOL  = 0x04
REG_PULSE2_LO   = 0x06
REG_PULSE2_HI   = 0x07
REG_TRI_LINEAR  = 0x08
REG_TRI_LO      = 0x0A
REG_TRI_HI      = 0x0B
REG_STATUS      = 0x15

FMSQ_CMD_END = 0xFE

def freq_to_timer(freq)
  (CPU_FREQ / (16.0 * freq) - 1).round
end

def reg_write(offset, value)
  [0xC0 | (offset & 0x1F), value & 0xFF].pack('CC')
end

def wait(frames)
  out = "".b
  while frames > 0
    w = [frames, 128].min
    out << [(w - 1) & 0x7F].pack('C')
    frames -= w
  end
  out
end

def note_on_pulse(ch, freq, volume: 12, duty: 2)
  timer = freq_to_timer(freq)
  vol_reg = ch == 1 ? REG_PULSE1_VOL : REG_PULSE2_VOL
  lo_reg  = ch == 1 ? REG_PULSE1_LO  : REG_PULSE2_LO
  hi_reg  = ch == 1 ? REG_PULSE1_HI  : REG_PULSE2_HI
  vol_byte = (duty << 6) | 0x30 | (volume & 0x0F)
  reg_write(vol_reg, vol_byte) +
    reg_write(lo_reg, timer & 0xFF) +
    reg_write(hi_reg, 0x08 | ((timer >> 8) & 0x07))
end

# The triangle's timer counts at half the pulse rate, so it needs its own
# divider: apu_helper.c uses 16 for the pulses and 32 here. Reusing
# freq_to_timer sounds the note an octave below the one asked for.
def freq_to_tri_timer(freq)
  (CPU_FREQ / (32.0 * freq) - 1).round
end

def note_on_triangle(freq)
  timer = freq_to_tri_timer(freq)
  reg_write(REG_TRI_LINEAR, 0xFF) +
    reg_write(REG_TRI_LO, timer & 0xFF) +
    reg_write(REG_TRI_HI, 0x08 | ((timer >> 8) & 0x07))
end

def silence_pulse(ch)
  reg_write(ch == 1 ? REG_PULSE1_VOL : REG_PULSE2_VOL, 0x30)
end

def silence_triangle
  reg_write(REG_TRI_LINEAR, 0x80)
end

NOTES = {
  'C3' => 130.81,
  'C5' => 523.25, 'E5' => 659.26, 'G5' => 783.99,
  'C6' => 1046.50,
}

output = ARGV[0] || "intro.fmsq"

data = "".b
frame_count = 0

# Enable Pulse1 + Pulse2 + Triangle
data << reg_write(REG_STATUS, 0x07)

# --- Phase 1: pulse1 ascending arpeggio C5 -> E5 -> G5
data << note_on_pulse(1, NOTES['C5'], volume: 13, duty: 2)
data << wait(12)
data << note_on_pulse(1, NOTES['E5'], volume: 13, duty: 2)
data << wait(12)
data << note_on_pulse(1, NOTES['G5'], volume: 13, duty: 2)
data << wait(12)
frame_count += 36

# --- Phase 2: triangle C3 bass joins, pulse1 holds G5
data << note_on_triangle(NOTES['C3'])
data << wait(32)
frame_count += 32

# --- Phase 3: the "flash" moment -- full C5+E5+G5 chord
data << note_on_pulse(1, NOTES['E5'], volume: 14, duty: 2)
data << note_on_pulse(2, NOTES['C5'], volume: 13, duty: 1)
# re-trigger triangle to reinforce the bass at the flash
data << note_on_triangle(NOTES['C3'])
data << wait(50)
frame_count += 50

# --- Phase 4: pulse1 lifts to C6 for a bright finish
data << note_on_pulse(1, NOTES['C6'], volume: 14, duty: 2)
data << note_on_pulse(2, NOTES['G5'], volume: 12, duty: 1)
data << wait(60)
frame_count += 60

# --- Phase 5: tail fade -- drop pulse2, keep pulse1+triangle soft
data << note_on_pulse(1, NOTES['C6'], volume: 8, duty: 2)
data << silence_pulse(2)
data << wait(40)
frame_count += 40

# --- End: silence everything
data << silence_pulse(1)
data << silence_pulse(2)
data << silence_triangle
data << wait(10)
frame_count += 10

data << [FMSQ_CMD_END].pack('C')

header = ["FMSQ", 1, 0, frame_count & 0xFFFF, data.bytesize & 0xFFFF, 0].pack('a4CCvvv')

File.binwrite(output, header + data)
duration = frame_count / 60.0
puts "Generated #{output}: #{frame_count} frames (#{duration.round(2)}s), #{data.bytesize} bytes"
