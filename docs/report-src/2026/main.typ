// 使用示例：
//   cd docs/report-src
//   typst compile --root . 2026/main.typ 2026/f7ly-os-2026-outline.pdf
//
// 本文件只提供 2026 年 OSCOMP 设计文档骨架，不包含正文。

#import "../conf.typ": doc, preface, main
#import "../components/cover.typ": cover
#import "../components/outline.typ": outline-page

#show: doc

#set text(lang: "zh", region: "cn")

#cover(
  title: "2026 年 OSCOMP 设计文档",
  year: 2026,
  month: 6,
)

#show: preface.with(title: "F7LY-OS")

#outline-page()

#show: main

#include "content/01-overview.typ"
#include "content/02-boot.typ"
#include "content/03-trap.typ"
#include "content/04-memory.typ"
#include "content/05-process.typ"
#include "content/06-filesystem.typ"
#include "content/07-ipc.typ"
#include "content/appendix.typ"
