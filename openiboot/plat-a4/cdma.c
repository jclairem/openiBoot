#include "openiboot.h"
#include "openiboot-asmhelpers.h"
#include "arm.h"
#include "util.h"
#include "malloc.h"
#include "hardware/dma.h"
#include "clock.h"
#include "timer.h"
#include "interrupt.h"
#include "mmu.h"

typedef struct dmaAES {
	uint32_t unkn0;
	uint32_t AESType;
	uint32_t key[8];
	uint32_t dataSize;
	void (*handler)(uint32_t dataBuffer, uint32_t dmaAES_setting2, uint32_t* unknAESSetting1);
	uint32_t dataBuffer;
} dmaAES;

typedef struct segmentBuffer {
	uint32_t address;
	uint32_t value;
	uint32_t offset;
	uint32_t length;
	uint32_t unknAESSetting1;
	uint32_t unknAESSetting2;
	uint32_t unknAESSetting3;
	uint32_t unknAESSetting4;
} segmentBuffer;

typedef struct DMAInfo {
	uint32_t signalled;
	uint32_t txrx_register;
	uint32_t unk_separator;
	segmentBuffer* segmentBuffer[32];
	uint32_t dmaSegmentNumber;
	uint32_t unsegmentedSize;
	uint32_t segmentationSetting;
	uint32_t segmentOffset;
	uint32_t previousSegmentOffset;
	uint32_t previousDmaSegmentNumber;
	uint32_t previousUnsegmentedSize;
	void (*handler)(int channel);
	int channel;
	uint32_t dataSize;
	dmaAES* dmaAESInfo;
	uint32_t dmaAES_channel;
	uint32_t dmaAES_setting2;
	uint32_t irq_state;
} DMAInfo;

typedef struct DMATxRxInfo {
	uint8_t unkn0;
	uint8_t unkn1;
	uint8_t unkn2;
	uint8_t unkn3;
	uint32_t unkn4;
} DMATxRxInfo;

static const DMATxRxInfo DMATxRx[8] = {
	{3, 4, 0, 0, 0x80000020},
	{3, 4, 0, 1, 0x800000A0},
	{3, 4, 0, 1, 0x800000A4},
	{5, 8, 1, 0, 0x81000020},
	{5, 8, 1, 1, 0x81200014},
	{5, 8, 1, 2, 0x81200018},
	{5, 8, 1, 3, 0x81300014},
	{5, 8, 1, 4, 0x81300018},
};

static DMAInfo dmaInfo[32];

static uint32_t dmaAES_channel_used = 0;

static void dmaIRQHandler(uint32_t token);
int dma_continue_async(int channel);

int dma_setup() {
	clock_gate_switch(0x14, ON);

	return 0;
}

int dma_channel_activate(int channel, uint32_t activate) {
	uint32_t status;
	uint32_t cmask;

	cmask = 1 << channel;
	status = GET_REG(DMA + DMA_STATUS) & cmask;
	if (status != activate) {
		if (activate)
			SET_REG(DMA, cmask);
		else
			SET_REG(DMA + DMA_OFF, cmask);
	}

	return status;
}

signed int dma_init_channel(uint8_t direction, uint32_t channel, int segmentationSetting, uint32_t txrx_register, uint32_t size, uint32_t Setting1Index, uint32_t Setting2Index, void* handler) {
	int i = 0;

	dma_channel_activate(channel, 1);

	DMAInfo* dma = &dmaInfo[channel];

	if (!dma->signalled) {
		*dma->segmentBuffer = memalign(0x20, 1024);

		if (!dma->segmentBuffer)
			system_panic("CDMA: can't allocate command chain");

		for (i = 0; i != 256; i += 8) {
			*((uint32_t*)dma->segmentBuffer + i) = get_physical_address((uint32_t)((uint32_t*)dma->segmentBuffer + i + 8));
		}

		dma->signalled = 1;
		dma->txrx_register = 0;
		dma->unk_separator = 0;

		interrupt_set_int_type(DMA_CHANNEL_INTERRUPT_BASE + channel, 0);
		interrupt_install(DMA_CHANNEL_INTERRUPT_BASE + channel, dmaIRQHandler, channel);
		interrupt_enable(DMA_CHANNEL_INTERRUPT_BASE + channel);
	}

	dma->irq_state = 0;

	if (txrx_register != dma->txrx_register) {
		for (i = 0; i < 8; i++) {
			if (txrx_register == DMATxRx[i].unkn4) {
				dma->txrx_register = txrx_register;
				dma->unk_separator = (DMATxRx[i].unkn3 & 0x3F) << 16;
				break;
			} else if (i == 7) {
				return -1;
			}
		}
	}

	dma->dmaSegmentNumber = 0;
	dma->segmentationSetting = segmentationSetting;
	dma->segmentOffset = 0;
	dma->dataSize = size;
	dma->unsegmentedSize = size;
	dma->handler = handler;
	dma->channel = channel;

	uint8_t Setting1;
	uint8_t Setting2;

	if ( Setting1Index == 1 ) {
		Setting1 = 0;
	} else if ( Setting1Index == 2 ) {
		Setting1 = 1 << 2;
	} else if ( Setting1Index == 4 ) {
		Setting1 = 1 << 3;
	} else {
		return -1;
	}

	if ((Setting2Index - 1) > 0x1F)
		return -1;

	switch (Setting2Index) {
		case 2:
			Setting2 = 0;
			break;
		case 3:
			Setting2 = 1 << 4;
			break;
		case 5:
			Setting2 = 1 << 5;
			break;
		case 9:
			Setting2 = 3 << 4;
			break;
		case 17:
			Setting2 = 1 << 6;
			break;
		case 33:
			Setting2 = 5 << 4;
			break;
		default:
			return -1;
	}

	uint32_t channel_reg = channel << 12;
	SET_REG(DMA + channel_reg, 2);

	uint8_t direction_setting;
	if (direction == 1) // if out
		direction_setting = 1 << 1;
	else
		direction_setting = 0 << 1;

	SET_REG(DMA + DMA_SETTINGS + channel_reg, dma->unk_separator | Setting1 | Setting2 | direction_setting);
	SET_REG(DMA + DMA_TXRX_REGISTER + channel_reg, txrx_register);
	SET_REG(DMA + DMA_SIZE + channel_reg, size);

	if (dma->dmaAESInfo)
		dma->dmaAES_setting2 = 0;

	dma_continue_async(channel);
	return 0;
}

int dma_continue_async(int channel) {
	uint32_t endOffset;
	uint8_t segmentId;
	uint32_t segmentLength;
	uint32_t value;
	DMAInfo* dma = &dmaInfo[channel];
	if (!dma->unsegmentedSize)
		system_panic("CDMA: ASSERT FAILED");
	dma->previousUnsegmentedSize = dma->unsegmentedSize;
	dma->previousDmaSegmentNumber = dma->dmaSegmentNumber;
	dma->previousSegmentOffset = dma->segmentOffset;
	if (dma->dmaAESInfo) {
		endOffset = dma->segmentationSetting + 8 * dma->dmaSegmentNumber;
		for (segmentId = 0; segmentId != 28; segmentId++) {
			if (!dma->unsegmentedSize)
				break;

			dma->segmentBuffer[segmentId]->value = 2;
			++segmentId;
			dma->dmaAESInfo->handler(dma->dmaAESInfo->dataBuffer, dma->dmaAES_setting2, &dma->segmentBuffer[segmentId]->unknAESSetting1);
			segmentLength = 0;
			++dma->dmaAES_setting2;

			int encryptedSegmentOffset;
			int encryptedSegmentOffsetEnd = 0;
			for (encryptedSegmentOffset = 0; encryptedSegmentOffset < dma->dmaAESInfo->dataSize; encryptedSegmentOffset += segmentLength) {
				encryptedSegmentOffsetEnd = dma->dmaAESInfo->dataSize;
				if (encryptedSegmentOffset >= encryptedSegmentOffsetEnd)
					break;

				segmentLength = endOffset + 4 - dma->segmentOffset;
				if (encryptedSegmentOffset + segmentLength > encryptedSegmentOffsetEnd)
					segmentLength = encryptedSegmentOffsetEnd - encryptedSegmentOffset;

				value = 0x10003;
				if (!encryptedSegmentOffset)
					value = 0x30003;

				dma->segmentBuffer[segmentId]->value = value;
				dma->segmentBuffer[segmentId]->offset = dma->segmentOffset + endOffset;
				dma->segmentBuffer[segmentId]->length = segmentLength;

				if (!segmentLength)
					system_panic("Caught trying to generate zero-length cdma segment on channel %d, irqState: %d", channel, dma->irq_state);

				dma->segmentOffset += segmentLength;
				if (dma->segmentOffset >= endOffset + 4 ) {
					endOffset += 8;
					++dma->dmaSegmentNumber;
					dma->segmentOffset = 0;
				}
				++segmentId;
			}
			dma->unsegmentedSize -= encryptedSegmentOffsetEnd;
		    }

		if (!dma->unsegmentedSize)
			dma->segmentBuffer[segmentId-1]->value |= 0x100;

		dma->segmentBuffer[segmentId]->value = 0;
	} else {
		for (segmentId = 0; segmentId < 31; segmentId++) {
			int segmentLength = dma->segmentationSetting + 8 * dma->dmaSegmentNumber + 4 - dma->segmentOffset;

			dma->segmentBuffer[segmentId]->value = 3;
			dma->segmentBuffer[segmentId]->offset = dma->segmentationSetting + 8 * dma->dmaSegmentNumber + dma->segmentOffset;
			dma->segmentBuffer[segmentId]->length = segmentLength;

			if (!segmentLength)
				system_panic("Caught trying to generate zero-length cdma segment on channel %d, irqState: %d", channel, dma->irq_state);

			dma->segmentOffset = 0;

			if (segmentLength >= dma->unsegmentedSize) {
				dma->segmentBuffer[segmentId]->value |= 0x100;
				dma->unsegmentedSize = 0;
				break;
			}

			dma->unsegmentedSize -= segmentLength;
			dma->dmaSegmentNumber++;
		}

		dma->segmentBuffer[segmentId+1]->value = 0;
	}

	DataCacheOperation(1, (uint32_t)dma->segmentBuffer, 32 * (segmentId + 2));

	uint32_t channel_reg = channel << 12;
	SET_REG(DMA + channel_reg + 0x14, get_physical_address((uint32_t)dma->segmentBuffer));

	value = 0x1C0009;

	if (dma->dmaAESInfo)
		value |= (dma->dmaAES_channel << 8);

	SET_REG(DMA + channel_reg, value);

	return (DMA + channel_reg);
}

int dma_set_aes(int channel, dmaAES* dmaAESInfo) {
        DMAInfo* dma = &dmaInfo[channel];
	int result = (int)dmaAESInfo;
	uint32_t value;
	int i;

	dma->dmaAESInfo = dmaAESInfo;

	if (!dma->dmaAESInfo)
		return result;

	int activation = dma_channel_activate(channel, 1);

	if (!dma->dmaAES_channel) {
		EnterCriticalSection();
		for (i = 2; i < 9; i++) {
			if (dmaAES_channel_used & (1 << i))
				continue;
			dmaAES_channel_used &= (1 << i);
			dma->dmaAES_channel = i;
		}
		LeaveCriticalSection();

		if (!dma->dmaAES_channel)
			system_panic("CDMA: no AES filter contexts: 0x%08x", dmaAES_channel_used);
	}

	int dmaAES_channel_reg = dma->dmaAES_channel << 12;

	value = (channel << 8) | 0x20000;
	if (!(dma->dmaAESInfo->unkn0 & 0xF))
		value |= 0x30000;


	if ((dma->dmaAESInfo->AESType & 0xFFF) > 256)
		return -1;

	switch(GET_BITS(dma->dmaAESInfo->AESType, 28, 4)) {
		case 2:				// AES 256
			value |= 0x80000;
			break;
		case 1:				// AES 192
			value |= 0x40000;
		case 0:				// AES 128
			break;
		default:			// Fail
			return -1;
	}

	if ((dma->dmaAESInfo->AESType & 0xFFF) < 256) {
		switch(GET_BITS(dma->dmaAESInfo->AESType, 28, 4)) {
			case 2:				// AES 256
				SET_REG(DMA + DMA_AES_KEY_7 + dmaAES_channel_reg, dma->dmaAESInfo->key[7]);
				SET_REG(DMA + DMA_AES_KEY_6 + dmaAES_channel_reg, dma->dmaAESInfo->key[6]);
			case 1:				// AES 192
	        		SET_REG(DMA + DMA_AES_KEY_5 + dmaAES_channel_reg, dma->dmaAESInfo->key[5]);
			        SET_REG(DMA + DMA_AES_KEY_4 + dmaAES_channel_reg, dma->dmaAESInfo->key[4]);
			case 0:				// AES 128
			        SET_REG(DMA + DMA_AES_KEY_3 + dmaAES_channel_reg, dma->dmaAESInfo->key[3]);
			        SET_REG(DMA + DMA_AES_KEY_2 + dmaAES_channel_reg, dma->dmaAESInfo->key[2]);
			        SET_REG(DMA + DMA_AES_KEY_1 + dmaAES_channel_reg, dma->dmaAESInfo->key[1]);
			        SET_REG(DMA + DMA_AES_KEY_0 + dmaAES_channel_reg, dma->dmaAESInfo->key[0]);
				value |= 0x100000;
				break;
			default:			// Fail
				return -1;
		}
	} else if ((dma->dmaAESInfo->AESType & 0xFFF) == 512) {
		value |= 0x200000;
	} else if ((dma->dmaAESInfo->AESType & 0xFFF) == 513) {
		value |= 0x400000;
	} else if ((dma->dmaAESInfo->AESType & 0xFFF) != 256) {
		return -1;
	}

	SET_REG(DMA + dmaAES_channel_reg + DMA_AES, value);

	dma_channel_activate(channel, activation);

	result = 0;
	return result;
}

int dma_cancel(int channel) {
        DMAInfo* dma = &dmaInfo[channel];
	uint64_t startTime = timer_get_system_microtime();

	if (!dma->signalled)
		return channel;

	dma_channel_activate(channel, 1);

	uint32_t channel_reg = DMA + (channel << 12);
	if (GET_BITS(GET_REG(channel_reg), 16, 2) == 1) {
		SET_REG(channel_reg, 2);

		while (GET_BITS(GET_REG(channel_reg), 16, 2) == 1) {
			if (has_elapsed(startTime, 10000))
				system_panic("CDMA: channel %d timeout during abort", channel);
		}
	}

	dma->signalled = 1;
	dma_set_aes(channel, 0);

	return dma_channel_activate(channel, 0);
}

void dmaIRQHandler(uint32_t token) {
	uint32_t wholeSegment;
	uint32_t currentSegment;
	uint8_t segmentId;
	int channel = token;
	uint32_t channel_reg = channel << 12;
	DMAInfo* dma = &dmaInfo[channel];

	if (GET_REG(DMA + channel_reg) & 0x40000)
		system_panic("CDMA: channel %d error interrupt, error status 0x%0x", channel, GET_REG(DMA + DMA_INTERRUPT_ERROR + channel_reg));

	if (GET_REG(DMA + channel_reg) & 0x100000)
		system_panic("CDMA: channel %d spurious CIR", channel);

	SET_REG(DMA + channel_reg, 0x80000);

	if (dma->unsegmentedSize || GET_REG(DMA + DMA_SIZE + channel_reg)) {
		if (GET_REG(DMA + channel_reg) & 0x30000) {
			dma->unsegmentedSize = GET_REG(DMA + DMA_SIZE + channel_reg) + dma->unsegmentedSize;
			wholeSegment = dma->previousUnsegmentedSize - dma->unsegmentedSize;

			for (segmentId = 0; segmentId != 32; segmentId++) {
				currentSegment = GET_REG(8 * (dma->previousDmaSegmentNumber + segmentId) + dma->segmentationSetting + 4) - dma->previousSegmentOffset;
				if (wholeSegment <= currentSegment)
					break;
				wholeSegment -= currentSegment;
				dma->previousSegmentOffset = 0;
			}

			dma->dmaSegmentNumber = segmentId + dma->previousDmaSegmentNumber;
			dma->segmentOffset = dma->previousSegmentOffset + wholeSegment;
			dma->irq_state = 1;
		} else {
			dma->irq_state = 2;
		}
		dma_continue_async(channel);
	} else {
		dma->signalled = 1;
		dma_set_aes(channel, 0);

		dma_channel_activate(channel, 0);

		if (dma->handler)
			dma->handler(dma->channel);
	}
}
