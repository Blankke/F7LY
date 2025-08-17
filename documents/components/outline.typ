#import "typography.typ": 字体

#let outline-page() = [
  #set par(first-line-indent: 0em)

  #[
    #show heading: none
    #heading([目录], level: 1, outlined: false)
  ]

  #outline(title: align(center)[目录], indent: 2em, depth: 3)
  // #outline(title: align(center)[目录], indent: auto)
]
