#import "../../docs/report-src/2026-final-final/conf.typ": doc, preface, main
#import "../../docs/report-src/2026-final-final/components/cover.typ": cover
#import "../../docs/report-src/2026-final-final/components/outline.typ": outline-page

#show: doc

#set text(lang: "zh", region: "cn")

#cover(
  title: "内核设计与优化实现文档",
  institute: "武汉大学计算机学院",
  year: 2026,
  month: 8,
)

#show: preface.with(title: "内核设计与优化实现文档")

#outline-page()

#show: main

// 正文采用 12pt 两端对齐，但窄表格中的路径和脚本名需要更紧凑的独立规则。
// 表格内取消首行缩进与两端拉伸，并降低行内代码字号，避免不可拆分标识符越界。
#set table(inset: (x: 4pt, y: 4pt))
#show table: it => {
  set text(size: 9.5pt)
  set par(first-line-indent: 0em, leading: 0.65em, justify: false)
  show raw.where(block: false): text.with(size: 8.5pt)
  it
}

#include "content/01-scope.typ"
#include "content/02-agent-workflow.typ"
#include "content/03-scripts-tools.typ"
#include "content/04-root-cause.typ"
#include "content/05-design.typ"
#include "content/06-experiments.typ"
#include "content/07-ai-reproduction.typ"
#include "content/appendix.typ"
