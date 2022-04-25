#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "pd_api/pd_api_gfx.h"
#include "pd_api.h"

LCDBitmap* bitmap;
LCDBitmap* gradient;

static char error_message[256] = "";

static int crash_handler(void* userdata)
{
    PlaydateAPI* pd = userdata;

    pd->graphics->clear(kColorBlack);
    static const char header[] = "- Runtime error -";
    pd->graphics->setDrawMode(kDrawModeInverted);
    pd->graphics->drawText(header, strlen(header), kASCIIEncoding, 0, 0);
    pd->graphics->drawText(error_message, strlen(error_message), kASCIIEncoding, 0, 24);

    return 1;
}

void register_error(PlaydateAPI* pd, const char* err)
{
    pd->system->error(err);
    snprintf(error_message, 254, "> %s", err);
    error_message[254] = '\0';
}

static bool init(PlaydateAPI* pd)
{
    const char* err = NULL;
    bitmap = pd->graphics->loadBitmap("barrel", &err);
    if (bitmap == NULL)
    {
        register_error(pd, err);
        return false;
    }

    gradient = pd->graphics->loadBitmap("gradient_bayer_big", &err);
    if (gradient == NULL)
    {
        register_error(pd, err);
        return false;
    }

    int width, height;
    int rowbytes;
    int mask;
    uint8_t* data;
    pd->graphics->getBitmapData(bitmap, &width, &height, &rowbytes, &mask, &data);
    return true;
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

    float x_offset = t * 60.f;

    // Use the crank to scroll if it's undocked, it's funnier this way.
    if (!pd->system->isCrankDocked())
    {
        x_offset = pd->system->getCrankAngle() / 360.f * width;
    }

    for (int y = 0; y < 240; ++y)
    {

        // Bitmap scale, that's the core the of effect. 
        // The scale and offset values could be hardcoded into arrays.
        // Feel free to tweak the factors to see how it changes but
        // note that the effect currently doesn't work with negative scale.
        float scale = sinf(y / 240.f * 3.141592653f) * 0.55f + 0.75f;
        // The scale-based offset makes the effect centered.
        // Try running without the offset, it'll look like it's centered on the
        // left-most pixel column.
        float base_x_offset = x_offset - scale * 200.f;


        // handling negative offset.
        float input_x_offset = fmodf(base_x_offset, (float)width);
        if (input_x_offset <= 0)
        {
            input_x_offset = width + input_x_offset;
        }

        // I don't know if it's faster than modf(x_offset, 1.0f), but I expect so.
        float frac = input_x_offset - truncf(input_x_offset);

        uint32_t input_bit_pointer = (uint32_t)(input_x_offset) % 8;
        uint32_t input_byte_offset = (uint32_t)(input_x_offset) / 8;

        int input_y = (int)(y_offset + y) % height;
        // Precompute the adress offset of the input's row.
        // Note that if I don't precompute it, it turns from a simple add to a MAC,
        // which are supposedly 1-cycle on Cortex-M7, so it might just be a waste of
        // a register.
        int input_y_row_offset = input_y * rowbytes;
        uint8_t* input_pointer = &data[input_y_row_offset + input_byte_offset];
        uint8_t input_byte = *input_pointer;

        uint8_t* output_pointer = &buffer[y * LCD_ROWSIZE];
        // Temporary byte to work on until I blit it directly to the framebuffer.
        // Might be purely useless, maybe optimized out. I need to tune this too.
        uint8_t output_acc = 0;

        // This could be split into double for loops instead,
        // for(x; x < LCD_COLUMNS; x+=8) for(bit; bit < 8; ++bit)
        uint8_t written_bits = 0;

        // A runtime hint for the CPU to load the next cacheline-width data at the adress.
        // I'm not sure if it has a performance impact, that's one of the things I want to check
        // once I have the hardware to test on.
#ifdef TARGET_PLAYDATE
        __builtin_prefetch(output_pointer);
        __builtin_prefetch(output_pointer + 32);
#endif
        for (int x = 0; x < LCD_COLUMNS; ++x)
        {

            // If the input must be advanced
            if (frac >= 1)
            {
                input_bit_pointer += (uint8_t)truncf(frac);
                frac = fmodf(frac, 1.f);
                // The current input byte is through, fetch the next one.
                if (input_bit_pointer >= 8)
                {
                    // >>3 and & 7 for the two next operations but I assume that
                    // the compiler is smart enough to figure it out.
                    //
                    // Update: GCC is smart enough to do the change.
                    int offset = input_bit_pointer / 8;
                    input_bit_pointer %= 8;
                    input_byte_offset = (uint8_t)((input_byte_offset + offset) % rowbytes);
                    input_pointer = &data[input_y_row_offset + input_byte_offset];
                    input_byte = *input_pointer;
                }
            }

            // Output data seek and write.
            // Tricky bit : Because the data is aligned as MSB data, we must take care of the way
            // we collect and apply bits. From left to right => from the MSB to the LSB.
            uint8_t selected_bit = (input_byte & (1 << (7 - input_bit_pointer))) != 0;

            output_acc |= selected_bit << (7 - written_bits);
            ++written_bits;

            // Write to the screen once we got a full byte.
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

    // Extra bit : fake depth shadowing with a repeated gradient.
    // This could be instead a mask applied on the drawing part in the barrel loop
    // but I wanted to see how some of the gradients would look without having to
    // program them.
    // I bundled a few different gradients if you want to test them.
    int gradient_width = 0;
    int gradient_height = 0;
    pd->graphics->getBitmapData(gradient, &gradient_width, &gradient_height, NULL, NULL, NULL);
    if (gradient_width != 0)
    {
        for (int x = 0; x < LCD_COLUMNS; x += gradient_width)
        {
            pd->graphics->drawBitmap(gradient, x, (LCD_ROWS - gradient_height) / 2, kBitmapUnflipped);
        }
    }

    // I'm not sure if it's really needed, given that I directly tap into the framebuffer.
    //pd->graphics->markUpdatedRows(0, LCD_ROWS - 1);
    
    
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
        if (init(pd))
        {
            pd->system->setUpdateCallback(update, pd);
        }
        else
        {
            pd->system->setUpdateCallback(crash_handler, pd);
        }

    }

    return 0;
}
