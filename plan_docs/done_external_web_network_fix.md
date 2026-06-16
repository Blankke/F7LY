## 情况描述
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
