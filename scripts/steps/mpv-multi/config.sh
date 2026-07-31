# mpv-multi 的公共常量，由 make-mpv-multi-app.sh 和 set-video-handlers.sh
# 共同 source。单独抽出来是因为 bundle id 一旦两边不一致，症状是"构建成功、
# 绑定也成功，但双击视频还是用旧 app 打开"，很难查。

MPV_MULTI_BUNDLE_ID="com.lecxcy.mpv-multi"
MPV_MULTI_APP_PATH="${HOME}/Applications/mpv-multi.app"
