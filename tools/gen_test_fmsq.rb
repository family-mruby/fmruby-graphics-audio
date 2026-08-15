#!/usr/bin/env ruby
# Generate a test FMSQ file with a scale pattern (~60s, looping).

CPU_FREQ = 1789773 # NTSC

# APU register offsets (from $4000)
REG_PULSE1_VOL  = 0x00
REG_PULSE1_LO   = 0x02
REG_PULSE1_HI   = 0x03
REG_TRI_LINEAR  = 0x08
REG_TRI_LO      = 0x0A
REG_TRI_HI      = 0x0B
REG_STATUS      = 0x15

FMSQ_CMD_END  = 0xFE
FMSQ_CMD_LOOP = 0xFF

def freq_to_timer(freq)
  (CPU_FREQ / (16.0 * freq) - 1).round
end

def reg_write(offset, value)
  [0xC0 | (offset & 0x1F), value & 0xFF].pack('CC')
end

def wait(frames)
  data = "".b
  while frames > 0
    w = [frames, 128].min
    data << [(w - 1) & 0x7F].pack('C')
    frames -= w
  end
  data
end

def note_on_pulse1(freq, volume: 12, duty: 2)
  timer = freq_to_timer(freq)
  vol_byte = (duty << 6) | 0x30 | (volume & 0x0F)
  reg_write(REG_PULSE1_VOL, vol_byte) +
    reg_write(REG_PULSE1_LO, timer & 0xFF) +
    reg_write(REG_PULSE1_HI, 0x08 | ((timer >> 8) & 0x07))
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

def silence_pulse1
  reg_write(REG_PULSE1_VOL, 0x30)
end

def silence_triangle
  reg_write(REG_TRI_LINEAR, 0x80)
end

NOTES = {
  'C4' => 261.63, 'D4' => 293.66, 'E4' => 329.63, 'F4' => 349.23,
  'G4' => 392.00, 'A4' => 440.00, 'B4' => 493.88, 'C5' => 523.25,
}

output = ARGV[0] || "test.fmsq"

data = "".b
frame_count = 0

# Enable Pulse 1 + Triangle
data << reg_write(REG_STATUS, 0x05)
data << wait(10)
frame_count += 10

note_dur = 15 # ~0.25s
gap_dur  = 3

loop_offset = data.bytesize

scale_up   = %w[C4 D4 E4 F4 G4 A4 B4 C5]
scale_down = %w[B4 A4 G4 F4 E4 D4]
arpeggio   = %w[C4 E4 G4 C5 G4 E4]

4.times do
  scale_up.each do |n|
    data << note_on_pulse1(NOTES[n], duty: 2)
    data << wait(note_dur)
    data << silence_pulse1
    data << wait(gap_dur)
    frame_count += note_dur + gap_dur
  end

  scale_down.each do |n|
    data << note_on_pulse1(NOTES[n], duty: 1)
    data << wait(note_dur)
    data << silence_pulse1
    data << wait(gap_dur)
    frame_count += note_dur + gap_dur
  end

  arpeggio.each do |n|
    data << note_on_triangle(NOTES[n])
    data << note_on_pulse1(NOTES['C4'], volume: 6, duty: 0)
    data << wait(20)
    data << silence_pulse1
    data << silence_triangle
    data << wait(4)
    frame_count += 24
  end
end

# Loop back
data << [FMSQ_CMD_LOOP].pack('C')
data << [loop_offset].pack('v')

# Header (12 bytes)
header = ["FMSQ", 1, 0, frame_count & 0xFFFF, data.bytesize & 0xFFFF, loop_offset & 0xFFFF].pack('a4CCvvv')

File.binwrite(output, header + data)
duration = frame_count / 60.0
puts "Generated #{output}: #{frame_count} frames (#{duration.round(1)}s), #{data.bytesize} bytes, loop at #{loop_offset}"
