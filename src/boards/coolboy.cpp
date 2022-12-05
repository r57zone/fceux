/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2018 CaH4e3, Cluster
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * SMD132 and SMD133 ASICs, MMC3 clones that can address up to 32 MiB of PRG-ROM, 256 KiB of CHR-RAM, and 8 KiB of WRAM.
 *
 * COOLBOY cartridges use registers at address $6xxx
 * MINDKIDS cartridges use a solder pad labelled "5/6K" to select between $5000 and $6000
 *
 * $xxx0
 * 7  bit  0
 * ---- ----
 * ABCC DEEE
 * |||| ||||
 * |||| |+++-- PRG offset (PRG A19, A18, A17)
 * |||| +----- Alternate CHR A17
 * ||++------- PRG offset (PRG A24, A23)
 * |+--------- PRG mask (PRG A17 from 0: MMC3; 1: offset)
 * +---------- CHR mask (CHR A17 from 0: MMC3; 1: alternate)
 *
 * $xxx1
 *
 * 7  bit  0
 * ---- ----
 * GHIJ KKLx
 * |||| |||
 * |||| ||+--- GNROM mode bank PRG size (1: 32 KiB bank, PRG A14=CPU A14; 0: 16 KiB bank, PRG A14=offset A14)
 * |||+-++---- PRG offset (in order: PRG A20, A22, A21)
 * ||+-------- PRG mask (PRG A20 from 0: offset; 1: MMC3)
 * |+--------- PRG mask (PRG A19 from 0: offset; 1: MMC3)
 * +---------- PRG mask (PRG A18 from 0: MMC3; 1: offset)
 *
 * $xxx2
 * 7  bit  0
 * ---- ----
 * xxxx MMMM
 *      ||||
 *      ++++-- CHR offset for GNROM mode (CHR A16, A15, A14, A13)
 *
 * $xxx3
 * 7  bit  0
 * ---- ----
 * NPxP QQRx
 * || | |||
 * || | +++--- PRG offset for GNROM mode (PRG A16, A15, A14)
 * || +------- 1: GNROM mode; 0: MMC3 mode
 * || |         (1: PRG A16...13 from QQ, L, R, CPU A14, A13 + CHR A16...10 from MMMM, PPU A12...10;
 * || |          0: PRG A16...13 from MMC3 + CHR A16...A10 from MMC3 )
 * |+-+------- Banking mode
 * |+--------- "Weird MMC3 mode"
 * +---------- Lockout (prevent further writes to these four registers, only works in MMC3 mode)
 *
 * Also some new cartridges from MINDKIDS have /WE and /OE pins connected to mapper,
 * which allows you to rewrite flash memory without soldering.
 * This also allows console to write data to the cartridge.
 * This behavior is not emulated.
 * No cart has been discovered so far that makes use of this feature, but this can be used for homebrew.
 *
 */

#include "mapinc.h"
#include "mmc3.h"

const int ROM_CHIP = 0x00;
const int WRAM_CHIP = 0x10;
const int CFI_CHIP = 0x11;
const int FLASH_CHIP = 0x12;

const int FLASH_SECTOR_SIZE = 128 * 1024;

extern uint8* WRAM;
static uint8* CFI = NULL;
static uint8* Flash = NULL;

static uint8 flash_save = 0;
static uint8 flash_state = 0;
static uint16 flash_buffer_a[10];
static uint8 flash_buffer_v[10];
static uint8 cfi_mode = 0;

// Macronix 256-mbit memory CFI data
const uint8 cfi_data[] =
{ 
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x51, 0x52, 0x59, 0x02, 0x00, 0x40, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x27, 0x36, 0x00, 0x00, 0x03,
	0x06, 0x09, 0x13, 0x03, 0x05, 0x03, 0x02, 0x19,
	0x02, 0x00, 0x06, 0x00, 0x01, 0xFF, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
	0x50, 0x52, 0x49, 0x31, 0x33, 0x14, 0x02, 0x01,
	0x00, 0x08, 0x00, 0x00, 0x02, 0x95, 0xA5, 0x05,
	0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static void COOLBOYCW(uint32 A, uint8 V) {
	uint32 mask = 0xFF ^ (EXPREGS[0] & 0x80);
	if (EXPREGS[3] & 0x10) {
		if (EXPREGS[3] & 0x40) { // Weird mode
			int cbase = (MMC3_cmd & 0x80) << 5;
			switch (cbase ^ A) { // Don't even try do understand
			case 0x0400:
			case 0x0C00: V &= 0x7F; break;
			}
		}
		// Highest bit goes from MMC3 registers when EXPREGS[3]&0x80==0 or from EXPREGS[0]&0x08 otherwise
		setchr1(A,
			(V & 0x80 & mask) | ((((EXPREGS[0] & 0x08) << 4) & ~mask)) // 7th bit
			| ((EXPREGS[2] & 0x0F) << 3) // 6-3 bits
			| ((A >> 10) & 7) // 2-0 bits
		);
	}
	else {
		if (EXPREGS[3] & 0x40) { // Weird mode, again
			int cbase = (MMC3_cmd & 0x80) << 5;
			switch (cbase ^ A) { // Don't even try do understand
			case 0x0000: V = DRegBuf[0]; break;
			case 0x0800: V = DRegBuf[1]; break;
			case 0x0400:
			case 0x0C00: V = 0; break;
			}
		}
		// Simple MMC3 mode
		// Highest bit goes from MMC3 registers when EXPREGS[3]&0x80==0 or from EXPREGS[0]&0x08 otherwise
		setchr1(A, (V & mask) | (((EXPREGS[0] & 0x08) << 4) & ~mask));
	}
}

static void COOLBOYPW(uint32 A, uint8 V) {
	uint32 mask = ((0x3F | (EXPREGS[1] & 0x40) | ((EXPREGS[1] & 0x20) << 2)) ^ ((EXPREGS[0] & 0x40) >> 2)) ^ ((EXPREGS[1] & 0x80) >> 2);
	uint32 base = ((EXPREGS[0] & 0x07) >> 0) | ((EXPREGS[1] & 0x10) >> 1) | ((EXPREGS[1] & 0x0C) << 2) | ((EXPREGS[0] & 0x30) << 2);

	if (cfi_mode)
	{
		setprg32r(CFI_CHIP, 0x8000, 0);
	}

	int chip = !flash_save ? ROM_CHIP : FLASH_CHIP;

	// Very weird mode
	// Last banks are first in this mode, ignored when MMC3_cmd&0x40
	if ((EXPREGS[3] & 0x40) && (V >= 0xFE) && !((MMC3_cmd & 0x40) != 0)) {
		switch (A & 0xE000) {
		case 0xC000:
		case 0xE000:
			V = 0;
			break;
		}
	}

	if (!(EXPREGS[3] & 0x10)) {
		// Regular MMC3 mode but can be extended to 2MByte
		setprg8r(chip, A, (((base << 4) & ~mask)) | (V & mask));
	}
	else {
		// NROM mode
		mask &= 0xF0;
		uint8 emask;
		if ((((EXPREGS[1] & 2) != 0))) // 32kb mode
			emask = (EXPREGS[3] & 0x0C) | ((A & 0x4000) >> 13);
		else // 16kb mode
			emask = EXPREGS[3] & 0x0E;
		setprg8r(chip, A, ((base << 4) & ~mask) // 7-4 bits are from base (see below)
			| (V & mask)                 // ... or from MM3 internal regs, depends on mask
			| emask                      // 3-1 (or 3-2 when (EXPREGS[3]&0x0C is set) from EXPREGS[3]
			| ((A & 0x2000) >> 13));     // 0th just as is
	}
}

static DECLFW(COOLBOYWrite) {
	if (A001B & 0x80)
		CartBW(A, V);

	// Deny any further writes when 7th bit is 1 AND 4th is 0
	if ((EXPREGS[3] & 0x90) != 0x80) {
		EXPREGS[A & 3] = V;
		FixMMC3PRG(MMC3_cmd);
		FixMMC3CHR(MMC3_cmd);
	}
}

static DECLFW(MINDKIDSWrite) {
	if (A >= 0x6000) {
		if (A001B & 0x80)
			CartBW(A, V);
		return;
	}

	// Deny any further writes when 7th bit is 1 AND 4th is 0
	if ((EXPREGS[3] & 0x90) != 0x80) {
		EXPREGS[A & 3] = V;
		FixMMC3PRG(MMC3_cmd);
		FixMMC3CHR(MMC3_cmd);
	}
}

static DECLFR(COOLBOYFlashRead) {
	return CartBR(A);
}

static DECLFW(COOLBOYFlashWrite) {
	if (A < 0xC000)
		MMC3_CMDWrite(A, V);
	else
		MMC3_IRQWrite(A, V);

	if (flash_save) {
		if (flash_state < sizeof(flash_buffer_a) / sizeof(flash_buffer_a[0])) {
			flash_buffer_a[flash_state] = A & 0xFFF;
			flash_buffer_v[flash_state] = V;
			flash_state++;

			// enter CFI mode
			if ((flash_state == 1) &&
				(flash_buffer_a[0] == 0x0AAA) && (flash_buffer_v[0] == 0x98)) {
				cfi_mode = 1;
				flash_state = 0;
				FixMMC3PRG(MMC3_cmd);
			}

			// erase sector
			if ((flash_state == 6) &&
				(flash_buffer_a[0] == 0x0AAA) && (flash_buffer_v[0] == 0xAA) &&
				(flash_buffer_a[1] == 0x0555) && (flash_buffer_v[1] == 0x55) &&
				(flash_buffer_a[2] == 0x0AAA) && (flash_buffer_v[2] == 0x80) &&
				(flash_buffer_a[3] == 0x0AAA) && (flash_buffer_v[3] == 0xAA) &&
				(flash_buffer_a[4] == 0x0555) && (flash_buffer_v[4] == 0x55) &&
				(flash_buffer_v[5] == 0x30)) {
				int offset = &Page[A >> 11][A] - Flash;
				int sector = offset / FLASH_SECTOR_SIZE;
				for (uint32 i = sector * FLASH_SECTOR_SIZE; i < (sector + 1) * FLASH_SECTOR_SIZE; i++)
					Flash[i % PRGsize[ROM_CHIP]] = 0xFF;
				FCEU_printf("Flash sector #%d is erased (0x%08x - 0x%08x)\n", sector, offset, offset + FLASH_SECTOR_SIZE);
			}

			// erase chip, lol
			if ((flash_state == 6) &&
				(flash_buffer_a[0] == 0x0AAA) && (flash_buffer_v[0] == 0xAA) &&
				(flash_buffer_a[1] == 0x0555) && (flash_buffer_v[1] == 0x55) &&
				(flash_buffer_a[2] == 0x0AAA) && (flash_buffer_v[2] == 0x80) &&
				(flash_buffer_a[3] == 0x0AAA) && (flash_buffer_v[3] == 0xAA) &&
				(flash_buffer_a[4] == 0x0555) && (flash_buffer_v[4] == 0x55) &&
				(flash_buffer_v[5] == 0x10)) {
				memset(Flash, 0xFF, PRGsize[ROM_CHIP]);
				FCEU_printf("Flash chip erased.\n");
			}

			// write byte
			if ((flash_state == 4) &&
				(flash_buffer_a[0] == 0x0AAA) && (flash_buffer_v[0] == 0xAA) &&
				(flash_buffer_a[1] == 0x0555) && (flash_buffer_v[1] == 0x55) &&
				(flash_buffer_a[2] == 0x0AAA) && (flash_buffer_v[2] == 0xA0)) {
				int offset = &Page[A >> 11][A] - Flash;
				if (CartBR(A) != 0xFF) {
					FCEU_PrintError("Error: can't write to 0x%08x, flash sector is not erased\n", offset);
				}
				else {
					CartBW(A, V);
				}
				flash_state = 0;
			}


		}

		// not a command
		if (((A & 0xFFF) != 0x0AAA) && ((A & 0xFFF) != 0x0555)) {
			flash_state = 0;
		}

		// reset
		if (V == 0xF0) {
			flash_state = 0;
			cfi_mode = 0;
			FixMMC3PRG(MMC3_cmd);
		}
	}
}

static void CommonReset(void) {
	MMC3RegReset();
	EXPREGS[0] = EXPREGS[1] = EXPREGS[2] = EXPREGS[3] = 0;
	flash_state = 0;
	cfi_mode = 0;
	FixMMC3PRG(MMC3_cmd);
	FixMMC3CHR(MMC3_cmd);
}

static void COOLBOYPower(void) {
	GenMMC3Power();
	EXPREGS[0] = EXPREGS[1] = EXPREGS[2] = EXPREGS[3] = 0;
	FixMMC3PRG(MMC3_cmd);
	FixMMC3CHR(MMC3_cmd);
	SetWriteHandler(0x5000, 0x5fff, CartBW);            // some games access random unmapped areas and crashes because of KT-008 PCB hack in MMC3 source lol
	SetWriteHandler(0x6000, 0x6fff, COOLBOYWrite);
	SetWriteHandler(0x8000, 0xFFFF, COOLBOYFlashWrite);
	SetReadHandler(0x8000, 0xFFFF, COOLBOYFlashRead);
}

static void MINDKIDSPower(void) {
	GenMMC3Power();
	EXPREGS[0] = EXPREGS[1] = EXPREGS[2] = EXPREGS[3] = 0;
	FixMMC3PRG(MMC3_cmd);
	FixMMC3CHR(MMC3_cmd);
	SetWriteHandler(0x5000, 0x7fff, MINDKIDSWrite);
	SetWriteHandler(0x8000, 0xFFFF, COOLBOYFlashWrite);
	SetReadHandler(0x8000, 0xFFFF, COOLBOYFlashRead);
}

static void CommonClose(void) {
	if (WRAM)
		FCEU_gfree(WRAM);
	if (Flash)
		FCEU_gfree(Flash);
	if (CFI) 
		FCEU_gfree(CFI);
	WRAM = Flash = CFI = NULL;
}

void CommonInit(CartInfo* info, int submapper)
{	
	GenMMC3_Init(info, 2048, info->vram_size / 1024, !info->ines2 ? 8 : (info->wram_size + info->battery_wram_size) / 1024, info->battery);
	pwrap = COOLBOYPW;
	cwrap = COOLBOYCW;

	switch (submapper)
	{
	case 1:
		info->Power = MINDKIDSPower;
	default:
		info->Power = COOLBOYPower;
		break;
	}
	info->Reset = CommonReset;
	info->Close = CommonClose;

	flash_save = info->battery;

	if (flash_save) {
		CFI = (uint8*)FCEU_gmalloc(sizeof(cfi_data) * 2);
		for (int i = 0; i < sizeof(cfi_data); i++) {
			CFI[i * 2] = CFI[i * 2 + 1] = cfi_data[i];
		}
		SetupCartPRGMapping(CFI_CHIP, CFI, sizeof(cfi_data) * 2, 0);

		Flash = (uint8*)FCEU_gmalloc(PRGsize[ROM_CHIP]);
		for (int i = 0; i < PRGsize[ROM_CHIP]; i++) {
			Flash[i] = PRGptr[ROM_CHIP][i % PRGsize[ROM_CHIP]];
		}
		SetupCartPRGMapping(FLASH_CHIP, Flash, PRGsize[ROM_CHIP], 1);
		info->SaveGame[1] = Flash;
		info->SaveGameLen[1] = PRGsize[ROM_CHIP];
	}

	AddExState(EXPREGS, 4, 0, "EXPR");
	if (flash_save)
	{
		AddExState(&flash_state, sizeof(flash_state), 0, "FLST");
		AddExState(flash_buffer_a, sizeof(flash_buffer_a), 0, "FLBA");
		AddExState(flash_buffer_v, sizeof(flash_buffer_v), 0, "FLBV");
		AddExState(&cfi_mode, sizeof(cfi_mode), 0, "CFIM");
		AddExState(Flash, PRGsize[ROM_CHIP], 0, "FLAS");
	}
}

// Registers at $6xxx
void COOLBOY_Init(CartInfo* info) {
	CommonInit(info, 0);
}

// Registers at $5xxx
void MINDKIDS_Init(CartInfo* info) {
	CommonInit(info, 1);
}

// For NES 2.0 loader
void SMD132_SMD133_Init(CartInfo* info) {

	switch (info->submapper)
	{
	case 0:
	case 1:
		CommonInit(info, info->submapper);
		break;
	default:
		FCEU_PrintError("Submapper #%d is not supported", info->submapper);
		break;
	}
}
