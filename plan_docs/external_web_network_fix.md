## 无法二次连接
情况描述
在host机器上使用
```sh
cd ~/project/temp 
python -m http.server 18081 --bind 127.0.0.1
```
拉起一个临时页面。
host访问，输出：
```sh

~/project/F7LY global-net *1 ?1                                                                   
❯ wget -O - http://127.0.0.1:18081/
--2026-06-16 17:25:00--  http://127.0.0.1:18081/
正在连接 127.0.0.1:18081... 已连接。
已发出 HTTP 请求，正在等待回应... 200 OK
长度：433 [text/html]
正在保存至: “STDOUT”

-                          0%[                                  ]       0  --.-KB/s               <!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
</head>
<body>
<h1>Directory listing for /</h1>
<hr>
<ul>
<li><a href=".agents/">.agents/</a></li>
<li><a href=".codex/">.codex/</a></li>
<li><a href=".git/">.git/</a></li>
<li><a href=".venv/">.venv/</a></li>
<li><a href="content.md">content.md</a></li>
<li><a href="latex-index/">latex-index/</a></li>
</ul>
<hr>
</body>
</html>
-                        100%[=================================>]     433  --.-KB/s  用时 0s      

2026-06-16 17:25:00 (45.8 MB/s) - 已写入至标准输出 [433/433]

```

在guest（即内核中）使用

```sh
make shell r

# 拉起的内核shell中
wget -O - http://10.0.2.2:18081/
```
访问host页面，结果如下：
```sh
F7LY:~$ wget -O - http://10.0.2.2:18081/
Connecting to 10.0.2.2:18081 (10.0.2.2:18081)
writing to stdout
<!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
</head>
<body>
<h1>Directory listing for /</h1>
<hr>
<ul>
<li><a href=".agents/">.agents/</a></li>
<li><a href=".codex/">.codex/</a></li>
<li><a href=".git/">.git/</a></li>
<li><a href=".venv/">.venv/</a></li>
<li><a href="content.md">content.md</a></li>
<li><a href="latex-index/">latex-index/</a></li>
</ul>
<hr>
</body>
</html>
-                    100% |********************************|   433  0:00:00 ETA
written to stdout
F7LY:~$ wget -O - http://10.0.2.2:18081/
Connecting to 10.0.2.2:18081 (10.0.2.2:18081)

```

问题为第二次访问时会卡在connecting处。怀疑网络协议栈和进程管理存在问题，网络模块未被唤醒。
请在网络和进程pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

## 修复小结

- 现象：第一次 `wget` 打印完 body 后不返回 shell。  
  原因：收到 FIN 后未可靠唤醒阻塞 `recv`，且 `tcp_recv_upper()` 会额外 `pend` 吃掉 EOF 唤醒。  
  解决方案：FIN 到达时投递 input 信号量；删除读到数据后的额外 `pend`；永久等待改为周期等待并复查关闭态。

- 现象：第二次 `wget` 卡在 `Connecting`。  
  原因：TCP link 链表索引用裸 `CHAR` 保存 `-1` 哨兵，RISC-V 下变成 `255`，used 链表遍历越界/成环。  
  解决方案：TCP/UDP link 的 `bIdx/bNext` 改为明确 `signed char`，保留遍历上限防御。

- 现象：用户 close 路径可能卡在 TCP link 回收。  
  原因：close 同步释放会和 one-shot timer/TCP link 全局链表形成锁竞争。  
  解决方案：FIN/TIMEWAIT/CLOSED 态 close 快速返回，实际 input/link 回收交给 close timer。

- 现象：input 释放时存在锁嵌套风险。  
  原因：原逻辑在 input 全局锁内释放 TCP link、buffer、sem。  
  解决方案：锁内只摘除 input 并取走资源指针，锁外释放附属资源。

- 验证：`make build ARCH=riscv`、`make build ARCH=loongarch` 通过；RISC-V guest 连续两次 `wget -O - http://10.0.2.2:18081/` 均输出 `written to stdout` 并返回 shell。

## 长网页connection closed
情况描述
在host机器上使用
```sh
cd /tmp
python -m http.server 18081 --bind 127.0.0.1
```
拉起一个临时页面。
host访问，输出：
```sh
~/project/F7LY global-net *1 !1                                                              ✘ INT
❯ wget -O - http://127.0.0.1:18081/
--2026-06-16 20:13:04--  http://127.0.0.1:18081/
正在连接 127.0.0.1:18081... 已连接。
已发出 HTTP 请求，正在等待回应... 200 OK
长度：4224 (4.1K) [text/html]
正在保存至: “STDOUT”

-                          0%[                                  ]       0  --.-KB/s               <!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
</head>
<body>
<h1>Directory listing for /</h1>
<hr>
<ul>
<li><a href=".font-unix/">.font-unix/</a></li>
<li><a href=".ICE-unix/">.ICE-unix/</a></li>
<li><a href=".mount_SnipasxHlUlV/">.mount_SnipasxHlUlV/</a></li>
<li><a href=".ses">.ses</a></li>
<li><a href=".X1-lock">.X1-lock</a></li>
<li><a href=".X11-unix/">.X11-unix/</a></li>
<li><a href=".XIM-unix/">.XIM-unix/</a></li>
<li><a href="claude-1000/">claude-1000/</a></li>
<li><a href="codex-bwrap-synthetic-mount-targets-1000/">codex-bwrap-synthetic-mount-targets-1000/</a></li>
<li><a href="codex-clipboard-kHaBgk.png">codex-clipboard-kHaBgk.png</a></li>
<li><a href="codex-ipc/">codex-ipc/</a></li>
<li><a href="com.microsoft.Edge.PDPMFL/">com.microsoft.Edge.PDPMFL/</a></li>
<li><a href="cv_debug.log">cv_debug.log</a></li>
<li><a href="f7ly_http_18081.log">f7ly_http_18081.log</a></li>
<li><a href="f7ly_http_root/">f7ly_http_root/</a></li>
<li><a href="hsperfdata_kidszz/">hsperfdata_kidszz/</a></li>
<li><a href="kidszz-code-zsh/">kidszz-code-zsh/</a></li>
<li><a href="mcp-npVTsa/">mcp-npVTsa/</a></li>
<li><a href="mcp-o3D18h/">mcp-o3D18h/</a></li>
<li><a href="mcp-RPmDJa/">mcp-RPmDJa/</a></li>
<li><a href="mcp-t7zlNr/">mcp-t7zlNr/</a></li>
<li><a href="node-compile-cache/">node-compile-cache/</a></li>
<li><a href="NutstoreTmp0xyz/">NutstoreTmp0xyz/</a></li>
<li><a href="plasma-csd-generator.PwceUn/">plasma-csd-generator.PwceUn/</a></li>
<li><a href="pyright-244561-ll5SjSFCR856/">pyright-244561-ll5SjSFCR856/</a></li>
<li><a href="python-languageserver-cancellation/">python-languageserver-cancellation/</a></li>
<li><a href="qipc_sharedmemory_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279">qipc_sharedmemory_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279</a></li>
<li><a href="qipc_systemsem_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279">qipc_systemsem_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279</a></li>
<li><a href="scoped_dirnC15du/">scoped_dirnC15du/</a></li>
<li><a href="sddm-%3A0-sCVIFc">sddm-:0-sCVIFc</a></li>
<li><a href="sddm-auth-dcf74044-d8d9-4d73-a565-645e17e480f7">sddm-auth-dcf74044-d8d9-4d73-a565-645e17e480f7</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-bluetooth.service-3c8pjQ/">systemd-private-391f198f52f541dda1ea650b029b45e9-bluetooth.service-3c8pjQ/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-fwupd.service-Z4Vdp4/">systemd-private-391f198f52f541dda1ea650b029b45e9-fwupd.service-Z4Vdp4/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-lm-sensors.service-BTBffW/">systemd-private-391f198f52f541dda1ea650b029b45e9-lm-sensors.service-BTBffW/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-ModemManager.service-YSmCC7/">systemd-private-391f198f52f541dda1ea650b029b45e9-ModemManager.service-YSmCC7/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-polkit.service-JMqBHg/">systemd-private-391f198f52f541dda1ea650b029b45e9-polkit.service-JMqBHg/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-switcheroo-control.service-KXEc6z/">systemd-private-391f198f52f541dda1ea650b029b45e9-switcheroo-control.service-KXEc6z/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-systemd-logind.service-nOYj7P/">systemd-private-391f198f52f541dda1ea650b029b45e9-systemd-logind.service-nOYj7P/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-upower.service-fCp3z1/">systemd-private-391f198f52f541dda1ea650b029b45e9-upower.service-fCp3z1/</a></li>
<li><a href="ucp-c1476a74b1b1ca4c04748214.sock">ucp-c1476a74b1b1ca4c04748214.sock</a></li>
<li><a href="UT54j2cDB3P_hkKsL5CFHQx_jIyvwhUeeL1hFHxJOjY">UT54j2cDB3P_hkKsL5CFHQx_jIyvwhUeeL1hFHxJOjY</a></li>
<li><a href="verge/">verge/</a></li>
<li><a href="vscode-typescript1000/">vscode-typescript1000/</a></li>
<li><a href="wmpf_images/">wmpf_images/</a></li>
<li><a href="%7B6ABCA629-A78D-4E25-8A52-CB69D316F315%7D/">{6ABCA629-A78D-4E25-8A52-CB69D316F315}/</a></li>
</ul>
<hr>
</body>
</html>
-                        100%[=================================>]   4.12K  --.-KB/s  用时 0s      

2026-06-16 20:13:04 (173 MB/s) - 已写入至标准输出 [4224/4224]


```

在guest（即内核中）使用

```sh
make shell r

# 拉起的内核shell中
wget -O - http://10.0.2.2:18081/
```
访问host页面，结果如下：
```sh
F7LY:~$ wget -O - http://10.0.2.2:18081
Connecting to 10.0.2.2:18081 (10.0.2.2:18081)
writing to stdout
<!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Directory listing for /</title>
</head>
<body>
<h1>Directory listing for /</h1>
<hr>
<ul>
<li><a href=".font-unix/">.font-unix/</a></li>
<li><a href=".ICE-unix/">.ICE-unix/</a></li>
<li><a href=".mount_SnipasxHlUlV/">.mount_SnipasxHlUlV/</a></li>
<li><a href=".ses">.ses</a></li>
<li><a href=".X1-lock">.X1-lock</a></li>
<li><a href=".X11-unix/">.X11-unix/</a></li>
<li><a href=".XIM-unix/">.XIM-unix/</a></li>
<li><a href="claude-1000/">claude-1000/</a></li>
<li><a href="codex-bwrap-synthetic-mount-targets-1000/">codex-bwrap-synthetic-mount-targets-1000/</a></li>
<li><a href="codex-clipboard-kHaBgk.png">codex-clipboard-kHaBgk.png</a></li>
<li><a href="codex-ipc/">codex-ipc/</a></li>
<li><a href="com.microsoft.Edge.PDPMFL/">com.microsoft.Edge.PDPMFL/</a></li>
<li><a href="cv_debug.log">cv_debug.log</a></li>
<li><a href="f7ly_http_18081.log">f7ly_http_18081.log</a></li>
<li><a href="f7ly_http_root/">f7ly_http_root/</a></li>
<li><a href="hsperfdata_kidszz/">hsperfdata_kidszz/</a></li>
<li><a href="kidszz-code-zsh/">kidszz-code-zsh/</a></li>
<li><a href="mcp-npVTsa/">mcp-npVTsa/</a></li>
<li><a href="mcp-o3D18h/">mcp-o3D18h/</a></li>
<li><a href="mcp-RPmDJa/">mcp-RPmDJa/</a></li>
-                     30% |*********                       |  1284  0:00:02 ETAr/">mcp-t7zlNr/</a></li>
<li><a href="node-compile-cache/">node-compile-cache/</a></li>
<li><a href="NutstoreTmp0xyz/">NutstoreTmp0xyz/</a></li>
<li><a href="plasma-csd-generator.PwceUn/">plasma-csd-generator.PwceUn/</a></li>
<li><a href="pyright-244561-ll5SjSFCR856/">pyright-244561-ll5SjSFCR856/</a></li>
<li><a href="python-languageserver-cancellation/">python-languageserver-cancellation/</a></li>
<li><a href="qipc_sharedmemory_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279">qipc_sharedmemory_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279</a></li>
<li><a href="qipc_systemsem_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279">qipc_systemsem_UTjcDBPhkKsLCFHQxjIyvwhUeeLhFHxJOjY62a774b42e8c2539fe326854a8ef83cb078f8279</a></li>
<li><a href="scoped_dirnC15du/">scoped_dirnC15du/</a></li>
<li><a href="sddm-%3A0-sCVIFc">sddm-:0-sCVIFc</a></li>
<li><a href="sddm-auth-dcf74044-d8d9-4d73-a565-645e17e480f7">sddm-auth-dcf74044-d8d9-4d73-a565-645e17e480f7</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-bluetooth.service-3c8pjQ/">systemd-private-391f198f52f541dda1ea650b029b45e9-bluetooth.service-3c8pjQ/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-fwupd.service-Z4Vdp4/">systemd-private-391f198f52f541dda1ea650b029b45e9-fwupd.service-Z4Vdp4/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-lm-sensors.service-BTBffW/">systemd-private-391f198f52f541dda1ea650b029b45e9-lm-sensors.service-BTBffW/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-ModemManager.service-YSmCC7/">systemd-private-391f198f52f541dda1ea650b029b45e9-ModemManager.service-YSmCC7/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-polkit.service-JMqBHg/">systemd-private-391f198f52f541dda1ea650b029b45e9-polkit.service-JMqBHg/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-switcheroo-control.service-KXEc6z/">systemd-private-391f198f52f541dda1ea650b029b45e9-switcheroo-control.service-KXEc6z/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-systemd-logind.service-nOYj7P/">systemd-private-391f198f52f541dda1ea650b029b45e9-systemd-logind.service-nOYj7P/</a></li>
<li><a href="systemd-private-391f198f52f541dda1ea650b029b45e9-upower.service-fCp3z1/">systemd-private-391f198f52f541dda1ea650b029b45e9-upower.service-fCp3z1/</a></li>
<li><a href="ucp-c1476a74b1b1ca4c04748214.sock">ucp-c1476a74b1b1ca4c04748214.sock</a></li>
<li><a href="UT54j2cDB3P_hkKsL5CFHQx_jIyvwhUeeL1hFHxJOjY">UT54j2cDB3P_hkKsL5CFHQx_jIyvwhUeeL1hFHxJOjY</a></li>
<li><a href="verge/">verge/</a></li>
<li><a href="vscode-typescript1000/">vscode-typescript1000/</a></li>
<li><a href="wmpf_images/">wmpf_images/</a></li>
<li><a href="%7B6ABCA629-A78D-4E25-8A52-CB69D316F315%7D/">{6ABCA629-A78D-4wget: connection closed prematurely


```

问题为长网页（比如在host/tmp中拉起python服务器），进行wget时无法正确获取所有信息，在运行一段时间后发生wget: connection closed prematurely。怀疑协议栈实现未符合标准语义。
请在网络pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

参考实现：
1. ref/rocketos（往届作品第一名）

## 长网页修复小结

- 现象：长页面 `wget` 约 30% 后报 `connection closed prematurely`。  
  原因：TCP `FIN` 包可能同时携带最后一段 payload，旧逻辑先处理 FIN，导致尾部数据未交付就 EOF。  
  解决方案：FIN 分支先接收 payload；payload 全部缓存后再 ACK FIN/进入 EOF。

- 现象：响应超过 `TCPRCVBUF_SIZE=2048` 后容易卡顿或截断。  
  原因：上层读走数据后只更新本地接收窗口，未主动发送 window update ACK。  
  解决方案：`tcp_recv_upper()` 每次读出数据后发送 ACK，通知 peer 新窗口。

- 现象：接收窗口满时不能完整接收 `payload+FIN`。  
  原因：若提前 ACK FIN，会把未缓存数据误确认。  
  解决方案：只 ACK 已缓存字节；窗口恢复后等待对端重传剩余 payload+FIN。
