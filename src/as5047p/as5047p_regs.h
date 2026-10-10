/*
 * AS5047P register map and SPI frames. Every value cites the ams datasheet
 * AS5047P [v2-00] 2021-Jul-22, docs/AS5047P_v2-00.pdf, by figure.
 *
 * Reads are pipelined: a frame returns the data for the address sent in the
 * *previous* frame (Figure 15), so every read is two frames with a NOP read
 * behind it. Commands execute on the rising edge of CSn ("SPI Interface
 * (slave)", p.11), so each 16-bit frame needs its own chip select.
 *
 * In as5047p.h: AS5047P_COUNTS, 16384, is the 14 bits of ANGLECOM and
 * ANGLEUNC (Figures 23, 24).
 */
#ifndef AS5047P_REGS_H
#define AS5047P_REGS_H

/* Volatile registers, Figure 18; each address is 14 bits. */
#define AS5047P_NOP		0x0000
#define AS5047P_ERRFL		0x0001	/* error flags, Figure 19 */
#define AS5047P_DIAAGC		0x3FFC
#define AS5047P_MAG		0x3FFD
#define AS5047P_ANGLEUNC	0x3FFE	/* measured angle, no compensation */
#define AS5047P_ANGLECOM	0x3FFF	/* measured angle with DAEC applied */

/* Command frame, Figure 13: PARC bit 15, R/W bit 14 (1 = read), ADDR 13:0. */
#define AS5047P_CMD_READ	(1u << 14)
/* PARC (Figure 13) and PARD (Figures 14, 16): even parity over bits 14:0. */
#define AS5047P_FRAME_PARITY	(1u << 15)
/* Read data frame, Figure 14: EF bit 14, set if the previous command erred. */
#define AS5047P_FRAME_ERROR	(1u << 14)
#define AS5047P_FRAME_DATA	0x3FFFu		/* DATA 13:0, Figure 14 */

/* DIAAGC, Figure 21. */
#define AS5047P_DIAAGC_AGC	0x00FFu		/* AGC 7:0 */
#define AS5047P_DIAAGC_LF	(1u << 8)	/* offset compensation finished */
#define AS5047P_DIAAGC_COF	(1u << 9)	/* CORDIC overflow: angle is garbage */
#define AS5047P_DIAAGC_MAGH	(1u << 10)	/* field too high: magnet too close */
#define AS5047P_DIAAGC_MAGL	(1u << 11)	/* field too low: magnet too far */

/*
 * "The AS5047P SPI uses mode=1 (CPOL=0, CPHA=1)", "SPI Interface (slave)",
 * p.11, which also gives the 10 MHz top clock; MSB first.
 */
#define AS5047P_SPI_MODE	1

#endif /* AS5047P_REGS_H */
