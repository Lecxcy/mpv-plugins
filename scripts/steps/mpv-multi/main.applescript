-- mpv-multi.app 的壳。真正的逻辑在 Contents/Resources/launch-mpv.sh，这里
-- 只负责一件事：把访达传来的文件转成参数交给那个脚本。
--
-- 为什么非得有这层 AppleScript：macOS 只把"打开文件"投递成 Apple Event
-- （odoc），bundle 里放个纯 shell 脚本是收不到的——双击文件会启动 app，但
-- 脚本拿不到任何参数。AppleScript 的 on open 处理器是接住这个事件最省事的
-- 办法，osacompile 一条命令就能编译，不需要 Xcode，也不需要像老版本那样
-- 靠 Automator 的图形界面去改。
--
-- 由 scripts/make-mpv-multi-app.sh 编译，不要直接改编译产物。

on run
	-- 双击 app 本身（没有文件），开一个空的 mpv 待机窗口。
	runLauncher({})
end run

on open theItems
	-- 访达里双击视频、拖文件到图标、或 open -a 都会走这里。多选时
	-- theItems 是一个列表，一次性传给脚本，由它给每个文件起一个进程。
	runLauncher(theItems)
end open

on runLauncher(theItems)
	-- path to me 得到的是 app 自身的位置，所以 app 整个挪到别的目录也照样
	-- 找得到内部脚本，不存在写死路径的问题。POSIX path 末尾自带斜杠。
	set launcher to (POSIX path of (path to me)) & "Contents/Resources/launch-mpv.sh"
	set cmd to quoted form of launcher

	repeat with anItem in theItems
		set cmd to cmd & " " & quoted form of (POSIX path of anItem)
	end repeat

	try
		do shell script cmd
	on error errMsg number errNum
		-- -128 是用户取消，正常流程，不要弹窗打扰。脚本自己找不到 mpv 时
		-- 已经弹过更具体的提示了，这里兜住的是脚本本身没跑起来的情况
		-- （比如被删了、丢了可执行位）。
		if errNum is not -128 then
			display alert "mpv-multi 启动失败" message errMsg as critical
		end if
	end try
end runLauncher
