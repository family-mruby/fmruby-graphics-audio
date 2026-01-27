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

desc "Apply LovyanGFX patches"
task :apply_patches do
  patch_file = "patches/lovyangfx-esp-idf-linux-support.patch"
  lovyangfx_dir = "components/LovyanGFX"

  if File.exist?(patch_file)
    # Check if patch is already applied
    sh "cd #{lovyangfx_dir} && git diff --quiet || git diff --quiet --cached", verbose: false do |ok, res|
      if ok
        puts "Applying LovyanGFX patches..."
        sh "cd #{lovyangfx_dir} && git apply ../../#{patch_file}"
        puts "Patches applied successfully"
      else
        puts "Patches already applied or LovyanGFX has local changes"
      end
    end
  else
    puts "Warning: Patch file not found: #{patch_file}"
  end
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
  desc "Linux target build (dev/test)"
  task :linux do
    # Note: host/sdl2 is now integrated into main/, no separate build needed

    unless Dir.exist?('build')
      Rake::Task['set_target:linux'].invoke
    end
    sh "#{DOCKER_CMD} bash -c 'export IDF_TARGET=linux && idf.py --preview -DCMAKE_BUILD_TYPE=Debug build'"
    puts 'Linux build complete. Run with: ./build/fmruby-graphics-audio.elf'
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

desc "Clean build artifacts"
task :clean do
  sh "rm -rf build"
end

desc "Serial monitor"
task :monitor do
  sh "#{DOCKER_CMD_INTERACTIVE} idf.py -p #{USB_SERIAL_PORT} monitor"
end

namespace :host do
  desc "Build SDL2 host process"
  task :build do
    sh "cd host/sdl2 && mkdir -p build && cd build && cmake .. && make"
  end

  desc "Run SDL2 host process in background"
  task :run => :build do
    puts "Starting SDL2 host process..."
    sh "cd host/sdl2/build && ./fmrb_graphics_audio_host &"
    sleep 1
    puts "SDL2 host running"
  end

  desc "Clean SDL2 host build"
  task :clean do
    sh "rm -rf host/sdl2/build"
  end
end

namespace :test do
  desc "Integration test: Run both core and host processes"
  task :integration => ['build:linux', 'host:build'] do
    puts "Starting integration test..."

    # SDL2ホストをバックグラウンドで起動
    host_pid = Process.spawn("cd host/sdl2/build && ./fmrb_graphics_audio_host")
    sleep 2  # 起動待ち

    begin
      puts "Starting fmruby-graphics-audio..."
      sh "./build/fmruby-graphics-audio.elf"
    ensure
      # 終了処理
      Process.kill("TERM", host_pid) rescue nil
      puts "Integration test completed"
    end
  end
end

desc "Run Linux build (depends on build:linux)"
task :run_linux => 'build:linux' do
  sh "./build/fmruby-graphics-audio.elf"
end

desc "List available tasks"
task :help do
  sh "rake -T"
end

task :default => :help
