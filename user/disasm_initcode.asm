
user/initcode-rv:     file format binary


Disassembly of section .data:

0000000000000000 <.data>:
   0:	1141                	addi	sp,sp,-16
   2:	e406                	sd	ra,8(sp)
   4:	460d                	li	a2,3
   6:	00000597          	auipc	a1,0x0
   a:	17258593          	addi	a1,a1,370 # 0x178
   e:	4505                	li	a0,1
  10:	010000ef          	jal	0x20
  14:	156000ef          	jal	0x16a
  18:	4501                	li	a0,0
  1a:	60a2                	ld	ra,8(sp)
  1c:	0141                	addi	sp,sp,16
  1e:	8082                	ret
  20:	03800893          	li	a7,56
  24:	18000693          	li	a3,384
  28:	00000073          	ecall
  2c:	2501                	sext.w	a0,a0
  2e:	8082                	ret
  30:	03900893          	li	a7,57
  34:	00000073          	ecall
  38:	2501                	sext.w	a0,a0
  3a:	8082                	ret
  3c:	03f00893          	li	a7,63
  40:	00000073          	ecall
  44:	8082                	ret
  46:	04000893          	li	a7,64
  4a:	00000073          	ecall
  4e:	8082                	ret
  50:	0ac00893          	li	a7,172
  54:	00000073          	ecall
  58:	2501                	sext.w	a0,a0
  5a:	8082                	ret
  5c:	0ad00893          	li	a7,173
  60:	00000073          	ecall
  64:	2501                	sext.w	a0,a0
  66:	8082                	ret
  68:	07c00893          	li	a7,124
  6c:	00000073          	ecall
  70:	2501                	sext.w	a0,a0
  72:	8082                	ret
  74:	85b2                	mv	a1,a2
  76:	863a                	mv	a2,a4
  78:	c191                	beqz	a1,0x7c
  7a:	95b6                	add	a1,a1,a3
  7c:	0dc00893          	li	a7,220
  80:	4681                	li	a3,0
  82:	4701                	li	a4,0
  84:	4781                	li	a5,0
  86:	00000073          	ecall
  8a:	2501                	sext.w	a0,a0
  8c:	8082                	ret
  8e:	05d00893          	li	a7,93
  92:	00000073          	ecall
  96:	8082                	ret
  98:	10400893          	li	a7,260
  9c:	4681                	li	a3,0
  9e:	00000073          	ecall
  a2:	2501                	sext.w	a0,a0
  a4:	8082                	ret
  a6:	0dd00893          	li	a7,221
  aa:	00000073          	ecall
  ae:	2501                	sext.w	a0,a0
  b0:	8082                	ret
  b2:	0dd00893          	li	a7,221
  b6:	00000073          	ecall
  ba:	2501                	sext.w	a0,a0
  bc:	8082                	ret
  be:	09900893          	li	a7,153
  c2:	00000073          	ecall
  c6:	8082                	ret
  c8:	0d700893          	li	a7,215
  cc:	00000073          	ecall
  d0:	2501                	sext.w	a0,a0
  d2:	8082                	ret
  d4:	1141                	addi	sp,sp,-16
  d6:	e406                	sd	ra,8(sp)
  d8:	85aa                	mv	a1,a0
  da:	4601                	li	a2,0
  dc:	557d                	li	a0,-1
  de:	fbbff0ef          	jal	0x98
  e2:	60a2                	ld	ra,8(sp)
  e4:	0141                	addi	sp,sp,16
  e6:	8082                	ret
  e8:	02500893          	li	a7,37
  ec:	1702                	slli	a4,a4,0x20
  ee:	9301                	srli	a4,a4,0x20
  f0:	00000073          	ecall
  f4:	2501                	sext.w	a0,a0
  f6:	8082                	ret
  f8:	02300893          	li	a7,35
  fc:	1602                	slli	a2,a2,0x20
  fe:	9201                	srli	a2,a2,0x20
 100:	00000073          	ecall
 104:	2501                	sext.w	a0,a0
 106:	8082                	ret
 108:	0a000893          	li	a7,160
 10c:	00000073          	ecall
 110:	2501                	sext.w	a0,a0
 112:	8082                	ret
 114:	0d600893          	li	a7,214
 118:	00000073          	ecall
 11c:	2501                	sext.w	a0,a0
 11e:	8082                	ret
 120:	03100893          	li	a7,49
 124:	00000073          	ecall
 128:	2501                	sext.w	a0,a0
 12a:	8082                	ret
 12c:	03d00893          	li	a7,61
 130:	00000073          	ecall
 134:	2501                	sext.w	a0,a0
 136:	8082                	ret
 138:	03b00893          	li	a7,59
 13c:	4581                	li	a1,0
 13e:	00000073          	ecall
 142:	2501                	sext.w	a0,a0
 144:	8082                	ret
 146:	48dd                	li	a7,23
 148:	00000073          	ecall
 14c:	2501                	sext.w	a0,a0
 14e:	8082                	ret
 150:	02800893          	li	a7,40
 154:	00000073          	ecall
 158:	2501                	sext.w	a0,a0
 15a:	8082                	ret
 15c:	02700893          	li	a7,39
 160:	4581                	li	a1,0
 162:	00000073          	ecall
 166:	2501                	sext.w	a0,a0
 168:	8082                	ret
 16a:	48cd                	li	a7,19
 16c:	00000073          	ecall
 170:	2501                	sext.w	a0,a0
 172:	8082                	ret
 174:	0000                	unimp
 176:	0000                	unimp
 178:	6464                	ld	s1,200(s0)
 17a:	0064                	addi	s1,sp,12
