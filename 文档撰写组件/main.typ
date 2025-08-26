#import "conf.typ": doc, preface, main
#import "components/cover.typ": cover
#import "components/figure.typ": algorithm-figure, code-figure
#import "components/outline.typ": outline-page
#import "@preview/lovelace:0.2.0": *

#show: doc

#set text(lang: "zh", region: "cn")

#cover(
  title: "设计文档",
)

#show: preface.with(title: "F7LY-OS")

#outline-page()

#show: main

#include "content/overview.typ"

#include "content/kernel-detailed.typ"

#include "content/syscall.typ"

#include "content/net.typ"
#include "content/device_manage.typ"
#include "content/summary.typ"

#include "content/appendix.typ"
