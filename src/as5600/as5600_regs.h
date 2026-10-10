/*
 * AS5600 register map. Every value cites the ams datasheet AS5600 [v1-06]
 * 2018-Jun-20, docs/AS5600_DS000365_5_00.pdf, by figure: the register map is
 * Figure 21, CONF Figure 22, STATUS Figure 23. Multi-byte values are
 * big-endian and the address pointer auto-increments, so each pair is one
 * read.
 *
 * In as5600.h: AS5600_ADDR, 0x36, is "I²C Interface" p.9; AS5600_COUNTS,
 * 4096, is the 12 bits of (RAW) ANGLE(11:0), Figure 21.
 */
#ifndef AS5600_REGS_H
#define AS5600_REGS_H

#define AS5600_CONF_H		0x07	/* WD, FTH(2:0), SF(1:0) */
#define AS5600_CONF_L		0x08	/* PWMF(1:0), OUTS(1:0), HYST(1:0), PM(1:0) */
#define AS5600_STATUS		0x0B
#define AS5600_RAW_ANGLE_H	0x0C	/* RAW ANGLE(11:8); (7:0) at 0x0D */
#define AS5600_ANGLE_H		0x0E	/* ANGLE(11:8); (7:0) at 0x0F */
#define AS5600_AGC		0x1A
#define AS5600_MAGNITUDE_H	0x1B	/* MAGNITUDE(11:8); (7:0) at 0x1C */

/* STATUS, Figures 21 and 23. */
#define AS5600_STATUS_MH	(1u << 3)	/* gain floor hit: magnet too strong */
#define AS5600_STATUS_ML	(1u << 4)	/* gain ceiling hit: magnet too weak */
#define AS5600_STATUS_MD	(1u << 5)	/* magnet detected */

/* CONF, Figure 22: SF is CONF bits 9:8, HYST bits 3:2. */
#define AS5600_CONF_SF_MASK	0x03u		/* CONF high byte, bits 1:0 */
#define AS5600_CONF_HYST_MASK	0x0Cu		/* CONF low byte, bits 3:2 */
#define AS5600_CONF_HYST_SHIFT	2

#endif /* AS5600_REGS_H */
