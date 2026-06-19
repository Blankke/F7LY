## 无法二次连接
### 情况描述
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

### 修复小结

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
### 情况描述
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

### 长网页修复小结

- 现象：长页面 `wget` 约 30% 后报 `connection closed prematurely`。  
  原因：TCP `FIN` 包可能同时携带最后一段 payload，旧逻辑先处理 FIN，导致尾部数据未交付就 EOF。  
  解决方案：FIN 分支先接收 payload；payload 全部缓存后再 ACK FIN/进入 EOF。

- 现象：响应超过 `TCPRCVBUF_SIZE=2048` 后容易卡顿或截断。  
  原因：上层读走数据后只更新本地接收窗口，未主动发送 window update ACK。  
  解决方案：`tcp_recv_upper()` 每次读出数据后发送 ACK，通知 peer 新窗口。

- 现象：接收窗口满时不能完整接收 `payload+FIN`。  
  原因：若提前 ACK FIN，会把未缓存数据误确认。  
  解决方案：只 ACK 已缓存字节；窗口恢复后等待对端重传剩余 payload+FIN。

## ping发生dup
### 情况描述
使用ping指令ping ip时（无论哪个ip都一样，以下给出ping 10.0.2.2时的情况），发生dup！提示
```sh
F7LY:~$ ping 10.0.2.2
PING 10.0.2.2 (10.0.2.2): 56 data bytes
64 bytes from 10.0.2.2: seq=0 ttl=255 time=22.163 ms
64 bytes from 10.0.2.2: seq=0 ttl=255 time=1006.451 ms (DUP!)
64 bytes from 10.0.2.2: seq=1 ttl=255 time=9.593 ms
64 bytes from 10.0.2.2: seq=1 ttl=255 time=1005.141 ms (DUP!)
64 bytes from 10.0.2.2: seq=2 ttl=255 time=10.287 ms
64 bytes from 10.0.2.2: seq=2 ttl=255 time=1005.468 ms (DUP!)
64 bytes from 10.0.2.2: seq=3 ttl=255 time=10.277 ms
64 bytes from 10.0.2.2: seq=3 ttl=255 time=1004.614 ms (DUP!)
64 bytes from 10.0.2.2: seq=4 ttl=255 time=10.307 ms
64 bytes from 10.0.2.2: seq=4 ttl=255 time=1005.861 ms (DUP!)
```
怀疑raw mode实现有问题
请在网络pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

参考实现：
1. ref/rocketos（往届作品第一名）

### ping DUP 修复小结

- 现象：`ping` 每个 seq 约 1 秒后重复显示 `(DUP!)`。
  原因：ICMP 固定接收缓冲读出后未标记消费，下一次唤醒再次返回旧 echo reply。
  解决方案：`onps_input_recv_icmp()` 返回前清 `unRcvedBytes`，空缓冲唤醒直接返回无数据。


## dns解析时大量resource temporarily unavailable
### 情况描述
在进行dns解析测试的时候会出现大量`nslookup: read: Resource temporarily unavailable`，然后拿到dns解析结果
```sh
F7LY:~$ nslookup example.com 10.0.2.3
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
...(省略大量nslookup: read: Resource temporarily unavailable)
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
nslookup: read: Resource temporarily unavailable
Server:         10.0.2.3
Address:        10.0.2.3:53

Non-authoritative answer:
Name:   example.com
Address: 172.66.147.243
Name:   example.com
Address: 104.20.23.154

Non-authoritative answer:
Name:   example.com
Address: 2606:4700:10::6814:179a
Name:   example.com
Address: 2606:4700:10::ac42:93f3
```
怀疑系统调用不是标准行为/dns解析pipeline存在问题
请在网络pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

参考实现：
1. ref/rocketos（往届作品第一名）

### DNS EAGAIN 修复小结

- 现象：`nslookup` 解析成功前大量打印 `Resource temporarily unavailable`。
  原因：ONPS UDP socket 无报文时也被 `poll/select` 判为可读，非阻塞 `read/recvfrom` 被空唤醒后返回 `EAGAIN`。
  解决方案：新增 `onps_input_has_pending_data()`；ONPS UDP 仅在接收队列有报文时读就绪，TCP 保留数据/EOF 就绪语义。
  验证：`nslookup example.com 10.0.2.3` 返回 A/AAAA，未再出现 `Resource temporarily unavailable`。


## dns解析正常，但无法直接ping
### 情况描述
以百度作为实验对象，发现dns解析正常，直接ping解析出来的ip也能正常返回，但是直接使用ping baidu.com则会直接卡死
```sh
nslookup baidu.com 10.0.2.3
Server:         10.0.2.3
Address:        10.0.2.3:53

Non-authoritative answer:
Name:   baidu.com
Address: 124.237.177.164
Name:   baidu.com
Address: 110.242.74.102
Name:   baidu.com
Address: 111.63.65.247
Name:   baidu.com
Address: 111.63.65.103

Non-authoritative answer:

F7LY:~$ ping 124.237.177.164
PING 124.237.177.164 (124.237.177.164): 56 data bytes
64 bytes from 124.237.177.164: seq=0 ttl=255 time=29.683 ms
64 bytes from 124.237.177.164: seq=1 ttl=255 time=31.024 ms
64 bytes from 124.237.177.164: seq=2 ttl=255 time=30.810 ms
64 bytes from 124.237.177.164: seq=3 ttl=255 time=30.730 ms
（因为当前终端对信号输入的处理有问题，因此杀掉原终端后新开了一个，这个可以暂时不修）

F7LY:~$ ping baidu.com
(卡死)

```

怀疑系统调用不是标准行为/dns->ip pipeline存在问题
请在网络pipeline中添加注释，排查该问题。
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码

参考实现：
1. ref/rocketos（往届作品第一名）

### 域名 ping修复小结

- 现象：`ping/wget baidu.com` 从卡死变为 `bad address`，直接 IP 正常。原因：`ppoll` 用 `CLOCKS_PER_SEC` 换算内核 tick，resolver 超时被放大。解决方案：`ppoll` 改用真实微秒 deadline。

- 现象：DNS 回包已进 ONPS UDP input，但 `poll/recv` 看不到。原因：INADDR_ANY UDP 同时注册 loopback/ONPS，旧逻辑只查 loopback 队列。解决方案：UDP 就绪同时检查 loopback 和 ONPS；ONPS 有数据时优先读取。
- 现象：未显式 bind 的 DNS UDP socket 收不到回包。原因：ONPS `udp_sendto()` 自动分配端口后，socket 层未同步 `_onps_bound`。解决方案：ONPS UDP 外发成功后同步 `_onps_bound` 和本地地址。

## 网络buffer异常耗尽(优化)
### 情况描述
在多次成功`wget`后,会出现如下输出:
```sh
...(成功 wget 3次)
F7LY:~$ wget -O - http://httpbin.org
Connecting to httpbin.org (35.153.186.200:80)
wget: can't connect to remote host (35.153.186.200): No buffer space available
```
请确定该输出是否符合标准实现.

### 修复小结

- 现象：快速多次 `wget` 后 `No buffer space available`。原因：TCP 关闭态连接仍保留 2KB 接收缓冲 + 4KB SACK 发送缓冲，32KB ONPS buddy 池短时耗尽。解决方案：用户 fd close 后，FIN/TIMEWAIT/CLOSED 只保留 TCP link/timer，提前释放收发大缓冲。


## wget invalid argument(无法复现,暂时搁置)
### 情况描述
我找了一个http协议的公网网页(http://httpbin.org), 使用wget进行多次实验
```sh
F7LY:~$ wget -O - http://httpbin.org
Connecting to httpbin.org (35.153.186.200:80)
writing to stdout
<!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <title>httpbin.org</title>
    <link href="https://fonts.googleapis.com/css?family=Open+Sans:400,700|Source+Code+Pro:300,600|Titillium+Web:400,600,700"
        rel="stylesheet">
    <link rel="stylesheet" type="text/css" href="/flasgger_static/swagger-ui.css">
    <link rel="icon" type="image/png" href="/static/favicon.ico" sizes="64x64 32x32 16x16" />
    <style>
        html {

...(中间省略)
                <br />
            </div>
        </section>
    </div>
</div>
</body>

-                    100% |********************************|  9593  0:00:00 ETA
written to stdout
F7LY:~$ wget -O - http://httpbin.org
Connecting to httpbin.org (35.153.186.200:80)
wget: error getting response: Invalid argument

```
宿主机执行效果
```sh
~/project/F7LY global-net ⇡1 *1 !1
❯ wget -O - http://httpbin.org
--2026-06-17 00:00:31--  http://httpbin.org/
正在解析主机 httpbin.org (httpbin.org)... 44.208.196.123, 44.217.243.132, 35.153.186.200, ...
正在连接 httpbin.org (httpbin.org)|44.208.196.123|:80... 已连接。
已发出 HTTP 请求，正在等待回应... 200 OK
长度：9593 (9.4K) [text/html]
正在保存至: “STDOUT”

-                          0%[                                  ]       0  --.-KB/s               <!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <title>httpbin.org</title>
    <link href="https://fonts.googleapis.com/css?family=Open+Sans:400,700|Source+Code+Pro:300,600|Titillium+Web:400,600,700"
        rel="stylesheet">
    <link rel="stylesheet" type="text/css" href="/flasgger_static/swagger-ui.css">
    <link rel="icon" type="image/png" href="/static/favicon.ico" sizes="64x64 32x32 16x16" />
    <style>
...(中间省略)
                <br />
                <br />
            </div>
        </section>
    </div>
</div>
</body>

-                        100%[=================================>]   9.37K  --.-KB/s  用时 0s

2026-06-17 00:00:33 (332 MB/s) - 已写入至标准输出 [9593/9593]

```
发现os在执行网络请求中出现了偶发性的不一致现象. `wget: error getting response: Invalid argument`. 请通过在链路上打印调试输出,定位这个问题出现的原因,按照下面的步骤
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码
参考实现：
1. ref/rocketos（往届作品第一名）


## https协议未适配
### 情况描述
对于同一公网测试网站:"httpbin.org",使用http协议和https协议连接得到结果不同.
```sh
F7LY:~$ wget -O - https://httpbin.org
Connecting to httpbin.org (44.217.243.132:443)
ssl_client: write: Broken pipe
wget: error getting response: Invalid argument
F7LY:~$ wget -O - https://httpbin.org
Connecting to httpbin.org (32.196.248.58:443)
ssl_client: write: Broken pipe
wget: error getting response: Resource temporarily unavailable
F7LY:~$ wget -O - http://httpbin.org
Connecting to httpbin.org (44.217.243.132:80)
writing to stdout
<!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <title>httpbin.org</title>
    <link href="https://fonts.googleapis.com/css?family=Open+Sans:400,700|Source+Code+Pro:300,600|Titillium+Web:400,600,700"
        rel="stylesheet">
...(中间部分省略)
    </div>
</div>
</body>

-                    100% |********************************|  9593  0:00:00 ETA
written to stdout
```
host上连接结果如下:
```sh
❯ wget -O - http://httpbin.org
--2026-06-17 10:25:19--  http://httpbin.org/
正在解析主机 httpbin.org (httpbin.org)... 98.84.2.93, 35.153.186.200, 52.202.201.157, ...
正在连接 httpbin.org (httpbin.org)|98.84.2.93|:80... 已连接。
已发出 HTTP 请求，正在等待回应... 200 OK
长度：9593 (9.4K) [text/html]
正在保存至: “STDOUT”

-                                                    0%[                                                                                                               ]       0  --.-KB/s               <!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <title>httpbin.org</title>
...(中间部分省略)
    </div>
</div>
</body>

-                                                  100%[==============================================================================================================>]   9.37K  --.-KB/s  用时 0s      

2026-06-17 10:25:19 (375 MB/s) - 已写入至标准输出 [9593/9593]


~                                                                                                                                                                                                        
❯ wget -O - https://httpbin.org
--2026-06-17 10:25:25--  https://httpbin.org/
正在解析主机 httpbin.org (httpbin.org)... 98.84.2.93, 35.153.186.200, 52.202.201.157, ...
正在连接 httpbin.org (httpbin.org)|98.84.2.93|:443... 已连接。
已发出 HTTP 请求，正在等待回应... 200 OK
长度：9593 (9.4K) [text/html]
正在保存至: “STDOUT”

-                                                    0%[                                                                                                               ]       0  --.-KB/s               <!DOCTYPE html>
<html lang="en">

<head>
    <meta charset="UTF-8">
    <title>httpbin.org</title>
...(中间部分省略)    
    </div>
</div>
</body>

-                                                  100%[==============================================================================================================>]   9.37K  --.-KB/s  用时 0s      

2026-06-17 10:25:26 (195 MB/s) - 已写入至标准输出 [9593/9593]
```
我在host中搭建了一个https server，方法如下：
```sh
~/project/temp
❯ mkdir -p /tmp/f7ly_https                                 

openssl req -x509 -newkey rsa:2048 \
  -keyout /tmp/f7ly_https/key.pem \
  -out /tmp/f7ly_https/cert.pem \
  -days 1 -nodes \
  -subj "/CN=localhost"

openssl s_server \
  -quiet -WWW \
  -accept 18443 \
  -cert /tmp/f7ly_https/cert.pem \
  -key /tmp/f7ly_https/key.pem
```
~/project/temp中已经创建了一个index.html,在host中使用wget访问结果如下：
```sh
~ 
❯ wget --no-check-certificate -O - https://127.0.0.1:18443/index.html
--2026-06-17 11:06:39--  https://127.0.0.1:18443/index.html
正在连接 127.0.0.1:18443... 已连接。
警告: “127.0.0.1” 的证书不可信。
警告: “127.0.0.1” 的证书颁发者未知。
证书所有者与主机名 “127.0.0.1” 不符
已发出 HTTP 请求，正在等待回应... 200 ok
长度：未指定 [text/html]
正在保存至: “STDOUT”

-                                                      [<=>                                                                                                            ]       0  --.-KB/s               hello https
-                                                      [ <=>                                                                                                           ]      12  --.-KB/s  用时 0s      

2026-06-17 11:06:39 (5.85 MB/s) - 已写入标准输出 [12]

```
你可以使用这台server进行调试

怀疑是协议栈未适配https(ssl)协议.请通过在链路上打印调试输出,定位这个问题出现的原因,按照下面的步骤
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码
参考实现：
1. ref/rocketos（往届作品第一名）

### 修复小结

- 现象：HTTPS `wget` 报 `ssl_client: write: Broken pipe`。  
  原因：`wget` 对本地 `socketpair` 执行 `shutdown(SHUT_WR)` 后，内核误标记对端已关闭。  
  解决方案：拆分 `_peer_closed` 与 `_peer_write_shutdown`；半关闭只让读端 EOF，不阻止对端写回响应。

## 多次复测https连接后产生异常
### 情况描述
多次使用`wget --no-check-certificate -O - https://10.0.2.2:18443/index.html`获取主机上的index.html，发现出现如下报错：
```sh
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
writing to stdout
hello https
-                    100% |********************************|    12  0:00:00 ETA
written to stdout
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
writing to stdout
hello https
-                    100% |********************************|    12  0:00:00 ETA
written to stdout
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
wget: error getting response
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
writing to stdout
hello https
-                    100% |********************************|    12  0:00:00 ETA
written to stdout
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
writing to stdout
hello https
-                    100% |********************************|    12  0:00:00 ETA
written to stdout
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
D0BC0E0000000000:error:0A000126:SSL routines::unexpected eof while reading:ssl/record/rec_layer_s3.c:698:
ssl_client: SSL_connect
wget: error getting response
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
wget: can't connect to remote host (10.0.2.2): No buffer space available
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
wget: can't connect to remote host (10.0.2.2): No buffer space available
F7LY:~$ wget --no-check-certificate -O - https://10.0.2.2:18443/index.html
Connecting to 10.0.2.2:18443 (10.0.2.2:18443)
wget: can't connect to remote host (10.0.2.2): No buffer space available

```

报错分为以下几种：
1. wget: error getting response。无法稳定复现，但是在多次测试时是有发生
2. D0BC0E0000000000:error:0A000126:SSL routines::unexpected eof while reading:ssl/record/rec_layer_s3.c:698:ssl_client: SSL_connect 多次测试后常出现
3. No buffer space available。2报错出现后必定出现，且经过一段时候后依然无法回复，怀疑buffer泄漏

我保证host上已经拉起https server, 拉起指令如下：
```sh
cd ~/project/temp
mkdir -p /tmp/f7ly_https

openssl req -x509 -newkey rsa:2048 \
  -keyout /tmp/f7ly_https/key.pem \
  -out /tmp/f7ly_https/cert.pem \
  -days 1 -nodes \
  -subj "/CN=localhost"

openssl s_server \
  -quiet -WWW \
  -accept 18443 \
  -cert /tmp/f7ly_https/cert.pem \
  -key /tmp/f7ly_https/key.pem
```

请借助这个server，并通过在链路上打印调试输出,定位这个问题出现的原因,按照下面的步骤
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码
参考实现：
1. ref/rocketos（往届作品第一名）

### 修复小结

- 现象：HTTPS 短连接多次后 `No buffer space available`。原因：TCP `TLSCONNECTED` close 只发 FIN，未提前释放收/发用户缓冲，ONPS 32KB buddy 池被短连接占满。解决方案：主动 close 后立即 `onps_input_release_tcp_user_buffers()`，保留 input/link 给 FIN timer 收尾。

## 连接https://dl-cdn.alpinelinux.org报错
### 情况描述
在shell中尝试连接`dl-cdn.alpinelinux.org`页面，返回`Address family not supported by protocol`
```sh
F7LY:~$ wget -O - https://dl-cdn.alpinelinux.org
Connecting to dl-cdn.alpinelinux.org (199.232.114.132:443)
D0BC0E0000000000:error:0A000126:SSL routines::unexpected eof while reading:ssl/record/rec_layer_s3.c:698:
ssl_client: SSL_connect
wget: error getting response: Address family not supported by protocol

F7LY:~$ nslookup dl-cdn.alpinelinux.org 10.0.2.3
Server:         10.0.2.3
Address:        10.0.2.3:53

Non-authoritative answer:
dl-cdn.alpinelinux.org  canonical name = dualstack.j.sni.global.fastly.net
Name:   dualstack.j.sni.global.fastly.net
Address: 2a04:4e42:5c::644

Non-authoritative answer:
dl-cdn.alpinelinux.org  canonical name = dualstack.j.sni.global.fastly.net
Name:   dualstack.j.sni.global.fastly.net
Address: 199.232.114.132

```
请通过在链路上打印调试输出,定位这个问题出现的原因,按照下面的步骤
debug方式：（必须先阅读agent_docs/development_debugging.md，了解调试范式）
1. 先进行静态代码阅读，确定可能的问题点
2. 针对性的添加注释，逐渐排查问题
3. 按照调试范式运行代码
参考实现：
1. ref/rocketos（往届作品第一名）


### 修复小结

- 现象：HTTPS 已连上 `199.232.114.132:443`，TLS 握手报 `unexpected eof while reading`，随后 `wget` 显示 `Address family not supported by protocol`。
- 原因：`tcp_recv_upper()` 把 `bRcvTimeout=-1` 的 1 秒内部轮询误当成超时；无 FIN/RST、只是暂时无数据时向上返回 0，导致 OpenSSL 误判 EOF。
- 解决方案：只有 `signed_timeout > 0 && bWaitSecs <= 0` 才返回 0；永久等待继续循环复查关闭态。验证：`wget` 成功，双架构 build 通过。
