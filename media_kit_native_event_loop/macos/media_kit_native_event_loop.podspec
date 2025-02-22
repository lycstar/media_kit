#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint media_kit_native_event_loop.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'media_kit_native_event_loop'
  s.version          = '1.0.0'
  s.summary          = 'Platform specific threaded event handling for media_kit.'
  s.description      = <<-DESC
  Platform specific threaded event handling for media_kit.
                       DESC
  s.homepage         = 'https://github.com/alexmercerind/media_kit.git'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Hitesh Kumar Saini' => 'saini123hitesh@gmail.com' }

  # This will ensure the source files in Classes/ are included in the native
  # builds of apps using this FFI plugin. Podspec does not support relative
  # paths, so Classes contains a forwarder C file that relatively imports
  # `../src/*` so that the C sources can be shared among all target platforms.
  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'GCC_WARN_INHIBIT_ALL_WARNINGS' => 'YES',
    'HEADER_SEARCH_PATHS' => '"$(inherited)" "$(PROJECT_DIR)/../Flutter/ephemeral/.symlinks/plugins/media_kit_libs_macos/macos/Headers"',
    'LIBRARY_SEARCH_PATHS' => '"$(inherited)" "$(PROJECT_DIR)/../Flutter/ephemeral/.symlinks/plugins/media_kit_libs_macos/macos/Libs"',
    'OTHER_LDFLAGS' => '"$(inherited)" -lmpv',
  }
  s.swift_version = '5.0'
end
