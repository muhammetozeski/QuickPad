#include "glyphs.h"
#include "quickpad.h"

GlyphAtlas *GlyphAtlasCreate(HFONT font, LayoutMetrics *metrics, int lineHeight, COLORREF text, COLORREF background)
{
    GlyphAtlas *atlas = MemAllocZero(sizeof *atlas);
    if (atlas == NULL) {
        return NULL;
    }
    atlas->font = font;
    atlas->metrics = metrics;
    atlas->lineHeight = lineHeight > 0 ? lineHeight : 1;
    atlas->text = text;
    atlas->background = background;
    return atlas;
}

void GlyphAtlasRelease(GlyphAtlas *atlas)
{
    if (atlas == NULL) {
        return;
    }
    for (size_t i = 0; i < ARRAYSIZE(atlas->pages); ++i) {
        if (atlas->pages[i] != NULL) {
            DeleteObject(atlas->pages[i]->bitmap);
            MemFree(atlas->pages[i]);
        }
    }
    if (atlas->dc != NULL) {
        DeleteDC(atlas->dc);
    }
    MemFree(atlas);
}

/* The memory DC set up for drawing glyphs of this atlas, made on first use. */
static HDC AtlasDC(GlyphAtlas *atlas)
{
    if (atlas->dc == NULL) {
        atlas->dc = CreateCompatibleDC(NULL);
        if (atlas->dc == NULL) {
            return NULL;
        }
        SelectObject(atlas->dc, atlas->font);
        SetTextAlign(atlas->dc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
        SetBkMode(atlas->dc, OPAQUE);
        SetBkColor(atlas->dc, atlas->background);
        SetTextColor(atlas->dc, atlas->text);
    }
    return atlas->dc;
}

/* The page of ch, made as a blank bitmap when it does not exist yet. */
static GlyphPage *EnsurePage(GlyphAtlas *atlas, unsigned index)
{
    if (atlas->pages[index] != NULL) {
        return atlas->pages[index];
    }
    wchar_t first = (wchar_t)(index << 8);
    int slotWidth = 1;
    for (int i = 0; i < 256; ++i) {
        wchar_t ch = (wchar_t)(first + i);
        int width = ch == L'\t' ? 0 : LayoutCharWidth(atlas->metrics, ch, 0);
        slotWidth = width > slotWidth ? width : slotWidth;
    }

    HDC dc = AtlasDC(atlas);
    GlyphPage *page = MemAllocZero(sizeof *page);
    if (dc == NULL || page == NULL) {
        MemFree(page);
        return NULL;
    }
    page->slotWidth = slotWidth;
    BITMAPINFO info = { 0 };
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 256 * slotWidth + GLYPH_PAGE_PADDING;
    info.bmiHeader.biHeight = -atlas->lineHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    page->bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    if (page->bitmap == NULL) {
        MemFree(page);
        return NULL;
    }
    page->pixels = bits;
    DWORD background = ((DWORD)GetRValue(atlas->background) << 16) | ((DWORD)GetGValue(atlas->background) << 8) | GetBValue(atlas->background);
    size_t count = (size_t)info.bmiHeader.biWidth * atlas->lineHeight;
    for (size_t i = 0; i < count; ++i) {
        page->pixels[i] = background;
    }
    atlas->pages[index] = page;
    return page;
}

static void DrawSlot(GlyphAtlas *atlas, GlyphPage *page, wchar_t ch)
{
    int slot = ch & 0xFF;
    page->drawn[slot >> 3] |= (unsigned char)(1 << (slot & 7));
    BOOL blank = ch == L'\t' || ch == L'\n' || ch == L'\r' || IS_HIGH_SURROGATE(ch) || IS_LOW_SURROGATE(ch)
        || LayoutCharWidth(atlas->metrics, ch, 0) <= 0;
    if (blank) {
        return;
    }
    HDC dc = AtlasDC(atlas);
    HGDIOBJ previous = SelectObject(dc, page->bitmap);
    RECT box = { slot * page->slotWidth, 0, (slot + 1) * page->slotWidth, atlas->lineHeight };
    ExtTextOutW(dc, box.left, 0, ETO_OPAQUE | ETO_CLIPPED, &box, &ch, 1, NULL);
    GdiFlush();
    SelectObject(dc, previous);
}

BOOL GlyphAtlasPrepare(GlyphAtlas *atlas, wchar_t ch)
{
    GlyphPage *page = EnsurePage(atlas, (unsigned)ch >> 8);
    if (page == NULL) {
        return FALSE;
    }
    int slot = ch & 0xFF;
    if ((page->drawn[slot >> 3] & (1 << (slot & 7))) == 0) {
        DrawSlot(atlas, page, ch);
    }
    return TRUE;
}

void GlyphAtlasPrepareRange(GlyphAtlas *atlas, wchar_t first, wchar_t last)
{
    for (unsigned ch = first; ch <= last; ++ch) {
        if (!GlyphAtlasPrepare(atlas, (wchar_t)ch)) {
            return;
        }
    }
}

const DWORD *GlyphAtlasPixels(const GlyphAtlas *atlas, wchar_t ch, int *stride)
{
    const GlyphPage *page = atlas->pages[(unsigned)ch >> 8];
    if (page == NULL) {
        return NULL;
    }
    *stride = 256 * page->slotWidth + GLYPH_PAGE_PADDING;
    return page->pixels + (ch & 0xFF) * page->slotWidth;
}
