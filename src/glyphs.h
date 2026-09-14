#pragma once

#include <windows.h>

#include "layout.h"

/*
 * Glyph bitmaps of one font in one pair of colors: a character is drawn by GDI the first time it
 * is needed, into its slot in a page of 256, and copied from there whenever a row is painted. A
 * slot is as wide as the widest glyph of its page and one line tall; a character's own advance
 * from the layout metrics is what gets copied, so a glyph is clipped to its advance the way
 * ETO_CLIPPED clips a run. Drawing happens on the thread that calls GlyphAtlasPrepare; reading
 * prepared slots with GlyphAtlasPixels is safe from any thread.
 */

/* Pixels after the last slot of a page row, so copies of 16 bytes at a time can overshoot a slot. */
#define GLYPH_PAGE_PADDING 4

typedef struct GlyphPage {
    HBITMAP bitmap;
    DWORD *pixels;               /* 256 slots side by side, lineHeight rows of 256 * slotWidth + GLYPH_PAGE_PADDING pixels */
    int slotWidth;
    unsigned char drawn[32];     /* one bit per slot */
} GlyphPage;

typedef struct GlyphAtlas {
    HFONT font;
    LayoutMetrics *metrics;
    int lineHeight;
    COLORREF text;
    COLORREF background;
    HDC dc;                      /* the memory DC glyphs are drawn with, kept between calls */
    GlyphPage *pages[256];
} GlyphAtlas;

GlyphAtlas *GlyphAtlasCreate(HFONT font, LayoutMetrics *metrics, int lineHeight, COLORREF text, COLORREF background);
void GlyphAtlasRelease(GlyphAtlas *atlas);

/* Draws ch into its slot if it is not drawn yet; FALSE when memory runs out. */
BOOL GlyphAtlasPrepare(GlyphAtlas *atlas, wchar_t ch);

/* Draws every character from first to last that is not drawn yet. */
void GlyphAtlasPrepareRange(GlyphAtlas *atlas, wchar_t first, wchar_t last);

/* The top-left pixel of ch's slot and the stride of its page in pixels; the slot must have been prepared. */
const DWORD *GlyphAtlasPixels(const GlyphAtlas *atlas, wchar_t ch, int *stride);
