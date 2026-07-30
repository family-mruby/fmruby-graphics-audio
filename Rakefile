# Rakefile — fmruby-graphics-audio ESP-IDF build wrapper (Docker)
require "rake"

EXPECTED_CHIP = "ESP32"
PORT_CACHE_FILE = ".serial_port"
PROBE_PORTS = ["/dev/ttyUSB0", "/dev/ttyUSB1"]

def get_serial_port
  if File.exist?(PORT_CACHE_FILE)
    cached = File.read(PORT_CACHE_FILE).strip
    if File.exist?(cached)
      return cached
    else
      abort "Cached port #{cached} no longer exists. Run 'rake check-port'"
    end
  end
  abort "Serial port not configured. Run 'rake check-port' first."
end

# Load environment variables from .env file
if File.exist?(".env")
  File.readlines(".env").each do |line|
    line.strip!
    next if line.empty? || line.start_with?("#")
    key, value = line.split("=", 2)
    ENV[key] = value if key && value
  end
end

UID  = `id -u`.strip
GID  = `id -g`.strip
PWD_ = Dir.pwd

# Pin the build image to a version tag, never :latest -- two machines on the same
# commit have already ended up with different IDF and toolchain versions that
# way, and :latest here was in fact 1242 commits past the v5.5.4 tag while the
# rest of the project built against the release. Keep in sync with .env,
# fmruby-core/Rakefile and the root docker-compose.yml.
ESP_IDF_VERSION = ENV.fetch("ESP_IDF_VERSION", "v5.5.4")
IMAGE           = ENV.fetch("DOCKER_IMAGE",
                            "ghcr.io/family-mruby/fmruby-esp32-build:#{ESP_IDF_VERSION}")

# Detect available serial devices
DEVICE_ARGS = Dir.glob("/dev/ttyUSB*").concat(Dir.glob("/dev/ttyACM*"))
                 .select { |dev| File.exist?(dev) }
                 .map { |dev| "--device=#{dev}" }
                 .join(" ")

# Always use current user's UID:GID to avoid permission issues
USER_OPT = "--user #{UID}:#{GID}"

DOCKER_CMD = [
  "docker run --rm",
  USER_OPT,
  "-e HOME=/tmp",
  "-v #{PWD_}:/project",
  IMAGE
].join(" ")

DOCKER_CMD_PRIVILEGED = [
  "docker run --rm",
  "--group-add=dialout --group-add=plugdev --privileged",
  DEVICE_ARGS,
  USER_OPT,
  "-e HOME=/tmp",
  "-v #{PWD_}:/project",
  "-v /dev/bus/usb:/dev/bus/usb",
  IMAGE
].join(" ")

DOCKER_CMD_INTERACTIVE = [
  "docker run --rm -it",
  "--group-add=dialout --group-add=plugdev --privileged",
  DEVICE_ARGS,
  USER_OPT,
  "-e HOME=/tmp",
  "-v #{PWD_}:/project",
  "-v /dev/bus/usb:/dev/bus/usb",
  IMAGE
].join(" ")

desc "Apply component patches (file replacement)"
task :apply_patches do
  # LovyanGFX patches
  lovyangfx_dir = "components/LovyanGFX"
  lovyangfx_patch_dir = "patches/lovyangfx-files"

  lovyangfx_mappings = {
    "#{lovyangfx_patch_dir}/esp-idf.cmake" => "#{lovyangfx_dir}/boards.cmake/esp-idf.cmake",
    "#{lovyangfx_patch_dir}/common.hpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/common.hpp",
    "#{lovyangfx_patch_dir}/device.hpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/device.hpp",
    "#{lovyangfx_patch_dir}/Panel_sdl.cpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp",
    # Misaki 8x8 Japanese bitmap font (license: see Fonts/misaki/COPYRIGHT.txt).
    "#{lovyangfx_patch_dir}/Fonts/misaki/lgfx_misaki.h"   => "#{lovyangfx_dir}/src/lgfx/Fonts/misaki/lgfx_misaki.h",
    "#{lovyangfx_patch_dir}/Fonts/misaki/lgfx_misaki.c"   => "#{lovyangfx_dir}/src/lgfx/Fonts/misaki/lgfx_misaki.c",
    "#{lovyangfx_patch_dir}/Fonts/misaki/lgfx_misaki.cpp" => "#{lovyangfx_dir}/src/lgfx/Fonts/misaki/lgfx_misaki.cpp",
    "#{lovyangfx_patch_dir}/Fonts/misaki/lgfx_misaki_fonts.hpp" => "#{lovyangfx_dir}/src/lgfx/Fonts/misaki/lgfx_misaki_fonts.hpp",
    "#{lovyangfx_patch_dir}/Fonts/misaki/COPYRIGHT.txt"   => "#{lovyangfx_dir}/src/lgfx/Fonts/misaki/COPYRIGHT.txt",
  }

  # esp_littlefs patches
  esp_littlefs_dir = "components/esp_littlefs"
  esp_littlefs_patch_dir = "patches/esp_littlefs-files"

  esp_littlefs_mappings = {
    "#{esp_littlefs_patch_dir}/CMakeLists.txt" => "#{esp_littlefs_dir}/CMakeLists.txt"
  }

  # Apply all patches
  puts "Applying component patches (file replacement)..."

  puts "  LovyanGFX patches:"
  lovyangfx_mappings.each do |src, dst|
    if File.exist?(src)
      mkdir_p File.dirname(dst)
      sh "cp #{src} #{dst}"
      puts "    OK #{File.basename(src)} -> #{dst}"
    else
      puts "    Warning: Source file not found: #{src}"
    end
  end

  puts "  esp_littlefs patches:"
  esp_littlefs_mappings.each do |src, dst|
    if File.exist?(src)
      sh "cp #{src} #{dst}"
      puts "    OK #{File.basename(src)} -> #{dst}"
    else
      puts "    Warning: Source file not found: #{src}"
    end
  end

  puts "Patches applied successfully"
end

namespace :set_target do
  desc "Linux target (dev/test)"
  task :linux => :apply_patches do
    sh "#{DOCKER_CMD} idf.py --preview set-target linux"
  end

  desc "Set ESP32 target"
  task :esp32 => :apply_patches do
    sh "#{DOCKER_CMD} idf.py set-target esp32"
  end
end

namespace :build do
  desc "ESP-IDF Linux simulation build (SDL2 host)"
  task :linux do
    unless Dir.exist?('build')
      Rake::Task['set_target:linux'].invoke
    end
    sh "#{DOCKER_CMD} bash -c 'export IDF_TARGET=linux && idf.py --preview -DCMAKE_BUILD_TYPE=Debug build'"
    puts 'ESP-IDF Linux build complete. Run with: ./build/fmruby-graphics-audio.elf'
  end

  desc "ESP32 build"
  # Comm transport: default is UART. To use SPI instead:
  #   CMAKE_OPTS="-DFMRB_COMM_TRANSPORT=SPI" rake build:esp32
  task :esp32 do
    unless Dir.exist?('build')
      Rake::Task['set_target:esp32'].invoke
    end
    cmake_opts = ENV['CMAKE_OPTS'].to_s
    sh "#{DOCKER_CMD} idf.py #{cmake_opts} build".squeeze(' ')
  end
end

desc "Detect and cache the correct serial port for #{EXPECTED_CHIP}"
task :"check-port" do
  ports = PROBE_PORTS.select { |p| File.exist?(p) }
  abort "No serial devices found in #{PROBE_PORTS}" if ports.empty?

  puts "Scanning ports for #{EXPECTED_CHIP}..."
  detected = nil

  ports.each do |port|
    print "  Probing #{port}... "
    docker_cmd = [
      "docker run --rm --privileged",
      "--device=#{port}",
      "-v /dev/bus/usb:/dev/bus/usb",
      IMAGE
    ].join(" ")

    output = `#{docker_cmd} esptool.py --port #{port} chip_id 2>&1`
    chip_match = output.match(/Detecting chip type\.\.\.\s*(\S+)/)
    if chip_match
      chip = chip_match[1]
      puts chip
      if chip == EXPECTED_CHIP
        detected = port
        break
      end
    else
      puts "no response"
    end
  end

  if detected
    File.write(PORT_CACHE_FILE, detected)
    puts "#{EXPECTED_CHIP} found on #{detected} (cached to #{PORT_CACHE_FILE})"
  else
    abort "ERROR: #{EXPECTED_CHIP} not found on any port"
  end
end

desc "Flash to ESP32"
task :flash do
  sh "#{DOCKER_CMD_PRIVILEGED} idf.py -p #{get_serial_port} flash"
end

desc "Check ESP32 HW"
task :check do
  sh "#{DOCKER_CMD_PRIVILEGED} esptool.py -p #{get_serial_port} flash_id"
end

desc "Open menuconfig"
task :menuconfig do
  term = ENV['TERM'] || 'xterm-256color'
  docker_cmd_interactive = [
    "docker run --rm -it",
    USER_OPT,
    "-e HOME=/tmp",
    "-e TERM=#{term}",
    "-v #{PWD_}:/project",
    IMAGE
  ].join(" ")
  sh "#{docker_cmd_interactive} idf.py menuconfig"
end

desc "Full clean build artifacts (including host)"
task :clean_all do
  sh "rm -f sdkconfig"
  sh "rm -rf build"
end

desc "Clean ESP32 build artifacts"
task :clean do
  sh "rm -rf build"
end

desc "Serial monitor"
task :monitor do
  sh "#{DOCKER_CMD_INTERACTIVE} idf.py -p #{get_serial_port} monitor"
end

desc "List available tasks"
task :help do
  sh "rake -T"
end

task :default => :help
