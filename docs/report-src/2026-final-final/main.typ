// 使用示例：
//   cd docs/report-src
//   typst compile --root . 2026/main.typ 2026/f7ly-os-2026-outline.pdf
//
// 本文件只提供 2026 年 OSCOMP 设计文档骨架，不包含正文。

#import "conf.typ": doc, preface, main
#import "components/cover.typ": cover
#import "components/outline.typ": outline-page

#show: doc

#set text(lang: "zh", region: "cn")

#cover(
  title: "OSCOMP 设计文档（决赛阶段）",
  year: 2026,
  month: 8,
)

#show: preface.with(title: "OSCOMP 设计文档（决赛阶段）")

#outline-page()

#show: main

#include "content/01-overview.typ"
#include "content/02-linux-semantics.typ"
#include "content/03-network.typ"
#include "content/04-smp.typ"
#include "content/05-performance.typ"
#include "content/06-summary&review.typ"
#include "content/appendix.typ"
