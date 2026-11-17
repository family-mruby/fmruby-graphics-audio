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

# The wire format shared with fmruby-core (components/fmrb_wire: the link
# protocol, the HID events, the simulation's shared memory) is read from the
# fmruby-core checkout next to this one, never copied (main/CMakeLists.txt).
# The container sees it read-only at /fmruby-core, beside /project, so the
# relative path main/CMakeLists.txt uses is the same inside and outside.
CORE_DIR = File.expand_path("../fmruby-core", __dir__)
CORE_MOUNT = "-v #{CORE_DIR}:/fmruby-core:ro"

def need_core!
  return if File.exist?(File.join(CORE_DIR, "components/fmrb_wire/include/fmrb_link_protocol.h"))
  abort "fmruby-core is not next to this repository (#{CORE_DIR}): graphics-audio " \
        "reads the link protocol headers from its components/fmrb_wire. Check it out there."
end

DOCKER_CMD = [
  "docker run --rm",
  USER_OPT,
  "-e HOME=/tmp",
  "-v #{PWD_}:/project",
  CORE_MOUNT,
  IMAGE
].join(" ")

DOCKER_CMD_PRIVILEGED = [
  "docker run --rm",
  "--group-add=dialout --group-add=plugdev --privileged",
  DEVICE_ARGS,
  USER_OPT,
  "-e HOME=/tmp",
  "-v #{PWD_}:/project",
  CORE_MOUNT,
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
  CORE_MOUNT,
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

# The Linux sim builds in build-linux/ with its own sdkconfig, beside the
# ESP32 build in build/ (sdkconfig at the top, as idf.py expects), so
# switching between the two rebuilds neither. sdkconfig.defaults and
# sdkconfig.defaults.linux (which idf.py adds for the linux target) are the
# defaults of both, as before.
LINUX_BUILD_DIR = "build-linux"
LINUX_SDKCONFIG = "#{LINUX_BUILD_DIR}/sdkconfig"
LINUX_IDF_ARGS = "-B #{LINUX_BUILD_DIR} -DSDKCONFIG=#{LINUX_SDKCONFIG}"
ELF_NAME = "fmruby-graphics-audio.elf"

# The sim build's stamp (fmrb_build_stamp.json): which sources it was made
# from, so sim_up and tools/dev_run_check.sh refuse a stale one. The helper
# is fmruby-core's (kind ga-linux), the same one its sim build uses.
def build_stamp
  need_core!
  require File.join(CORE_DIR, "tools/build_stamp")
  FmrbBuildStamp
end

# idf.py set-target runs a first configure that writes the sdkconfig; it is
# needed when the build directory has no CMake cache for that target.
def configured_for?(dir, target)
  cache = File.join(dir, "CMakeCache.txt")
  File.exist?(cache) && File.read(cache)[/^IDF_TARGET:\w+=(.*)$/, 1] == target
end

namespace :set_target do
  desc "Linux target (dev/test), in build-linux/"
  task :linux => :apply_patches do
    need_core!
    sh "#{DOCKER_CMD} idf.py --preview #{LINUX_IDF_ARGS} set-target linux"
  end

  desc "Set ESP32 target"
  task :esp32 => :apply_patches do
    need_core!
    sh "#{DOCKER_CMD} idf.py set-target esp32"
  end
end

namespace :build do
  desc "ESP-IDF Linux simulation build (SDL2 host), in build-linux/"
  task :linux do
    need_core!
    stamp = build_stamp
    # The sources this build is made from, taken before it starts: a file
    # changed while it runs makes the build stale, as it should.
    stamp_state = stamp.state(CORE_DIR, "ga-linux")
    Rake::Task['set_target:linux'].invoke unless configured_for?(LINUX_BUILD_DIR, "linux")
    sh "#{DOCKER_CMD} bash -c 'export IDF_TARGET=linux && idf.py --preview #{LINUX_IDF_ARGS} -DCMAKE_BUILD_TYPE=Debug build'"
    # Believe the ELF header, not this task's word: a Linux build directory
    # that somehow holds another ISA must not be stamped as good.
    elf = "#{LINUX_BUILD_DIR}/#{ELF_NAME}"
    machine = File.exist?(elf) ? File.binread(elf, 20)[18, 2].unpack1('v') : nil
    abort "build:linux: #{elf} is not an x86-64 ELF (e_machine #{machine.inspect})" unless machine == 0x3E
    stamp.write(CORE_DIR, "ga-linux", LINUX_BUILD_DIR, state: stamp_state)
    puts "ESP-IDF Linux build complete: #{elf}. Start it with the sim tools (sim_up, tools/dev_run_check.sh)."
  end

  desc "ESP32 build"
  # Comm transport: default is UART. To use SPI instead:
  #   CMAKE_OPTS="-DFMRB_COMM_TRANSPORT=SPI" rake build:esp32
  task :esp32 do
    need_core!
    Rake::Task['set_target:esp32'].invoke unless configured_for?("build", "esp32")
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
    CORE_MOUNT,
    IMAGE
  ].join(" ")
  sh "#{docker_cmd_interactive} idf.py menuconfig"
end

desc "Clean the ESP32 build (build/ and sdkconfig); the sim build (build-linux/) stays"
task :clean_all do
  sh "rm -f sdkconfig"
  sh "rm -rf build"
end

desc "Clean ESP32 build artifacts (build/)"
task :clean do
  sh "rm -rf build"
end

namespace :clean do
  desc "Clean the Linux sim build (build-linux/, its sdkconfig inside)"
  task :linux do
    sh "rm -rf #{LINUX_BUILD_DIR}"
  end
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
