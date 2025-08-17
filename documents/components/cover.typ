#import "typography.typ": 字体, 字号

#let cover(
  title: "",
  institute: "",
  year: datetime.today().year(),
  month: datetime.today().month(),
) = {
  align(center)[

    #let space_scale_ratio = 1.2

    // 添加校徽
    #image("../content/fig/校徽.png", width: 10cm)

    #text(size: 字号.特号, font: 字体.宋体, weight: "bold")[*F7LY-OS*]

    #text(size: 字号.一号, font: 字体.黑体, weight: "bold")[#title]

    #v(字号.小四 * 6 * space_scale_ratio)    #v(字号.小四 * 5 * space_scale_ratio)


    // 添加参赛人员信息
    #align(center)[
      #grid(
        columns: 1,
        gutter: 字号.小三 * 0.8,
        align: center,
        [#text(size: 字号.三号, font: 字体.宋体)[队伍名称：#box(width: 12em, stroke: (bottom: 0.5pt), inset: (bottom: 3pt))[#align(center)[F7LY]]]],
        [#text(size: 字号.三号, font: 字体.宋体)[队伍成员：#box(width: 12em, stroke: (bottom: 0.5pt), inset: (bottom: 3pt))[#align(center)[曹子宸、郑喆宇、官恺祺]]]],
        [#text(size: 字号.三号, font: 字体.宋体)[指导老师：#box(width: 12em, stroke: (bottom: 0.5pt), inset: (bottom: 3pt))[#align(center)[蔡朝晖]]]]
      )
    ]

    #v(字号.小四 * 3 * space_scale_ratio)

    #align(center)[
      #text(size: 字号.小二, font: 字体.楷体, weight: "bold")[#institute]

      #text(size: 字号.小二, font: 字体.宋体, weight: "bold")[
        #[#year]年#[#month]月
      ]
    ]
  ]
}
