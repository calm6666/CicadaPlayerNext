#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint flutter_cicadaplayer.podspec' to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'flutter_cicadaplayer'
  s.version          = '0.0.1'
  s.summary          = 'A new flutter plugin project.'
  s.description      = <<-DESC
A new flutter plugin project.
                       DESC
  s.homepage         = 'http://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }
  s.source           = { :path => '.' }
  # Classes/**/* 已经把新增的 FlutterCicadaPlayerTexture.{h,m}（iOS 零拷贝纹理）一起编进来。
  s.source_files = 'Classes/**/*'
  s.public_header_files = 'Classes/**/*.h'
  s.vendored_frameworks = 'IOS_SDK/SDK/ARM_SIMULATOR/*.framework'
  s.dependency 'Flutter'
#  s.dependency 'AliPlayerSDK_iOS'
  s.dependency 'MJExtension'
  # Flutter 3.x（本工程 3.47）的 iOS 插件最低部署版本已经抬到 12.0；原来写 8.0 会和
  # Flutter 的 podhelper / app 侧 Podfile 的 platform 打架（pod install 阶段就报错）。
  s.platform = :ios, '12.0'

  # Flutter.framework 不含 i386 切片，模拟器只支持 x86_64。
  # VALID_ARCHS 在 Xcode 12+ 已废弃（会报 warning，pod 校验甚至当错误），改成
  # EXCLUDED_ARCHS：模拟器上排掉 arm64（Intel Mac 上的 Flutter 模拟器只能用 x86_64）。
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'arm64 i386' }
end
