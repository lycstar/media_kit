#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint media_kit_libs_macos.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  system("make")

  s.name             = 'media_kit_libs_macos'
  s.version          = '1.0.0'
  s.summary          = 'macOS dependency package for package:media_kit'
  s.description      = <<-DESC
  macOS dependency package for package:media_kit.
                       DESC
  s.homepage         = 'https://github.com/alexmercerind/media_kit.git'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Hitesh Kumar Saini' => 'saini123hitesh@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files     = 'Classes/**/*'
  s.dependency 'FlutterMacOS'

  s.vendored_libraries = 'Libs/*.dylib'

  s.platform = :osx, '10.13'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
