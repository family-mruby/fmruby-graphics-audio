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
IMAGE           = "esp32_build_container:#{ESP_IDF_VERSION}"

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

desc "Apply LovyanGFX patches (file replacement)"
task :apply_patches do
  lovyangfx_dir = "components/LovyanGFX"
  patch_files_dir = "patches/lovyangfx-files"

  unless Dir.exist?(patch_files_dir)
    puts "Warning: Patch files directory not found: #{patch_files_dir}"
    next
  end

  # File mappings: source => destination
  file_mappings = {
    "#{patch_files_dir}/esp-idf.cmake" => "#{lovyangfx_dir}/boards.cmake/esp-idf.cmake",
    "#{patch_files_dir}/common.hpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/common.hpp",
    "#{patch_files_dir}/device.hpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/device.hpp",
    "#{patch_files_dir}/Panel_sdl.cpp" => "#{lovyangfx_dir}/src/lgfx/v1/platforms/sdl/Panel_sdl.cpp"
  }

  puts "Applying LovyanGFX patches (file replacement)..."
  file_mappings.each do |src, dst|
    if File.exist?(src)
      sh "cp #{src} #{dst}"
      puts "  ✓ #{File.basename(src)} -> #{dst}"
    else
      puts "  ✗ Warning: Source file not found: #{src}"
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
  sh "rm -rf host/build"
end

desc "Clean ESP32 build artifacts"
task :clean do
  sh "rm -rf build"
end

desc "Clean Linux native build"
task :clean_linux do
  sh "rm -rf host/build"
end

desc "Serial monitor"
task :monitor do
  sh "#{DOCKER_CMD_INTERACTIVE} idf.py -p #{USB_SERIAL_PORT} monitor"
end

# Removed: host namespace
# Use 'rake build:linux' for native Linux build

# Note: Integration test disabled because host/ directory was removed
# The Linux build now runs standalone without separate host process
# Use 'rake run_linux' to run the graphics-audio service
#
# namespace :test do
#   desc "Integration test: Run both core and host processes"
#   task :integration => ['build:linux', 'host:build'] do
#     puts "Starting integration test..."
#
#     # SDL2ホストをバックグラウンドで起動
#     host_pid = Process.spawn("cd host/sdl2/build && ./fmrb_graphics_audio_host")
#     sleep 2  # 起動待ち
#
#     begin
#       puts "Starting fmruby-graphics-audio..."
#       sh "./build/fmruby-graphics-audio.elf"
#     ensure
#       # 終了処理
#       Process.kill("TERM", host_pid) rescue nil
#       puts "Integration test completed"
#     end
#   end
# end

desc "Run native Linux build"
task :run_linux => 'build:linux' do
  sh "./host/build/fmrb_graphics_audio_host"
end

desc "List available tasks"
task :help do
  sh "rake -T"
end

task :default => :help
