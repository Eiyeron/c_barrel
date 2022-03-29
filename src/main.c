#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <math.h>

#include "pd_api/pd_api_gfx.h"
#include "pd_api.h"

LCDBitmap* bitmap;

static void init(PlaydateAPI* pd)
{
	const char* err = NULL;
	bitmap = pd->graphics->loadBitmap("barrel", &err);
	if (bitmap == NULL)
	{
		pd->system->logToConsole(err);
		exit(1);
	}

	int width, height;
	int rowbytes;
	int mask;
	uint8_t* data;
	pd->graphics->getBitmapData(bitmap, &width, &height, &rowbytes, &mask, &data);

	pd->system->logToConsole("Loaded barrel. Stride : %d\n", rowbytes);
}

static int update(void* userdata)
{
	PlaydateAPI* pd = userdata;

	pd->graphics->clear(kColorWhite);

	int width, height;
	int rowbytes;
	int mask;
	uint8_t* data;
	pd->graphics->getBitmapData(bitmap, &width, &height, &rowbytes, &mask, &data);

	uint8_t* buffer = pd->graphics->getFrame();

	float t = pd->system->getElapsedTime();
	// Just to play around.
	// The routine doesn't work with negative scale.
	// And it should work only on bitmaps with a width dividable by 8.

	// Super Castlevania IV-like barrel effect.

	float y_offset = fmodf(sinf(t / 3.f) * 120.f, (float)height);
	// handling negative signs.
	if (y_offset <= 0)
	{
		y_offset = height + y_offset;
	}

	for (int y = 0; y < 240; ++y)
	{

		// The scale and offset values could be hardcoded into arrays.
		float scale = sinf(y / 240.f * 3.141592653f) * 0.25f + 0.5f;
		float base_x_offset = (t * 60.f) - scale * 200.f;


		// handling negative offset.
		float x_offset = fmodf(base_x_offset, (float)width);
		if (x_offset <= 0)
		{
			x_offset = width + x_offset;
		}

		float frac = x_offset - truncf(x_offset);

		uint8_t input_bit_pointer = (uint8_t)(x_offset) % 8;
		uint8_t input_byte_offset = (uint8_t)(x_offset) / 8;
		// TODO change that it assumes that the bitmap is as tall as the screen.

		int input_y = (int)(y_offset + y) % height;
		uint8_t* input_pointer = &data[input_y * rowbytes + input_byte_offset];
		uint8_t input_byte = *input_pointer;

		uint8_t* output_pointer = &buffer[y * LCD_ROWSIZE];
		uint8_t output_acc = 0; // Accumulator byte.


		uint8_t written_bits = 0;
		for (int x = 0; x < LCD_COLUMNS; ++x)
		{

			// Input data fetch.
			// TODO support scale > 1
			if (frac >= 1)
			{
				input_bit_pointer += (uint8_t)truncf(frac);
				frac = fmodf(frac, 1.f);

				if (input_bit_pointer >= 8)
				{
					// >>3 and & 7 for the two next operations but I assume that the compiler is smart enough to figure it out.
					int offset = input_bit_pointer / 8;
					input_bit_pointer %= 8;
					input_byte_offset = (uint8_t)((input_byte_offset + offset) % rowbytes);
					input_pointer = &data[input_y * rowbytes + input_byte_offset];
					input_byte = *input_pointer;
				}
			}

			// Output data seek and write.
			// Tricky bit : Because the data is aligned as MSB data, we must take care of the way
			// we collect and apply bits. From left to right => from the MSB to the LSB.
			uint8_t selected_bit = (input_byte & (1 << (7 - input_bit_pointer))) != 0;

			output_acc |= selected_bit << (7 - written_bits);
			++written_bits;

			if (written_bits == 8)
			{
				*output_pointer = output_acc;
				++output_pointer;
				output_acc = 0;
				written_bits = 0;
			}
			frac += scale;

		}
	}

	pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
	pd->system->drawFPS(0, 0);
	return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI* pd, PDSystemEvent event, uint32_t arg)
{
	(void)arg; // arg is currently only used for event = kEventKeyPressed




	if (event == kEventInit)
	{
		pd->system->setUpdateCallback(update, pd);
		init(pd);
	}

	return 0;
}
