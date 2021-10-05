#include "vdp.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "clowncommon.h"

#include "error.h"

enum
{
	SHADOW_HIGHLIGHT_NORMAL = 0 << 6,
	SHADOW_HIGHLIGHT_SHADOW = 1 << 6,
	SHADOW_HIGHLIGHT_HIGHLIGHT = 2 << 6
};

/* Some of the logic here is based on research done by Nemesis:
   https://gendev.spritesmind.net/forum/viewtopic.php?p=21016#p21016 */

/* This essentially pre-computes the VDP's depth-test and alpha-test,
   generating a lookup table to eliminate the need to perform these
   every time a pixel is blitted. This provides a *massive* speed boost. */
/* TODO - Maybe make this into a giant pre-computed array so that it
   doesn't have to take up space in the state struct? */
static void InitBlitLookupTable(VDP_State *state)
{
	const unsigned int palette_line_index_mask = 0xF;
	const unsigned int colour_index_mask = 0x3F;
	const unsigned int priority_mask = 0x40;
	const unsigned int not_shadowed_mask = 0x80;

	unsigned int old, new;

	for (old = 0; old < CC_COUNT_OF(state->blit_lookup); ++old)
	{
		for (new = 0; new < CC_COUNT_OF(state->blit_lookup[0]); ++new)
		{
			const unsigned int old_palette_line_index = old & palette_line_index_mask;
			const unsigned int old_colour_index = old & colour_index_mask;
			const cc_bool old_priority = old & priority_mask;
			const cc_bool old_not_shadowed = !!(old & not_shadowed_mask);

			const unsigned int new_palette_line_index = new & palette_line_index_mask;
			const unsigned int new_colour_index = new & colour_index_mask;
			const cc_bool new_priority = new & priority_mask;
			const cc_bool new_not_shadowed = new_priority;

			const cc_bool draw_new_pixel = new_palette_line_index != 0 && (old_palette_line_index == 0 || !old_priority || new_priority);

			unsigned char output;

			/* First, generate the table for regular blitting */
			output = draw_new_pixel ? new : old;

			output |= old_not_shadowed || new_not_shadowed ? not_shadowed_mask : 0;

			state->blit_lookup[old][new] = output;

			/* Now, generate the table for shadow/highlight blitting */
			if (draw_new_pixel)
			{
				/* Sprite goes on top of plane */
				if (new_colour_index == 0x3E)
				{
					/* Transparent highlight pixel */
					output = old_colour_index | (old_not_shadowed ? SHADOW_HIGHLIGHT_HIGHLIGHT : SHADOW_HIGHLIGHT_NORMAL);
				}
				else if (new_colour_index == 0x3F)
				{
					/* Transparent shadow pixel */
					output = old_colour_index | SHADOW_HIGHLIGHT_SHADOW;
				}
				else if (new_palette_line_index == 0xE)
				{
					/* Always-normal pixel */
					output = new_colour_index | SHADOW_HIGHLIGHT_NORMAL;
				}
				else
				{
					/* Regular sprite pixel */
					output = new_colour_index | (new_not_shadowed || old_not_shadowed ? SHADOW_HIGHLIGHT_NORMAL : SHADOW_HIGHLIGHT_SHADOW);
				}
			}
			else
			{
				/* Plane goes on top of sprite */
				output = old_colour_index | (old_not_shadowed ? SHADOW_HIGHLIGHT_NORMAL : SHADOW_HIGHLIGHT_SHADOW);
			}

			state->blit_lookup_shadow_highlight[old][new] = output;
		}
	}
}

static void WriteAndIncrement(VDP_State *state, unsigned short value)
{
	unsigned int index = state->access.index / 2;

	switch (state->access.selected_buffer)
	{
		case VDP_ACCESS_VRAM:
		{
			/* Update sprite cache if we're writing to the sprite table */
			const unsigned int sprite_table_index = index - state->sprite_table_address;

			if (sprite_table_index < (state->h40_enabled ? 80 : 64) * 4 && !(sprite_table_index & 2))
			{
				state->sprite_table_cache[sprite_table_index / 4][sprite_table_index & 1] = value;
				state->sprite_row_cache.needs_updating = cc_true;
			}

			state->vram[index & (CC_COUNT_OF(state->vram) - 1)] = value;

			break;
		}

		case VDP_ACCESS_CRAM:
		{
			/* Convert colour to RGB24 now so we don't have to do it while blitting */
			const unsigned int red = (value >> 1) & 7;
			const unsigned int green = (value >> 5) & 7;
			const unsigned int blue = (value >> 9) & 7;

			const unsigned int red_8bit = (red << 5) | (red << 2) | (red >> 1);
			const unsigned int green_8bit = (green << 5) | (green << 2) | (green >> 1);
			const unsigned int blue_8bit = (blue << 5) | (blue << 2) | (blue >> 1);

			index &= (CC_COUNT_OF(state->cram) - 1);

			/* Create normal colour */
			/* (leave as-is) */
			state->cram_native[SHADOW_HIGHLIGHT_NORMAL | index][0] = red_8bit;
			state->cram_native[SHADOW_HIGHLIGHT_NORMAL | index][1] = green_8bit;
			state->cram_native[SHADOW_HIGHLIGHT_NORMAL | index][2] = blue_8bit;

			/* Create shadow colour */
			/* (divide by two and leave in lower half of colour range) */
			state->cram_native[SHADOW_HIGHLIGHT_SHADOW | index][0] = red_8bit / 2;
			state->cram_native[SHADOW_HIGHLIGHT_SHADOW | index][1] = green_8bit / 2;
			state->cram_native[SHADOW_HIGHLIGHT_SHADOW | index][2] = blue_8bit / 2;

			/* Create highlight colour */
			/* (divide by two and move to upper half of colour range) */
			state->cram_native[SHADOW_HIGHLIGHT_HIGHLIGHT | index][0] = red_8bit / 2 + 0x80;
			state->cram_native[SHADOW_HIGHLIGHT_HIGHLIGHT | index][1] = green_8bit / 2 + 0x80;
			state->cram_native[SHADOW_HIGHLIGHT_HIGHLIGHT | index][2] = blue_8bit / 2 + 0x80;

			/* Finally store regular Mega Drive-format colour */
			state->cram[index] = value;

			break;
		}

		case VDP_ACCESS_VSRAM:
			state->vsram[index & (CC_COUNT_OF(state->vsram) - 1)] = value;
			break;
	}

	state->access.index += state->access.increment;
}

static unsigned short ReadAndIncrement(VDP_State *state)
{
	unsigned short value;

	const unsigned int index = state->access.index / 2;

	switch (state->access.selected_buffer)
	{
		case VDP_ACCESS_VRAM:
			value = state->vram[index & (CC_COUNT_OF(state->vram) - 1)];
			break;

		case VDP_ACCESS_CRAM:
			value = state->cram[index & (CC_COUNT_OF(state->cram) - 1)];
			break;

		case VDP_ACCESS_VSRAM:
			value = state->vsram[index & (CC_COUNT_OF(state->vsram) - 1)];
			break;

		default:
			value = 0;
			break;
	}

	state->access.index += state->access.increment;

	return value;
}

void VDP_Init(VDP_State *state)
{
	state->access.write_pending = cc_false;

	state->access.read_mode = cc_false;
	state->access.selected_buffer = VDP_ACCESS_VRAM;
	state->access.index = 0;
	state->access.increment = 0;

	state->dma.enabled = cc_false;
	state->dma.pending = cc_false;
	state->dma.source_address_high = 0;
	state->dma.source_address_low = 0;

	state->plane_a_address = 0;
	state->plane_b_address = 0;
	state->window_address = 0;
	state->sprite_table_address = 0;
	state->hscroll_address = 0;

	state->plane_width = 32;
	state->plane_height = 32;
	state->plane_width_bitmask = state->plane_width - 1;
	state->plane_height_bitmask = state->plane_height - 1;

	state->display_enabled = cc_false;
	state->v_int_enabled = cc_false;
	state->h_int_enabled = cc_false;
	state->h40_enabled = cc_false;
	state->v30_enabled = cc_false;
	state->shadow_highlight_enabled = cc_false;
	state->interlace_mode_2_enabled = cc_false;

	state->background_colour = 0;
	state->h_int_interval = 0;
	state->currently_in_vblank = 0;

	state->hscroll_mode = VDP_HSCROLL_MODE_FULL;
	state->vscroll_mode = VDP_VSCROLL_MODE_FULL;

	state->sprite_row_cache.needs_updating = cc_true;

	InitBlitLookupTable(state);
}

void VDP_RenderScanline(VDP_State *state, unsigned short scanline, void (*scanline_rendered_callback)(unsigned short scanline, void *pixels, unsigned short screen_width, unsigned short screen_height))
{
	unsigned int i;

	/* This is multipled by 3 because it holds RGB values */
	unsigned char pixels[VDP_MAX_SCANLINE_WIDTH * 3];

	const unsigned int tile_height_power = state->interlace_mode_2_enabled ? 4 : 3;
	const unsigned int tile_height_mask = (1 << tile_height_power) - 1;
	const unsigned int tile_size = (8 << tile_height_power) / 4;

	assert(scanline < VDP_MAX_SCANLINES);

	if (state->display_enabled)
	{
		#define MAX_SPRITE_WIDTH (8 * 4)

		/* ***** *
		 * Setup *
		 * ***** */

		unsigned int sprite_limit = state->h40_enabled ? 20 : 16;
		unsigned int pixel_limit = state->h40_enabled ? 320 : 256;

		/* The original hardware has a bug where if you use V-scroll and H-scroll at the same time,
		   the partially off-screen leftmost column will use an invalid V-scroll value.
		   To simulate this, 16 extra pixels are rendered at the left size of the scanline.
		   Depending on the H-scroll value, these pixels may appear on the left side of the screen.
		   This is done by offsetting where the pixels start being written to the buffer.
		   This is why the buffer has an extra 16 bytes at the beginning, and an extra 15 bytes at
		   the end.
		   Also of note is that this function renders a tile's entire width, regardless of whether
		   it's fully on-screen or not. This is why VDP_MAX_SCANLINE_WIDTH is rounded up to 8.
		   In both cases, these extra bytes exist to catch the 'overflow' values that are written
		   outside the visible portion of the buffer. */
		unsigned char plane_metapixels[16 + (VDP_MAX_SCANLINE_WIDTH + (8 - 1)) / 8 * 8 + (16 - 1)];

		/* The padding bytes of the left and right are for allowing sprites to overdraw at the
		   edges of the screen. */
		unsigned char sprite_metapixels[(MAX_SPRITE_WIDTH - 1) + VDP_MAX_SCANLINE_WIDTH + (MAX_SPRITE_WIDTH - 1)];

		/* Fill the scanline buffer with the background colour */
		memset(plane_metapixels, state->background_colour, sizeof(plane_metapixels));

		/* *********** */
		/* Draw planes */
		/* *********** */
		for (i = 2 ; i-- > 0; )
		{
			/* The extra two tiles on the left of the scanline */
			const unsigned int EXTRA_TILES = 2;

			unsigned int j;
			unsigned char *metapixels_pointer;
			unsigned int hscroll;
			unsigned int hscroll_scroll_offset;
			unsigned int plane_x_offset;

			/* Get the horizontal scroll value */
			switch (state->hscroll_mode)
			{
				default:
				case VDP_HSCROLL_MODE_FULL:
					hscroll = state->vram[state->hscroll_address + i];
					break;

				case VDP_HSCROLL_MODE_1CELL:
					hscroll = state->vram[state->hscroll_address + (scanline >> tile_height_power) * 2 + i];
					break;

				case VDP_HSCROLL_MODE_1LINE:
					hscroll = state->vram[state->hscroll_address + (scanline >> state->interlace_mode_2_enabled) * 2 + i];
					break;
			}

			/* Get the value used to offset the writes to the metapixel buffer */
			hscroll_scroll_offset = hscroll % 16;

			/* Get the value used to offset the reads from the plane map */
			plane_x_offset = -EXTRA_TILES + -((hscroll - hscroll_scroll_offset) / 8);

			/* Obtain the pointer used to write metapixels to the buffer */
			metapixels_pointer = plane_metapixels + hscroll_scroll_offset;

			/* Render tiles */
			for (j = 0; j < (VDP_MAX_SCANLINE_WIDTH + (8 - 1)) / 8 + EXTRA_TILES; ++j)
			{
				unsigned int k;

				unsigned int vscroll;

				unsigned int pixel_y_in_plane;
				unsigned int tile_x;
				unsigned int tile_y;
				unsigned int pixel_y_in_tile;

				unsigned int tile_metadata;
				cc_bool tile_priority;
				unsigned int tile_palette_line;
				cc_bool tile_y_flip;
				cc_bool tile_x_flip;
				unsigned int tile_index;

				/* Get the vertical scroll value */
				switch (state->vscroll_mode)
				{
					default:
					case VDP_VSCROLL_MODE_FULL:
						vscroll = state->vsram[i];
						break;

					case VDP_VSCROLL_MODE_2CELL:
						vscroll = state->vsram[(((-EXTRA_TILES + j) / 2) * 2 + i) % CC_COUNT_OF(state->vsram)];
						break;
				}

				/* Get the Y coordinate of the pixel in the plane */
				pixel_y_in_plane = vscroll + scanline;

				/* Get the coordinates of the tile in the plane */
				tile_x = (plane_x_offset + j) & state->plane_width_bitmask;
				tile_y = (pixel_y_in_plane >> tile_height_power) & state->plane_height_bitmask;

				/* Obtain and decode tile metadata */
				tile_metadata = state->vram[(i ? state->plane_b_address : state->plane_a_address) + tile_y * state->plane_width + tile_x];
				tile_priority = !!(tile_metadata & 0x8000);
				tile_palette_line = (tile_metadata >> 13) & 3;
				tile_y_flip = !!(tile_metadata & 0x1000);
				tile_x_flip = !!(tile_metadata & 0x0800);
				tile_index = tile_metadata & 0x7FF;

				/* Get the Y coordinate of the pixel in the tile */
				pixel_y_in_tile = (pixel_y_in_plane & tile_height_mask) ^ (tile_y_flip ? tile_height_mask : 0);

				/* TODO - Unroll this loop? */
				for (k = 0; k < 8; ++k)
				{
					/* Get the X coordinate of the pixel in the tile */
					const unsigned int pixel_x_in_tile = k ^ (tile_x_flip ? 7 : 0);

					/* Get raw tile data that contains the desired metapixel */
					const unsigned int tile_data = state->vram[tile_index * tile_size + pixel_y_in_tile * 2 + pixel_x_in_tile / 4];

					/* Obtain the index into the palette line */
					const unsigned int palette_line_index = (tile_data >> (4 * ((pixel_x_in_tile & 3) ^ 3))) & 0xF;

					/* Merge the priority, palette line, and palette line index into the complete indexed pixel */
					const unsigned int metapixel = (tile_priority << 6) | (tile_palette_line << 4) | palette_line_index;

					*metapixels_pointer = state->blit_lookup[*metapixels_pointer][metapixel];
					++metapixels_pointer;
				}
			}
		}

		/* ************ *
		 * Draw sprites *
		 * ************ */

		/* Update the sprite row cache if needed */
		if (state->sprite_row_cache.needs_updating)
		{
			/* Caching and preprocessing some of the sprite table allows the renderer to avoid
			   scanning the entire sprite table every time it renders a scanline. The VDP actually
			   partially caches its sprite data too, though I don't know if it's for the same purpose. */
			const unsigned int max_sprites = state->h40_enabled ? 80 : 64;

			unsigned int sprite_index;
			unsigned int sprites_remaining = max_sprites;

			state->sprite_row_cache.needs_updating = cc_false;

			/* Make it so we write to the start of the rows */
			for (i = 0; i < CC_COUNT_OF(state->sprite_row_cache.rows); ++i)
				state->sprite_row_cache.rows[i].total = 0;

			sprite_index = 0;

			do
			{
				/* Decode sprite data */
				const unsigned short *cached_sprite = state->sprite_table_cache[sprite_index];
				const unsigned int y = cached_sprite[0] & 0x3FF;
				const unsigned int width = ((cached_sprite[1] >> 10) & 3) + 1;
				const unsigned int height = ((cached_sprite[1] >> 8) & 3) + 1;
				const unsigned int link = cached_sprite[1] & 0x7F;

				const unsigned int blank_lines = 128 << state->interlace_mode_2_enabled;

				/* This loop only processes rows that are on-screen, and haven't been drawn yet */
				for (i = CC_MAX(blank_lines + scanline, y); i < CC_MIN(blank_lines + ((state->v30_enabled ? 30 : 28) << tile_height_power), y + (height << tile_height_power)); ++i)
				{
					struct VDP_SpriteRowCacheRow *row = &state->sprite_row_cache.rows[i - blank_lines];

					/* Don't write more sprites than are allowed to be drawn on this line */
					if (row->total != (state->h40_enabled ? 20 : 16))
					{
						struct VDP_SpriteRowCacheEntry *sprite_row_cache_entry = &row->sprites[row->total++];

						sprite_row_cache_entry->table_index = sprite_index;
						sprite_row_cache_entry->width = width;
						sprite_row_cache_entry->height = height;
						sprite_row_cache_entry->y_in_sprite = i - y;
					}
				}

				if (link >= max_sprites)
				{
					/* Invalid link - bail before it can cause a crash.
					   According to Nemesis, this is actually what real hardware does too:
					   http://gendev.spritesmind.net/forum/viewtopic.php?p=8364#p8364 */
					break;
				}

				sprite_index = link;
			}
			while (sprite_index != 0 && --sprites_remaining != 0);
		}

		/* Clear the scanline buffer, so that the sprite blitter
		   knows which pixels haven't been drawn yet. */
		memset(sprite_metapixels, 0, sizeof(sprite_metapixels));

		/* Render sprites */
		for (i = 0; i < state->sprite_row_cache.rows[scanline].total; ++i)
		{
			struct VDP_SpriteRowCacheEntry *sprite_row_cache_entry = &state->sprite_row_cache.rows[scanline].sprites[i];

			/* Decode sprite data */
			const unsigned short *sprite = &state->vram[state->sprite_table_address + sprite_row_cache_entry->table_index * 4];
			const unsigned int width = sprite_row_cache_entry->width;
			const unsigned int height = sprite_row_cache_entry->height;
			const unsigned int tile_metadata = sprite[2];
			const unsigned int x = sprite[3] & 0x1FF;

			/* Decode tile metadata */
			const cc_bool tile_priority = !!(tile_metadata & 0x8000);
			const unsigned int tile_palette_line = (tile_metadata >> 13) & 3;
			const cc_bool tile_y_flip = !!(tile_metadata & 0x1000);
			const cc_bool tile_x_flip = !!(tile_metadata & 0x0800);
			const unsigned int tile_index_base = tile_metadata & 0x7FF;

			unsigned int y_in_sprite = sprite_row_cache_entry->y_in_sprite;

			/* TODO - sprites only mask if another sprite rendered before them on the
			   current scanline, or if the previous scanline reached its pixel limit */
			/* This is a masking sprite: prevent all remaining sprites from being drawn */
			if (x == 0)
				break;

			if (x + width * 8 > 128 && x < 128 + (state->h40_enabled ? 40 : 32) * 8 - 1)
			{
				unsigned int j;

				unsigned char *metapixels_pointer = sprite_metapixels + (MAX_SPRITE_WIDTH - 1) + x - 128;

				y_in_sprite = tile_y_flip ? (height << tile_height_power) - y_in_sprite - 1 : y_in_sprite;

				for (j = 0; j < width; ++j)
				{
					unsigned int k;

					const unsigned int x_in_sprite = tile_x_flip ? width - j - 1 : j;
					const unsigned int tile_index = tile_index_base + (y_in_sprite >> tile_height_power) + x_in_sprite * height;
					const unsigned int pixel_y_in_tile = y_in_sprite & tile_height_mask;

					for (k = 0; k < 8; ++k)
					{
						/* Get the X coordinate of the pixel in the tile */
						const unsigned int pixel_x_in_tile = k ^ (tile_x_flip ? 7 : 0);

						/* Get raw tile data that contains the desired metapixel */
						const unsigned int tile_data = state->vram[tile_index * tile_size + pixel_y_in_tile * 2 + pixel_x_in_tile / 4];

						/* Obtain the index into the palette line */
						const unsigned int palette_line_index = (tile_data >> (4 * ((pixel_x_in_tile & 3) ^ 3))) & 0xF;

						/* Merge the priority, palette line, and palette line index into the complete indexed pixel */
						const unsigned int metapixel = (tile_priority << 6) | (tile_palette_line << 4) | palette_line_index;

						*metapixels_pointer |= metapixel & -(unsigned int)(*metapixels_pointer == 0) & -(unsigned int)(palette_line_index != 0);
						++metapixels_pointer;

						if (--pixel_limit == 0)
							goto DoneWithSprites;
					}
				}
			}
			else
			{
				if (pixel_limit <= width * 8)
					break;

				pixel_limit -= width * 8;
			}

			if (--sprite_limit == 0)
				break;
		}
	DoneWithSprites:

		/* ******************************************** *
		 * Colourise output and send it to be displayed *
		 * ******************************************** */

		/* Convert the metapixels to RGB pixels */
		if (state->shadow_highlight_enabled)
		{
			for (i = 0; i < VDP_MAX_SCANLINE_WIDTH; ++i)
			{
				const unsigned char pixel = state->blit_lookup_shadow_highlight[plane_metapixels[16 + i]][sprite_metapixels[(MAX_SPRITE_WIDTH - 1) + i]];

				const unsigned char *colour = state->cram_native[pixel];

				pixels[i * 3 + 0] = colour[0];
				pixels[i * 3 + 1] = colour[1];
				pixels[i * 3 + 2] = colour[2];
			}
		}
		else
		{
			for (i = 0; i < VDP_MAX_SCANLINE_WIDTH; ++i)
			{
				const unsigned char pixel = state->blit_lookup[plane_metapixels[16 + i]][sprite_metapixels[(MAX_SPRITE_WIDTH - 1) + i]];

				const unsigned char *colour = state->cram_native[pixel & 0x3F];

				pixels[i * 3 + 0] = colour[0];
				pixels[i * 3 + 1] = colour[1];
				pixels[i * 3 + 2] = colour[2];
			}
		}

		#undef MAX_SPRITE_WIDTH
	}
	else
	{
		const unsigned char *colour = state->cram_native[state->background_colour];

		for (i = 0; i < VDP_MAX_SCANLINE_WIDTH; ++i)
		{
			pixels[i * 3 + 0] = colour[0];
			pixels[i * 3 + 1] = colour[1];
			pixels[i * 3 + 2] = colour[2];
		}
	}

	/* Send the RGB pixels to be rendered */
	scanline_rendered_callback(scanline, pixels, (state->h40_enabled ? 40 : 32) * 8, (state->v30_enabled ? 30 : 28) << tile_height_power);
}

unsigned short VDP_ReadData(VDP_State *state)
{
	unsigned short value = 0;

	if (!state->access.read_mode)
	{
		/* According to GENESIS SOFTWARE DEVELOPMENT MANUAL (COMPLEMENT) section 4.1,
		   this should cause the 68k to hang */
		/* TODO */
		PrintError("Data was read from the VDP data port while the VDP was in write mode");
	}
	else
	{
		value = ReadAndIncrement(state);
	}

	return value;
}

unsigned short VDP_ReadControl(VDP_State *state)
{
	/* TODO */

	/* Reading from the control port aborts the VDP waiting for a second command word to be written.
	   This doesn't appear to be documented in the official SDK manuals we have, but the official
	   boot code makes use of this feature. */
	state->access.write_pending = cc_false;

	/* Set the 'V-blanking' and 'H-blanking' bits */
	return (state->currently_in_vblank << 3) | (1 << 2);
}

void VDP_WriteData(VDP_State *state, unsigned short value)
{
	if (state->access.read_mode)
	{
		/* Invalid input, but defined behaviour */
		PrintError("Data was written to the VDP data port while the VDP was in read mode");

		/* According to GENESIS SOFTWARE DEVELOPMENT MANUAL (COMPLEMENT) section 4.1,
		   data should not be written, but the address should be incremented */
		state->access.index += state->access.increment;
	}
	else if (state->dma.pending)
	{
		/* Perform DMA fill */
		state->dma.pending = cc_false;

		do
		{
			WriteAndIncrement(state, value);
		}
		while (--state->dma.length, state->dma.length &= 0xFFFF, state->dma.length != 0);
	}
	else
	{
		/* Write the value to memory */
		WriteAndIncrement(state, value);
	}
}

void VDP_WriteControl(VDP_State *state, unsigned short value, unsigned short (*read_callback)(void *user_data, unsigned long address), void *user_data)
{
	if (state->access.write_pending)
	{
		/* This command is setting up for a memory access (part 2) */
		const unsigned short destination_address = (state->access.cached_write & 0x3FFF) | ((value & 3) << 14);
		const unsigned char access_mode = ((state->access.cached_write >> 14) & 3) | ((value >> 2) & 0x3C);

		state->access.write_pending = cc_false;

		state->access.index = destination_address;
		state->access.read_mode = !(access_mode & 1);

		if (state->dma.enabled)
			state->dma.pending = access_mode & 0x20;

		switch ((access_mode >> 1) & 7)
		{
			case 0: /* VRAM */
				state->access.selected_buffer = VDP_ACCESS_VRAM;
				break;

			case 4: /* CRAM (read) */
			case 1: /* CRAM (write) */
				state->access.selected_buffer = VDP_ACCESS_CRAM;
				break;

			case 2: /* VSRAM */
				state->access.selected_buffer = VDP_ACCESS_VSRAM;
				break;

			default: /* Invalid */
				PrintError("Invalid VDP access mode specified (0x%X)", access_mode);
				break;
		}

		if (state->dma.pending && state->dma.mode != VDP_DMA_MODE_FILL)
		{
			/* Firing DMA */
			state->dma.pending = cc_false;

			if (state->dma.mode == VDP_DMA_MODE_MEMORY_TO_VRAM)
			{
				do
				{
					WriteAndIncrement(state, read_callback(user_data, (state->dma.source_address_high << 17) | (state->dma.source_address_low << 1)));

					/* Emulate the 128KiB DMA wrap-around bug */
					++state->dma.source_address_low;
					state->dma.source_address_low &= 0xFFFF;
				}
				while (--state->dma.length, state->dma.length &= 0xFFFF, state->dma.length != 0);
			}
			else /*if (state->dma.mode == VDP_DMA_MODE_COPY)*/
			{
				/* TODO */
			}
		}
	}
	else if ((value & 0xC000) != 0x8000)
	{
		/* This command is setting up for a memory access (part 1) */
		state->access.write_pending = cc_true;
		state->access.cached_write = value;
	}
	else
	{
		const unsigned char reg = (value >> 8) & 0x3F;
		const unsigned char data = value & 0xFF;

		/* This command is setting a register */
		switch (reg)
		{
			case 0:
				/* MODE SET REGISTER NO.1 */
				state->h_int_enabled = !!(data & (1 << 4));
				/* TODO */
				break;

			case 1:
				/* MODE SET REGISTER NO.2 */
				state->display_enabled = !!(data & (1 << 6));
				state->v_int_enabled = !!(data & (1 << 5));
				state->dma.enabled = !!(data & (1 << 4));
				state->v30_enabled = !!(data & (1 << 3));
				/* TODO */
				break;

			case 2:
				/* PATTERN NAME TABLE BASE ADDRESS FOR SCROLL A */
				state->plane_a_address = (data & 0x38) << (10 - 1);
				break;

			case 3:
				/* PATTERN NAME TABLE BASE ADDRESS FOR WINDOW */
				state->window_address = (data & 0x3E) << (10 - 1);
				break;

			case 4:
				/* PATTERN NAME TABLE BASE ADDRESS FOR SCROLL B */
				state->plane_b_address = (data & 7) << (13 - 1);
				break;

			case 5:
				/* SPRITE ATTRIBUTE TABLE BASE ADDRESS */
				state->sprite_table_address = (data & 0x7F) << (9 - 1);

				/* Real VDPs partially cache the sprite table, and forget to update it
				   when the sprite table base address is changed. Replicating this
				   behaviour may be needed in order to properly emulate certain effects
				   that involve manipulating the sprite table during rendering. */
				/*state->sprite_cache.needs_updating = cc_true;*/ /* Oops */

				break;

			case 7:
				/* BACKGROUND COLOR */
				state->background_colour = data & 0x3F;
				break;

			case 10:
				/* H INTERRUPT REGISTER */
				state->h_int_interval = data;
				break;

			case 11:
				/* MODE SET REGISTER NO.3 */

				/* TODO - External interrupt */

				state->vscroll_mode = data & 4 ? VDP_VSCROLL_MODE_2CELL : VDP_VSCROLL_MODE_FULL;

				switch (data & 3)
				{
					case 0:
						state->hscroll_mode = VDP_HSCROLL_MODE_FULL;
						break;

					case 1:
						PrintError("Prohibitied H-scroll mode selected");
						break;

					case 2:
						state->hscroll_mode = VDP_HSCROLL_MODE_1CELL;
						break;

					case 3:
						state->hscroll_mode = VDP_HSCROLL_MODE_1LINE;
						break;
				}

				break;

			case 12:
				/* MODE SET REGISTER NO.4 */
				/* TODO */
				state->h40_enabled = !!(data & ((1 << 7) | (1 << 0)));
				state->shadow_highlight_enabled = !!(data & (1 << 3));
				state->interlace_mode_2_enabled = !!(data & (1 << 2));
				break;

			case 13:
				/* H SCROLL DATA TABLE BASE ADDRESS */
				state->hscroll_address = (data & 0x3F) << (10 - 1);
				break;

			case 15:
				/* AUTO INCREMENT DATA */
				state->access.increment = data;
				break;

			case 16:
			{
				/* SCROLL SIZE */
				const unsigned char width_index  = (data >> 0) & 3;
				const unsigned char height_index = (data >> 4) & 3;

				if ((width_index == 3 && height_index != 0) || (height_index == 3 && width_index != 0))
				{
					PrintError("Selected plane size exceeds 64x64/32x128/128x32");
				}
				else
				{
					switch (width_index)
					{
						case 0:
							state->plane_width = 32;
							break;

						case 1:
							state->plane_width = 64;
							break;

						case 2:
							PrintError("Prohibited plane width selected");
							break;

						case 3:
							state->plane_width = 128;
							break;
					}

					switch (height_index)
					{
						case 0:
							state->plane_height = 32;
							break;

						case 1:
							state->plane_height = 64;
							break;

						case 2:
							PrintError("Prohibited plane height selected");
							break;

						case 3:
							state->plane_height = 128;
							break;
					}

					state->plane_width_bitmask = state->plane_width - 1;
					state->plane_height_bitmask = state->plane_height - 1;
				}

				break;
			}

			case 17:
				/* WINDOW H POSITION */
				/* TODO */
				break;

			case 18:
				/* WINDOW V POSITION */
				/* TODO */
				break;

			case 19:
				/* DMA LENGTH COUNTER LOW */
				state->dma.length &= ~((unsigned short)0xFF << 0);
				state->dma.length |= (unsigned short)data << 0;
				break;

			case 20:
				/* DMA LENGTH COUNTER HIGH */
				state->dma.length &= ~((unsigned short)0xFF << 8);
				state->dma.length |= (unsigned short)data << 8;
				break;

			case 21:
				/* DMA SOURCE ADDRESS LOW */
				state->dma.source_address_low &= ~((unsigned short)0xFF << 0);
				state->dma.source_address_low |= (unsigned short)data << 0;
				break;

			case 22:
				/* DMA SOURCE ADDRESS MID. */
				state->dma.source_address_low &= ~((unsigned short)0xFF << 8);
				state->dma.source_address_low |= (unsigned short)data << 8;
				break;

			case 23:
				/* DMA SOURCE ADDRESS HIGH */
				if (data & 0x80)
				{
					state->dma.source_address_high = data & 0x3F;
					state->dma.mode = data & 0x40 ? VDP_DMA_MODE_COPY : VDP_DMA_MODE_FILL;
				}
				else
				{
					state->dma.source_address_high = data & 0x7F;
					state->dma.mode = VDP_DMA_MODE_MEMORY_TO_VRAM;
				}

				break;

			case 6:
			case 8:
			case 9:
			case 14:
				/* Unused legacy register */
				break;

			default:
				/* Invalid */
				PrintError("Attempted to set invalid VDP register (0x%X)", reg);
				break;
		}
	}
}
