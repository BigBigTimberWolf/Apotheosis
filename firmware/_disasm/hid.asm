
hid_device.c.obj:     file format elf32-xtensa-le


Disassembly of section .literal.tud_hid_n_ready:

00000000 <.literal.tud_hid_n_ready>:
	...

Disassembly of section .literal.tud_hid_n_report:

00000000 <.literal.tud_hid_n_report>:
	...

Disassembly of section .literal.tud_hid_n_interface_protocol:

00000000 <.literal.tud_hid_n_interface_protocol>:
   0:	00 00 00 00 	

Disassembly of section .literal.tud_hid_n_get_protocol:

00000000 <.literal.tud_hid_n_get_protocol>:
   0:	00 00 00 00 	

Disassembly of section .literal.tud_hid_n_keyboard_report:

00000000 <.literal.tud_hid_n_keyboard_report>:
	...

Disassembly of section .literal.tud_hid_n_mouse_report:

00000000 <.literal.tud_hid_n_mouse_report>:
   0:	00 00 00 00 	

Disassembly of section .literal.tud_hid_n_gamepad_report:

00000000 <.literal.tud_hid_n_gamepad_report>:
   0:	00 00 00 00 	

Disassembly of section .literal.hidd_reset:

00000000 <.literal.hidd_reset>:
	...

Disassembly of section .literal.hidd_init:

00000000 <.literal.hidd_init>:
   0:	00 00 00 00 	

Disassembly of section .literal.hidd_open:

00000000 <.literal.hidd_open>:
   0:	00 00 00 00 	
   4:	01 00 00 00 	
   8:	02 00 00 00 	
   c:	48 00 00 00 	
	...

Disassembly of section .literal.hidd_control_xfer_cb:

00000000 <.literal.hidd_control_xfer_cb>:
   0:	09 00 00 00 	
   4:	08 00 00 00 	
   8:	48 00 00 00 	
   c:	49 00 00 00 	
	...
  18:	05 00 00 00 	
  1c:	04 00 00 00 	
	...

Disassembly of section .literal.hidd_xfer_cb:

00000000 <.literal.hidd_xfer_cb>:
	...
   8:	08 00 00 00 	
   c:	48 00 00 00 	
	...

Disassembly of section .text.tud_hid_n_ready:

00000000 <tud_hid_n_ready>:
   0:	004136        	entry	a1, 32
   3:	742020        	extui	a2, a2, 0, 8
   6:	1182e0        	slli	a8, a2, 2
   9:	882a      	add.n	a8, a8, a2
   b:	1128d0        	slli	a2, a8, 3
   e:	c02280        	sub	a2, a2, a8
  11:	000081        	l32r	a8, fffc0014 <tud_hid_n_ready+0xfffc0014>
  14:	1122e0        	slli	a2, a2, 2
  17:	282a      	add.n	a2, a8, a2
  19:	010232        	l8ui	a3, a2, 1
  1c:	000081        	l32r	a8, fffc001c <tud_hid_n_ready+0xfffc001c>
  1f:	0008e0        	callx8	a8
  22:	6acc      	bnez.n	a10, 2c <tud_hid_n_ready+0x2c>
  24:	020c      	movi.n	a2, 0
  26:	042020        	extui	a2, a2, 0, 1
  29:	f01d      	retw.n
  2b:	00          	.byte 00
  2c:	000081        	l32r	a8, fffc002c <tud_hid_n_ready+0xfffc002c>
  2f:	0008e0        	callx8	a8
  32:	120c      	movi.n	a2, 1
  34:	fec316        	beqz	a3, 24 <tud_hid_n_ready+0x24>
  37:	30aa20        	xor	a10, a10, a2
  3a:	74a0a0        	extui	a10, a10, 0, 8
  3d:	fe3a16        	beqz	a10, 24 <tud_hid_n_ready+0x24>
  40:	20b330        	or	a11, a3, a3
  43:	00a0a2        	movi	a10, 0
  46:	000081        	l32r	a8, fffc0048 <tud_hid_n_ready+0xfffc0048>
  49:	0008e0        	callx8	a8
  4c:	302a20        	xor	a2, a10, a2
  4f:	742020        	extui	a2, a2, 0, 8
  52:	fff406        	j	26 <tud_hid_n_ready+0x26>

Disassembly of section .text.tud_hid_n_report:

00000000 <tud_hid_n_report>:
   0:	006136        	entry	a1, 48
   3:	742020        	extui	a2, a2, 0, 8
   6:	749030        	extui	a9, a3, 0, 8
   9:	1132e0        	slli	a3, a2, 2
   c:	632a      	add.n	a6, a3, a2
   e:	1176d0        	slli	a7, a6, 3
  11:	c07760        	sub	a7, a7, a6
  14:	000061        	l32r	a6, fffc0014 <tud_hid_n_report+0xfffc0014>
  17:	1177e0        	slli	a7, a7, 2
  1a:	767a      	add.n	a7, a6, a7
  1c:	0107b2        	l8ui	a11, a7, 1
  1f:	0a0c      	movi.n	a10, 0
  21:	3199      	s32i.n	a9, a1, 12
  23:	000081        	l32r	a8, fffc0024 <tud_hid_n_report+0xfffc0024>
  26:	0008e0        	callx8	a8
  29:	f45050        	extui	a5, a5, 0, 16
  2c:	3198      	l32i.n	a9, a1, 12
  2e:	2acc      	bnez.n	a10, 34 <tud_hid_n_report+0x34>
  30:	020c      	movi.n	a2, 0
  32:	f01d      	retw.n
  34:	1162e0        	slli	a6, a2, 2
  37:	a62a      	add.n	a10, a6, a2
  39:	116ad0        	slli	a6, a10, 3
  3c:	c066a0        	sub	a6, a6, a10
  3f:	1166e0        	slli	a6, a6, 2
  42:	000081        	l32r	a8, fffc0044 <tud_hid_n_report+0xfffc0044>
  45:	668b      	addi.n	a6, a6, 8
  47:	668a      	add.n	a6, a6, a8
  49:	c9bc      	beqz.n	a9, 89 <tud_hid_n_report+0x89>
  4b:	084792        	s8i	a9, a7, 8
  4e:	f73c      	movi.n	a7, 63
  50:	a61b      	addi.n	a10, a6, 1
  52:	da3757        	bltu	a7, a5, 30 <tud_hid_n_report+0x30>
  55:	05cd      	mov.n	a12, a5
  57:	04bd      	mov.n	a11, a4
  59:	551b      	addi.n	a5, a5, 1
  5b:	000081        	l32r	a8, fffc005c <tud_hid_n_report+0xfffc005c>
  5e:	0008e0        	callx8	a8
  61:	f45050        	extui	a5, a5, 0, 16
  64:	832a      	add.n	a8, a3, a2
  66:	1128d0        	slli	a2, a8, 3
  69:	c02280        	sub	a2, a2, a8
  6c:	000041        	l32r	a4, fffc006c <tud_hid_n_report+0xfffc006c>
  6f:	1122e0        	slli	a2, a2, 2
  72:	342a      	add.n	a3, a4, a2
  74:	0103b2        	l8ui	a11, a3, 1
  77:	05dd      	mov.n	a13, a5
  79:	06cd      	mov.n	a12, a6
  7b:	00a0a2        	movi	a10, 0
  7e:	000081        	l32r	a8, fffc0080 <tud_hid_n_report+0xfffc0080>
  81:	0008e0        	callx8	a8
  84:	0a2d      	mov.n	a2, a10
  86:	ffea06        	j	32 <tud_hid_n_report+0x32>
  89:	074c      	movi.n	a7, 64
  8b:	a13757        	bltu	a7, a5, 30 <tud_hid_n_report+0x30>
  8e:	05cd      	mov.n	a12, a5
  90:	20b440        	or	a11, a4, a4
  93:	20a660        	or	a10, a6, a6
  96:	000081        	l32r	a8, fffc0098 <tud_hid_n_report+0xfffc0098>
  99:	0008e0        	callx8	a8
  9c:	fff106        	j	64 <tud_hid_n_report+0x64>

Disassembly of section .text.tud_hid_n_interface_protocol:

00000000 <tud_hid_n_interface_protocol>:
   0:	004136        	entry	a1, 32
   3:	742020        	extui	a2, a2, 0, 8
   6:	1182e0        	slli	a8, a2, 2
   9:	282a      	add.n	a2, a8, a2
   b:	1182d0        	slli	a8, a2, 3
   e:	c08820        	sub	a8, a8, a2
  11:	000021        	l32r	a2, fffc0014 <tud_hid_n_interface_protocol+0xfffc0014>
  14:	1188e0        	slli	a8, a8, 2
  17:	828a      	add.n	a8, a2, a8
  19:	030822        	l8ui	a2, a8, 3
  1c:	f01d      	retw.n

Disassembly of section .text.tud_hid_n_get_protocol:

00000000 <tud_hid_n_get_protocol>:
   0:	004136        	entry	a1, 32
   3:	742020        	extui	a2, a2, 0, 8
   6:	1182e0        	slli	a8, a2, 2
   9:	282a      	add.n	a2, a8, a2
   b:	1182d0        	slli	a8, a2, 3
   e:	c08820        	sub	a8, a8, a2
  11:	000021        	l32r	a2, fffc0014 <tud_hid_n_get_protocol+0xfffc0014>
  14:	1188e0        	slli	a8, a8, 2
  17:	828a      	add.n	a8, a2, a8
  19:	040822        	l8ui	a2, a8, 4
  1c:	f01d      	retw.n

Disassembly of section .text.tud_hid_n_keyboard_report:

00000000 <tud_hid_n_keyboard_report>:
   0:	006136        	entry	a1, 48
   3:	080c      	movi.n	a8, 0
   5:	084142        	s8i	a4, a1, 8
   8:	094182        	s8i	a8, a1, 9
   b:	05bd      	mov.n	a11, a5
   d:	742020        	extui	a2, a2, 0, 8
  10:	743030        	extui	a3, a3, 0, 8
  13:	6c0c      	movi.n	a12, 6
  15:	0ac1a2        	addi	a10, a1, 10
  18:	859c      	beqz.n	a5, 34 <tud_hid_n_keyboard_report+0x34>
  1a:	000081        	l32r	a8, fffc001c <tud_hid_n_keyboard_report+0xfffc001c>
  1d:	0008e0        	callx8	a8
  20:	8d0c      	movi.n	a13, 8
  22:	c1da      	add.n	a12, a1, a13
  24:	03bd      	mov.n	a11, a3
  26:	02ad      	mov.n	a10, a2
  28:	000081        	l32r	a8, fffc0028 <tud_hid_n_keyboard_report+0xfffc0028>
  2b:	0008e0        	callx8	a8
  2e:	0a2d      	mov.n	a2, a10
  30:	f01d      	retw.n
  32:	00          	.byte 00
  33:	00          	.byte 00
  34:	000081        	l32r	a8, fffc0034 <tud_hid_n_keyboard_report+0xfffc0034>
  37:	0008e0        	callx8	a8
  3a:	fff886        	j	20 <tud_hid_n_keyboard_report+0x20>

Disassembly of section .text.tud_hid_n_mouse_report:

00000000 <tud_hid_n_mouse_report>:
   0:	006136        	entry	a1, 48
   3:	300182        	l8ui	a8, a1, 48
   6:	5d0c      	movi.n	a13, 5
   8:	c1bb      	addi.n	a12, a1, 11
   a:	74b030        	extui	a11, a3, 0, 8
   d:	74a020        	extui	a10, a2, 0, 8
  10:	0b4142        	s8i	a4, a1, 11
  13:	0c4152        	s8i	a5, a1, 12
  16:	0d4162        	s8i	a6, a1, 13
  19:	0e4172        	s8i	a7, a1, 14
  1c:	0f4182        	s8i	a8, a1, 15
  1f:	000081        	l32r	a8, fffc0020 <tud_hid_n_mouse_report+0xfffc0020>
  22:	0008e0        	callx8	a8
  25:	0a2d      	mov.n	a2, a10
  27:	f01d      	retw.n

Disassembly of section .text.tud_hid_n_gamepad_report:

00000000 <tud_hid_n_gamepad_report>:
   0:	006136        	entry	a1, 48
   3:	f188      	l32i.n	a8, a1, 60
   5:	380192        	l8ui	a9, a1, 56
   8:	3001b2        	l8ui	a11, a1, 48
   b:	3401a2        	l8ui	a10, a1, 52
   e:	0b4192        	s8i	a9, a1, 11
  11:	749880        	extui	a9, a8, 8, 8
  14:	0941b2        	s8i	a11, a1, 9
  17:	0a41a2        	s8i	a10, a1, 10
  1a:	0c4182        	s8i	a8, a1, 12
  1d:	0d4192        	s8i	a9, a1, 13
  20:	bd0c      	movi.n	a13, 11
  22:	759080        	extui	a9, a8, 16, 8
  25:	c15b      	addi.n	a12, a1, 5
  27:	758880        	extui	a8, a8, 24, 8
  2a:	74b030        	extui	a11, a3, 0, 8
  2d:	74a020        	extui	a10, a2, 0, 8
  30:	054142        	s8i	a4, a1, 5
  33:	064152        	s8i	a5, a1, 6
  36:	074162        	s8i	a6, a1, 7
  39:	084172        	s8i	a7, a1, 8
  3c:	0e4192        	s8i	a9, a1, 14
  3f:	0f4182        	s8i	a8, a1, 15
  42:	000081        	l32r	a8, fffc0044 <tud_hid_n_gamepad_report+0xfffc0044>
  45:	0008e0        	callx8	a8
  48:	0a2d      	mov.n	a2, a10
  4a:	f01d      	retw.n

Disassembly of section .text.hidd_reset:

00000000 <hidd_reset>:
   0:	004136        	entry	a1, 32
   3:	0000a1        	l32r	a10, fffc0004 <hidd_reset+0xfffc0004>
   6:	8ca0c2        	movi	a12, 140
   9:	0b0c      	movi.n	a11, 0
   b:	000081        	l32r	a8, fffc000c <hidd_reset+0xfffc000c>
   e:	0008e0        	callx8	a8
  11:	f01d      	retw.n

Disassembly of section .text.hidd_init:

00000000 <hidd_init>:
   0:	004136        	entry	a1, 32
   3:	00a0a2        	movi	a10, 0
   6:	000081        	l32r	a8, fffc0008 <hidd_init+0xfffc0008>
   9:	0008e0        	callx8	a8
   c:	f01d      	retw.n

Disassembly of section .text.hidd_open:

00000000 <hidd_open>:
   0:	004136        	entry	a1, 32
   3:	0503d2        	l8ui	a13, a3, 5
   6:	745020        	extui	a5, a2, 0, 8
   9:	f44040        	extui	a4, a4, 0, 16
   c:	043d26        	beqi	a13, 3, 14 <hidd_open+0x14>
   f:	020c      	movi.n	a2, 0
  11:	f01d      	retw.n
  13:	00          	.byte 00
  14:	0403c2        	l8ui	a12, a3, 4
  17:	112cd0        	slli	a2, a12, 3
  1a:	c022c0        	sub	a2, a2, a12
  1d:	12c222        	addi	a2, a2, 18
  20:	f42020        	extui	a2, a2, 0, 16
  23:	e83427        	bltu	a4, a2, f <hidd_open+0xf>
  26:	000041        	l32r	a4, fffc0028 <hidd_open+0xfffc0028>
  29:	010482        	l8ui	a8, a4, 1
  2c:	fdf856        	bnez	a8, f <hidd_open+0xf>
  2f:	000382        	l8ui	a8, a3, 0
  32:	192c      	movi.n	a9, 33
  34:	808380        	add	a8, a3, a8
  37:	0108a2        	l8ui	a10, a8, 1
  3a:	d19a97        	bne	a10, a9, f <hidd_open+0xf>
  3d:	226482        	s32i	a8, a4, 136
  40:	0008b2        	l8ui	a11, a8, 0
  43:	0000f1        	l32r	a15, fffc0044 <hidd_open+0xfffc0044>
  46:	0000e1        	l32r	a14, fffc0048 <hidd_open+0xfffc0048>
  49:	80b8b0        	add	a11, a8, a11
  4c:	20a550        	or	a10, a5, a5
  4f:	000081        	l32r	a8, fffc0050 <hidd_open+0xfffc0050>
  52:	0008e0        	callx8	a8
  55:	fb6a16        	beqz	a10, f <hidd_open+0xf>
  58:	060382        	l8ui	a8, a3, 6
  5b:	051866        	bnei	a8, 1, 64 <hidd_open+0x64>
  5e:	070382        	l8ui	a8, a3, 7
  61:	034482        	s8i	a8, a4, 3
  64:	180c      	movi.n	a8, 1
  66:	044482        	s8i	a8, a4, 4
  69:	020332        	l8ui	a3, a3, 2
  6c:	222482        	l32i	a8, a4, 136
  6f:	004432        	s8i	a3, a4, 0
  72:	080832        	l8ui	a3, a8, 8
  75:	070892        	l8ui	a9, a8, 7
  78:	113380        	slli	a3, a3, 8
  7b:	203390        	or	a3, a3, a9
  7e:	0204b2        	l8ui	a11, a4, 2
  81:	035432        	s16i	a3, a4, 6
  84:	f89b16        	beqz	a11, 11 <hidd_open+0x11>
  87:	0000c1        	l32r	a12, fffc0088 <hidd_open+0xfffc0088>
  8a:	0d4c      	movi.n	a13, 64
  8c:	05ad      	mov.n	a10, a5
  8e:	000081        	l32r	a8, fffc0090 <hidd_open+0xfffc0090>
  91:	0008e0        	callx8	a8
  94:	ffde46        	j	11 <hidd_open+0x11>

Disassembly of section .text.hidd_control_xfer_cb:

00000000 <hidd_control_xfer_cb>:
   0:	004136        	entry	a1, 32
   3:	0004a2        	l8ui	a10, a4, 0
   6:	745020        	extui	a5, a2, 0, 8
   9:	742030        	extui	a2, a3, 0, 8
   c:	4430a0        	extui	a3, a10, 0, 5
   f:	051326        	beqi	a3, 1, 18 <hidd_control_xfer_cb+0x18>
  12:	020c      	movi.n	a2, 0
  14:	001906        	j	7c <hidd_control_xfer_cb+0x7c>
  17:	00          	.byte 00
  18:	000031        	l32r	a3, fffc0018 <hidd_control_xfer_cb+0xfffc0018>
  1b:	040492        	l8ui	a9, a4, 4
  1e:	000382        	l8ui	a8, a3, 0
  21:	ed9987        	bne	a9, a8, 12 <hidd_control_xfer_cb+0x12>
  24:	60a082        	movi	a8, 96
  27:	10aa80        	and	a10, a10, a8
  2a:	3aac      	beqz.n	a10, 51 <hidd_control_xfer_cb+0x51>
  2c:	e2ca66        	bnei	a10, 32, 12 <hidd_control_xfer_cb+0x12>
  2f:	010482        	l8ui	a8, a4, 1
  32:	023866        	bnei	a8, 3, 38 <hidd_control_xfer_cb+0x38>
  35:	005d06        	j	1ad <hidd_control_xfer_cb+0x1ad>
  38:	6048f6        	bgeui	a8, 4, 9c <hidd_control_xfer_cb+0x9c>
  3b:	021866        	bnei	a8, 1, 41 <hidd_control_xfer_cb+0x41>
  3e:	002386        	j	d0 <hidd_control_xfer_cb+0xd0>
  41:	cd2866        	bnei	a8, 2, 12 <hidd_control_xfer_cb+0x12>
  44:	02dd      	mov.n	a13, a2
  46:	0000c1        	l32r	a12, fffc0048 <hidd_control_xfer_cb+0xfffc0048>
  49:	071266        	bnei	a2, 1, 54 <hidd_control_xfer_cb+0x54>
  4c:	000f86        	j	8e <hidd_control_xfer_cb+0x8e>
  4f:	00          	.byte 00
  50:	00          	.byte 00
  51:	041226        	beqi	a2, 1, 59 <hidd_control_xfer_cb+0x59>
  54:	120c      	movi.n	a2, 1
  56:	000886        	j	7c <hidd_control_xfer_cb+0x7c>
  59:	010482        	l8ui	a8, a4, 1
  5c:	b26866        	bnei	a8, 6, 12 <hidd_control_xfer_cb+0x12>
  5f:	030482        	l8ui	a8, a4, 3
  62:	192c      	movi.n	a9, 33
  64:	169897        	bne	a8, a9, 7e <hidd_control_xfer_cb+0x7e>
  67:	2223c2        	l32i	a12, a3, 136
  6a:	fa4c16        	beqz	a12, 12 <hidd_control_xfer_cb+0x12>
  6d:	000cd2        	l8ui	a13, a12, 0
  70:	04bd      	mov.n	a11, a4
  72:	05ad      	mov.n	a10, a5
  74:	000081        	l32r	a8, fffc0074 <hidd_control_xfer_cb+0xfffc0074>
  77:	0008e0        	callx8	a8
  7a:	0a2d      	mov.n	a2, a10
  7c:	f01d      	retw.n
  7e:	292c      	movi.n	a9, 34
  80:	8e9897        	bne	a8, a9, 12 <hidd_control_xfer_cb+0x12>
  83:	000081        	l32r	a8, fffc0084 <hidd_control_xfer_cb+0xfffc0084>
  86:	0008e0        	callx8	a8
  89:	0313d2        	l16ui	a13, a3, 6
  8c:	0acd      	mov.n	a12, a10
  8e:	04bd      	mov.n	a11, a4
  90:	05ad      	mov.n	a10, a5
  92:	000081        	l32r	a8, fffc0094 <hidd_control_xfer_cb+0xfffc0094>
  95:	0008e0        	callx8	a8
  98:	fff806        	j	7c <hidd_control_xfer_cb+0x7c>
  9b:	00          	.byte 00
  9c:	029866        	bnei	a8, 10, a2 <hidd_control_xfer_cb+0xa2>
  9f:	003846        	j	184 <hidd_control_xfer_cb+0x184>
  a2:	b90c      	movi.n	a9, 11
  a4:	029897        	bne	a8, a9, aa <hidd_control_xfer_cb+0xaa>
  a7:	004446        	j	1bc <hidd_control_xfer_cb+0x1bc>
  aa:	990c      	movi.n	a9, 9
  ac:	021897        	beq	a8, a9, b2 <hidd_control_xfer_cb+0xb2>
  af:	ffd7c6        	j	12 <hidd_control_xfer_cb+0x12>
  b2:	7b1266        	bnei	a2, 1, 131 <hidd_control_xfer_cb+0x131>
  b5:	0704d2        	l8ui	a13, a4, 7
  b8:	060422        	l8ui	a2, a4, 6
  bb:	11dd80        	slli	a13, a13, 8
  be:	20dd20        	or	a13, a13, a2
  c1:	024c      	movi.n	a2, 64
  c3:	02b2d7        	bgeu	a2, a13, c9 <hidd_control_xfer_cb+0xc9>
  c6:	ffd206        	j	12 <hidd_control_xfer_cb+0x12>
  c9:	0000c1        	l32r	a12, fffc00cc <hidd_control_xfer_cb+0xfffc00cc>
  cc:	0012c6        	j	11b <hidd_control_xfer_cb+0x11b>
  cf:	00          	.byte 00
  d0:	801266        	bnei	a2, 1, 54 <hidd_control_xfer_cb+0x54>
  d3:	0304b2        	l8ui	a11, a4, 3
  d6:	020422        	l8ui	a2, a4, 2
  d9:	11bb80        	slli	a11, a11, 8
  dc:	20bb20        	or	a11, a11, a2
  df:	070422        	l8ui	a2, a4, 7
  e2:	060482        	l8ui	a8, a4, 6
  e5:	112280        	slli	a2, a2, 8
  e8:	41c8b0        	srli	a12, a11, 8
  eb:	202280        	or	a2, a2, a8
  ee:	0e4c      	movi.n	a14, 64
  f0:	74b0b0        	extui	a11, a11, 0, 8
  f3:	63e2e0        	minu	a14, a2, a14
  f6:	ebac      	beqz.n	a11, 128 <hidd_control_xfer_cb+0x128>
  f8:	2c22b6        	bltui	a2, 2, 128 <hidd_control_xfer_cb+0x128>
  fb:	ee0b      	addi.n	a14, a14, -1
  fd:	0000d1        	l32r	a13, fffc0100 <hidd_control_xfer_cb+0xfffc0100>
 100:	0843b2        	s8i	a11, a3, 8
 103:	f4e0e0        	extui	a14, a14, 0, 16
 106:	120c      	movi.n	a2, 1
 108:	0a0c      	movi.n	a10, 0
 10a:	000081        	l32r	a8, fffc010c <hidd_control_xfer_cb+0xfffc010c>
 10d:	0008e0        	callx8	a8
 110:	a2aa      	add.n	a10, a2, a10
 112:	f4d0a0        	extui	a13, a10, 0, 16
 115:	0000c1        	l32r	a12, fffc0118 <hidd_control_xfer_cb+0xfffc0118>
 118:	ef6d16        	beqz	a13, 12 <hidd_control_xfer_cb+0x12>
 11b:	04bd      	mov.n	a11, a4
 11d:	05ad      	mov.n	a10, a5
 11f:	000081        	l32r	a8, fffc0120 <hidd_control_xfer_cb+0xfffc0120>
 122:	0008e0        	callx8	a8
 125:	ffcac6        	j	54 <hidd_control_xfer_cb+0x54>
 128:	020c      	movi.n	a2, 0
 12a:	0000d1        	l32r	a13, fffc012c <hidd_control_xfer_cb+0xfffc012c>
 12d:	fff5c6        	j	108 <hidd_control_xfer_cb+0x108>
 130:	00          	.byte 00
 131:	023226        	beqi	a2, 3, 137 <hidd_control_xfer_cb+0x137>
 134:	ffc706        	j	54 <hidd_control_xfer_cb+0x54>
 137:	0304b2        	l8ui	a11, a4, 3
 13a:	020422        	l8ui	a2, a4, 2
 13d:	11bb80        	slli	a11, a11, 8
 140:	20bb20        	or	a11, a11, a2
 143:	070422        	l8ui	a2, a4, 7
 146:	060452        	l8ui	a5, a4, 6
 149:	112280        	slli	a2, a2, 8
 14c:	41c8b0        	srli	a12, a11, 8
 14f:	202250        	or	a2, a2, a5
 152:	0e4c      	movi.n	a14, 64
 154:	74b0b0        	extui	a11, a11, 0, 8
 157:	63e2e0        	minu	a14, a2, a14
 15a:	eb9c      	beqz.n	a11, 17c <hidd_control_xfer_cb+0x17c>
 15c:	1c22b6        	bltui	a2, 2, 17c <hidd_control_xfer_cb+0x17c>
 15f:	480322        	l8ui	a2, a3, 72
 162:	0000d1        	l32r	a13, fffc0164 <hidd_control_xfer_cb+0xfffc0164>
 165:	0792b7        	bne	a2, a11, 170 <hidd_control_xfer_cb+0x170>
 168:	ee0b      	addi.n	a14, a14, -1
 16a:	0000d1        	l32r	a13, fffc016c <hidd_control_xfer_cb+0xfffc016c>
 16d:	f4e0e0        	extui	a14, a14, 0, 16
 170:	0a0c      	movi.n	a10, 0
 172:	000081        	l32r	a8, fffc0174 <hidd_control_xfer_cb+0xfffc0174>
 175:	0008e0        	callx8	a8
 178:	ffb606        	j	54 <hidd_control_xfer_cb+0x54>
 17b:	00          	.byte 00
 17c:	0000d1        	l32r	a13, fffc017c <hidd_control_xfer_cb+0xfffc017c>
 17f:	fffb46        	j	170 <hidd_control_xfer_cb+0x170>
 182:	00          	.byte 00
 183:	00          	.byte 00
 184:	021226        	beqi	a2, 1, 18a <hidd_control_xfer_cb+0x18a>
 187:	ffb246        	j	54 <hidd_control_xfer_cb+0x54>
 18a:	0304b2        	l8ui	a11, a4, 3
 18d:	000021        	l32r	a2, fffc0190 <hidd_control_xfer_cb+0xfffc0190>
 190:	0543b2        	s8i	a11, a3, 5
 193:	928c      	beqz.n	a2, 1a0 <hidd_control_xfer_cb+0x1a0>
 195:	0a0c      	movi.n	a10, 0
 197:	000081        	l32r	a8, fffc0198 <hidd_control_xfer_cb+0xfffc0198>
 19a:	0008e0        	callx8	a8
 19d:	e71a16        	beqz	a10, 12 <hidd_control_xfer_cb+0x12>
 1a0:	04bd      	mov.n	a11, a4
 1a2:	05ad      	mov.n	a10, a5
 1a4:	000081        	l32r	a8, fffc01a4 <hidd_control_xfer_cb+0xfffc01a4>
 1a7:	0008e0        	callx8	a8
 1aa:	ffa986        	j	54 <hidd_control_xfer_cb+0x54>
 1ad:	021226        	beqi	a2, 1, 1b3 <hidd_control_xfer_cb+0x1b3>
 1b0:	ffa806        	j	54 <hidd_control_xfer_cb+0x54>
 1b3:	02dd      	mov.n	a13, a2
 1b5:	0000c1        	l32r	a12, fffc01b8 <hidd_control_xfer_cb+0xfffc01b8>
 1b8:	ffb486        	j	8e <hidd_control_xfer_cb+0x8e>
 1bb:	00          	.byte 00
 1bc:	e01226        	beqi	a2, 1, 1a0 <hidd_control_xfer_cb+0x1a0>
 1bf:	023226        	beqi	a2, 3, 1c5 <hidd_control_xfer_cb+0x1c5>
 1c2:	ffa386        	j	54 <hidd_control_xfer_cb+0x54>
 1c5:	0204b2        	l8ui	a11, a4, 2
 1c8:	000021        	l32r	a2, fffc01c8 <hidd_control_xfer_cb+0xfffc01c8>
 1cb:	0443b2        	s8i	a11, a3, 4
 1ce:	e82216        	beqz	a2, 54 <hidd_control_xfer_cb+0x54>
 1d1:	0a0c      	movi.n	a10, 0
 1d3:	000081        	l32r	a8, fffc01d4 <hidd_control_xfer_cb+0xfffc01d4>
 1d6:	0008e0        	callx8	a8
 1d9:	ff9dc6        	j	54 <hidd_control_xfer_cb+0x54>

Disassembly of section .text.hidd_xfer_cb:

00000000 <hidd_xfer_cb>:
   0:	004136        	entry	a1, 32
   3:	000041        	l32r	a4, fffc0004 <hidd_xfer_cb+0xfffc0004>
   6:	743030        	extui	a3, a3, 0, 8
   9:	020482        	l8ui	a8, a4, 2
   c:	742020        	extui	a2, a2, 0, 8
   f:	010492        	l8ui	a9, a4, 1
  12:	1f1837        	beq	a8, a3, 35 <hidd_xfer_cb+0x35>
  15:	020c      	movi.n	a2, 0
  17:	159937        	bne	a9, a3, 30 <hidd_xfer_cb+0x30>
  1a:	000031        	l32r	a3, fffc001c <hidd_xfer_cb+0xfffc001c>
  1d:	120c      	movi.n	a2, 1
  1f:	d38c      	beqz.n	a3, 30 <hidd_xfer_cb+0x30>
  21:	0000b1        	l32r	a11, fffc0024 <hidd_xfer_cb+0xfffc0024>
  24:	f4c050        	extui	a12, a5, 0, 16
  27:	00a0a2        	movi	a10, 0
  2a:	000081        	l32r	a8, fffc002c <hidd_xfer_cb+0xfffc002c>
  2d:	0008e0        	callx8	a8
  30:	f01d      	retw.n
  32:	00          	.byte 00
  33:	00          	.byte 00
  34:	00          	.byte 00
  35:	e11987        	beq	a9, a8, 1a <hidd_xfer_cb+0x1a>
  38:	000031        	l32r	a3, fffc0038 <hidd_xfer_cb+0xfffc0038>
  3b:	0c0c      	movi.n	a12, 0
  3d:	0cbd      	mov.n	a11, a12
  3f:	0cad      	mov.n	a10, a12
  41:	f4e050        	extui	a14, a5, 0, 16
  44:	20d330        	or	a13, a3, a3
  47:	000081        	l32r	a8, fffc0048 <hidd_xfer_cb+0xfffc0048>
  4a:	0008e0        	callx8	a8
  4d:	0204b2        	l8ui	a11, a4, 2
  50:	02ad      	mov.n	a10, a2
  52:	0d4c      	movi.n	a13, 64
  54:	03cd      	mov.n	a12, a3
  56:	000081        	l32r	a8, fffc0058 <hidd_xfer_cb+0xfffc0058>
  59:	0008e0        	callx8	a8
  5c:	0a2d      	mov.n	a2, a10
  5e:	fff386        	j	30 <hidd_xfer_cb+0x30>
