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

#include "content/analysis.typ"


#include "content/proj_practice.typ"

#include "content/summary&review.typ"