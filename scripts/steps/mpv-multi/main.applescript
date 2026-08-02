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
	set cmd to shellQuote(launcher)

	-- 这个循环原先写成 repeat with anItem in theItems + POSIX path of anItem，
	-- 在某些文件上会抛 -1700：
	--   不能将“quoted form of POSIX path of item 1 of {alias "..."}”转换为
	--   “Unicode text”类型
	-- 按下标取值不再用 repeat...in 的引用绑定；真正的路径提取交给
	-- itemToPosixPath，它会挨个换招试。
	set okCount to 0
	set badCount to 0
	set diagLines to {}
	repeat with i from 1 to (count of theItems)
		set theItem to item i of theItems
		try
			set cmd to cmd & " " & shellQuote(itemToPosixPath(theItem))
			set okCount to okCount + 1
		on error errMsg number errNum
			-- 一个文件解析不出来，不该连累同批选中的其它文件。
			set badCount to badCount + 1
			set end of diagLines to "  第 " & i & " 项 class=" & describeClass(theItem) & " err=" & errNum & " " & errMsg
		end try
	end repeat

	-- 全军覆没时别再往下走，否则会莫名其妙弹出一个空的 mpv 窗口。
	if badCount > 0 and okCount is 0 then
		set diag to joinText(diagLines, linefeed)
		appendLog("路径解析失败" & linefeed & diag)
		display alert "mpv-multi 启动失败" message "无法解析访达传来的路径（" & badCount & " 个文件）。" & linefeed & linefeed & diag & linefeed & linefeed & "同样内容已写入 ~/Library/Logs/mpv-multi.log" as critical
		return
	end if
	if badCount > 0 then appendLog("部分路径解析失败" & linefeed & joinText(diagLines, linefeed))

	try
		do shell script cmd
	on error errMsg number errNum
		-- -128 是用户取消，正常流程，不要弹窗打扰。脚本自己找不到 mpv 时
		-- 已经弹过更具体的提示了，这里兜住的是脚本本身没跑起来的情况
		-- （比如被删了、丢了可执行位）。
		if errNum is not -128 then
			display alert "mpv-multi 启动失败" message errMsg as critical
		end if
		return
	end try

	if badCount > 0 then
		display alert "mpv-multi 有文件没打开" message "有 " & badCount & " 个文件的路径解析失败，已跳过。" as warning
	end if
end runLauncher

-- 替代内置的 quoted form of。内置那个是 text 的属性，作用在没解开的引用上时
-- 会跟着攒进引用链里（见上面 -1700 的注释）；这里全程只跟普通字符串打交道。
--
-- POSIX sh 的单引号串内部没法转义单引号，标准做法是把串断开再接一个转义的：
--   a'b  ->  'a'\''b'
on shellQuote(thePath)
	set thePath to thePath as text
	set savedDelims to AppleScript's text item delimiters
	try
		set AppleScript's text item delimiters to "'"
		set parts to text items of thePath
		set AppleScript's text item delimiters to "'\\''"
		set escaped to parts as text
		set AppleScript's text item delimiters to savedDelims
	on error errMsg number errNum
		-- 分隔符是全局状态，异常路径上也必须还原，否则会污染后续所有字符串操作。
		set AppleScript's text item delimiters to savedDelims
		error errMsg number errNum
	end try
	return "'" & escaped & "'"
end shellQuote

-- 把访达递过来的一项转成 POSIX 路径。
--
-- 为什么不能只用最直白的 POSIX path of（策略 A）：
--
-- 访达经 Apple Event 递过来的 alias，如果它的 file URL 做完百分号编码超过
-- 1024 字符，POSIX path of 就抛 -1700（errAECoercionFail）。实测阈值精确到
-- 单字节：URL 1024 字符正常，1025 字符失败——一个典型的 1024 缓冲区边界。
--
-- 注意这跟字符集无关。emoji、日文、半角片假名本身都没问题，它们只是让
-- UTF-8 字节数膨胀到 3 倍，而百分号编码又把每个字节变成 3 个字符，于是
-- 非 ASCII 字符实际按 9 倍算。一个 208 字符的日文名（524 字节）编码出来
-- 就有 1500 多字符，直接越界；同样 208 个 ASCII 字符则毫无问题。
--
-- 也跟路径深度无关：目录名是 ASCII 时，路径撑到 544 字节照样正常，因为
-- ASCII 不参与编码膨胀。真正决定生死的是编码后的 URL 总长度。
--
-- 策略 B 的 as alias 会强制重新解析一次，绕开这条坏掉的快路径。
on itemToPosixPath(theItem)
	-- A：绝大多数文件走这条。只做零成本的形状检查，这是热路径。
	--
	--    文本项必须排除：POSIX path of 会把任意文本当成 HFS 相对路径，
	--    "不是路径" 直接变成 "/不是路径"，形状检查拦不住。文本项交给 B，
	--    那边的 as alias 会因为文件不存在而干净地报错。
	set aErr to "未执行"
	try
		if class of theItem is text then error number -1700
		set p to (POSIX path of theItem) as text
		if looksLikePath(p) then return p
		set aErr to "形状不对"
	on error number n
		set aErr to "err" & n
	end try

	-- B：URL 超过 1024 字符时 A 必挂。as alias 强制重新解析，绕开它。
	try
		set p to (POSIX path of (theItem as alias)) as text
	on error number n
		error "路径解析失败 [A:" & aErr & " B:err" & n & "]" number -1700
	end try
	if not looksLikePath(p) then
		error "路径解析失败 [A:" & aErr & " B:形状不对]" number -1700
	end if

	noteFallback("B", theItem, aErr)
	return p
end itemToPosixPath

on looksLikePath(p)
	try
		if p is missing value then return false
		set p to p as text
		if p is "" then return false
		return (character 1 of p is "/")
	on error
		return false
	end try
end looksLikePath

-- 只用于诊断：报错时把这项的实际类型带出来（alias / furl / text / ...）。
on describeClass(theItem)
	try
		return (class of theItem) as text
	on error
		return "无法取得"
	end try
end describeClass

on joinText(theList, theSep)
	set savedDelims to AppleScript's text item delimiters
	try
		set AppleScript's text item delimiters to theSep
		set joined to theList as text
		set AppleScript's text item delimiters to savedDelims
	on error errMsg number errNum
		set AppleScript's text item delimiters to savedDelims
		error errMsg number errNum
	end try
	return joined
end joinText

-- 策略 A 没成、靠 B 救回来时记一笔。A 是常态，不记，否则每开一个文件都要
-- 往日志里写一行。
--
-- 留着它有两个用处：一是文件名过长这件事本身值得在日志里留痕，二是将来
-- macOS 更新把策略 A 在别的场景下也搞坏了，日志里会先冒出来，而不是等到
-- 彻底打不开才发现。
--
-- 只写策略字母、item 类型和错误号，不写路径——这东西是要贴出来给人看的。
on noteFallback(tag, theItem, aErr)
	try
		appendLog("回退策略 " & tag & " 生效（class=" & describeClass(theItem) & "，A 失败于 " & aErr & "）")
	on error
		-- 记日志失败绝不能连累已经拿到手的路径。
	end try
end noteFallback

-- 诊断信息落盘，方便事后翻。和 launch-mpv.sh 写的是同一个日志。
on appendLog(theText)
	try
		do shell script "/bin/date '+%Y-%m-%d %H:%M:%S applescript' >> \"$HOME/Library/Logs/mpv-multi.log\"; printf '%s\\n' " & shellQuote(theText) & " >> \"$HOME/Library/Logs/mpv-multi.log\""
	end try
end appendLog
