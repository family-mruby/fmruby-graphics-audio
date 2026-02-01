# Rakefile — fmruby-graphics-audio ESP-IDF build wrapper (Docker)
require "rake"

USB_SERIAL_PORT="/dev/ttyUSB1"

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

ESP_IDF_VERSION = ENV.fetch("ESP_IDF_VERSION", "v5.5.1")
IMAGE           = ENV.fetch("DOCKER_IMAGE", "ghcr.io/family-mruby/fmruby-esp32-build:latest")

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
    "#{lovyangfx_patch_dir}/Panel_sdl.cpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp"
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
  task :esp32 do
    unless Dir.exist?('build')
      Rake::Task['set_target:esp32'].invoke
    end
    sh "#{DOCKER_CMD} idf.py build"
  end
end

desc "Flash to ESP32"
task :flash do
  sh "#{DOCKER_CMD_PRIVILEGED} idf.py -p #{USB_SERIAL_PORT} flash"
end

desc "Check ESP32 HW"
task :check do
  sh "#{DOCKER_CMD_PRIVILEGED} esptool.py -p #{USB_SERIAL_PORT} flash_id"
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
  sh "#{DOCKER_CMD_INTERACTIVE} idf.py -p #{USB_SERIAL_PORT} monitor"
end

desc "List available tasks"
task :help do
  sh "rake -T"
end

task :default => :help
