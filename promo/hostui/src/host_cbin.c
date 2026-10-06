/* 64-bit host loader for xiaozhi-fonts CBIN (a 32-bit little-endian dump of lv_font_t). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "cbin_font.h"
static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t U16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
lv_font_t* cbin_font_create(uint8_t* bin) {
    lv_font_t* font = lv_malloc_zeroed(sizeof(lv_font_t));
    font->get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
    font->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    font->line_height = (int32_t)U32(bin + 12);
    font->base_line = (int32_t)U32(bin + 16);
    font->underline_position = (int8_t)bin[21];
    font->underline_thickness = (int8_t)bin[22];
    uint8_t* d = bin + U32(bin + 24);
    lv_font_fmt_txt_dsc_t* dsc = lv_malloc_zeroed(sizeof(*dsc));
    dsc->glyph_bitmap = d + U32(d + 0);
    dsc->glyph_dsc = (const lv_font_fmt_txt_glyph_dsc_t*)(d + U32(d + 4));
    uint8_t* cmaps = d + U32(d + 8);
    dsc->kern_dsc = NULL;
    dsc->kern_scale = U16(d + 16);
    uint16_t bits = U16(d + 18);
    dsc->cmap_num = bits & 0x1ff;
    dsc->bpp = (bits >> 9) & 0xf;
    dsc->kern_classes = 0;
    dsc->bitmap_format = (bits >> 14) & 3;
    lv_font_fmt_txt_cmap_t* cm = lv_malloc_zeroed(sizeof(*cm) * dsc->cmap_num);
    uint8_t* c = cmaps;
    for (int i = 0; i < dsc->cmap_num; ++i, c += 20) {
        cm[i].range_start = U32(c);
        cm[i].range_length = U16(c + 4);
        cm[i].glyph_id_start = U16(c + 6);
        uint32_t ul = U32(c + 8), go = U32(c + 12);
        cm[i].unicode_list = ul ? (const uint16_t*)(cmaps + ul) : NULL;
        cm[i].glyph_id_ofs_list = go ? (const void*)(cmaps + go) : NULL;
        cm[i].list_length = U16(c + 16);
        cm[i].type = (lv_font_fmt_txt_cmap_type_t)c[18];
    }
    dsc->cmaps = cm;
    font->dsc = dsc;
    fprintf(stderr, "cbin: line_height=%d base=%d cmaps=%d bpp=%d fmt=%d\n", (int)font->line_height,
            (int)font->base_line, dsc->cmap_num, dsc->bpp, dsc->bitmap_format);
    return font;
}
void cbin_font_delete(lv_font_t* f) { (void)f; }
