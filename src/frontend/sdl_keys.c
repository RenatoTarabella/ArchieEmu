/*
 * sdl_keys.c - Tastiera per i front end SDL2 (vedi sdl_keys.h)
 */
#include "sdl_keys.h"
#include <SDL.h>

/* posizione sulla tastiera (scancode USB) -> codice virtuale di Windows (UK) */
static const struct { uint16_t sc; uint8_t vk, ext; } key_table[] = {
    { SDL_SCANCODE_A, 'A', 0 }, { SDL_SCANCODE_B, 'B', 0 }, { SDL_SCANCODE_C, 'C', 0 },
    { SDL_SCANCODE_D, 'D', 0 }, { SDL_SCANCODE_E, 'E', 0 }, { SDL_SCANCODE_F, 'F', 0 },
    { SDL_SCANCODE_G, 'G', 0 }, { SDL_SCANCODE_H, 'H', 0 }, { SDL_SCANCODE_I, 'I', 0 },
    { SDL_SCANCODE_J, 'J', 0 }, { SDL_SCANCODE_K, 'K', 0 }, { SDL_SCANCODE_L, 'L', 0 },
    { SDL_SCANCODE_M, 'M', 0 }, { SDL_SCANCODE_N, 'N', 0 }, { SDL_SCANCODE_O, 'O', 0 },
    { SDL_SCANCODE_P, 'P', 0 }, { SDL_SCANCODE_Q, 'Q', 0 }, { SDL_SCANCODE_R, 'R', 0 },
    { SDL_SCANCODE_S, 'S', 0 }, { SDL_SCANCODE_T, 'T', 0 }, { SDL_SCANCODE_U, 'U', 0 },
    { SDL_SCANCODE_V, 'V', 0 }, { SDL_SCANCODE_W, 'W', 0 }, { SDL_SCANCODE_X, 'X', 0 },
    { SDL_SCANCODE_Y, 'Y', 0 }, { SDL_SCANCODE_Z, 'Z', 0 },
    { SDL_SCANCODE_1, '1', 0 }, { SDL_SCANCODE_2, '2', 0 }, { SDL_SCANCODE_3, '3', 0 },
    { SDL_SCANCODE_4, '4', 0 }, { SDL_SCANCODE_5, '5', 0 }, { SDL_SCANCODE_6, '6', 0 },
    { SDL_SCANCODE_7, '7', 0 }, { SDL_SCANCODE_8, '8', 0 }, { SDL_SCANCODE_9, '9', 0 },
    { SDL_SCANCODE_0, '0', 0 },
    { SDL_SCANCODE_RETURN, 0x0D, 0 }, { SDL_SCANCODE_ESCAPE, 0x1B, 0 },
    { SDL_SCANCODE_BACKSPACE, 0x08, 0 }, { SDL_SCANCODE_TAB, 0x09, 0 }, { SDL_SCANCODE_SPACE, 0x20, 0 },
    { SDL_SCANCODE_MINUS, 0xBD, 0 }, { SDL_SCANCODE_EQUALS, 0xBB, 0 },
    { SDL_SCANCODE_LEFTBRACKET, 0xDB, 0 }, { SDL_SCANCODE_RIGHTBRACKET, 0xDD, 0 },
    { SDL_SCANCODE_BACKSLASH, 0xDC, 0 },               /* ANSI; sulle ISO spesso e' il # */
    { SDL_SCANCODE_NONUSHASH, 0xDE, 0 },               /* # ~ accanto a Invio (UK) */
    { SDL_SCANCODE_SEMICOLON, 0xBA, 0 }, { SDL_SCANCODE_APOSTROPHE, 0xC0, 0 },   /* ' e' VK_OEM_3 su UK */
    { SDL_SCANCODE_GRAVE, 0xDF, 0 },                   /* ` (VK_OEM_8 su UK) */
    { SDL_SCANCODE_COMMA, 0xBC, 0 }, { SDL_SCANCODE_PERIOD, 0xBE, 0 }, { SDL_SCANCODE_SLASH, 0xBF, 0 },
    { SDL_SCANCODE_NONUSBACKSLASH, 0xE2, 0 },          /* \ accanto a Shift sinistro (ISO) */
    { SDL_SCANCODE_CAPSLOCK, 0x14, 0 },
    { SDL_SCANCODE_F1, 0x70, 0 }, { SDL_SCANCODE_F2, 0x71, 0 }, { SDL_SCANCODE_F3, 0x72, 0 },
    { SDL_SCANCODE_F4, 0x73, 0 }, { SDL_SCANCODE_F5, 0x74, 0 }, { SDL_SCANCODE_F6, 0x75, 0 },
    { SDL_SCANCODE_F7, 0x76, 0 }, { SDL_SCANCODE_F8, 0x77, 0 }, { SDL_SCANCODE_F9, 0x78, 0 },
    { SDL_SCANCODE_F10, 0x79, 0 }, { SDL_SCANCODE_F11, 0x7A, 0 }, { SDL_SCANCODE_F12, 0x7B, 0 },
    { SDL_SCANCODE_PRINTSCREEN, 0x2C, 1 }, { SDL_SCANCODE_SCROLLLOCK, 0x91, 0 },
    { SDL_SCANCODE_PAUSE, 0x13, 0 },                   /* Break */
    { SDL_SCANCODE_INSERT, 0x2D, 1 }, { SDL_SCANCODE_HOME, 0x24, 1 }, { SDL_SCANCODE_PAGEUP, 0x21, 1 },
    { SDL_SCANCODE_DELETE, 0x2E, 1 }, { SDL_SCANCODE_END, 0x23, 1 }, { SDL_SCANCODE_PAGEDOWN, 0x22, 1 },
    { SDL_SCANCODE_RIGHT, 0x27, 1 }, { SDL_SCANCODE_LEFT, 0x25, 1 },
    { SDL_SCANCODE_DOWN, 0x28, 1 }, { SDL_SCANCODE_UP, 0x26, 1 },
    { SDL_SCANCODE_NUMLOCKCLEAR, 0x90, 1 },
    { SDL_SCANCODE_KP_DIVIDE, 0x6F, 1 }, { SDL_SCANCODE_KP_MULTIPLY, 0x6A, 0 },
    { SDL_SCANCODE_KP_MINUS, 0x6D, 0 }, { SDL_SCANCODE_KP_PLUS, 0x6B, 0 },
    { SDL_SCANCODE_KP_ENTER, 0x0D, 1 },
    { SDL_SCANCODE_KP_1, 0x61, 0 }, { SDL_SCANCODE_KP_2, 0x62, 0 }, { SDL_SCANCODE_KP_3, 0x63, 0 },
    { SDL_SCANCODE_KP_4, 0x64, 0 }, { SDL_SCANCODE_KP_5, 0x65, 0 }, { SDL_SCANCODE_KP_6, 0x66, 0 },
    { SDL_SCANCODE_KP_7, 0x67, 0 }, { SDL_SCANCODE_KP_8, 0x68, 0 }, { SDL_SCANCODE_KP_9, 0x69, 0 },
    { SDL_SCANCODE_KP_0, 0x60, 0 }, { SDL_SCANCODE_KP_PERIOD, 0x6E, 0 },
    { SDL_SCANCODE_LSHIFT, 0x10, 0 }, { SDL_SCANCODE_RSHIFT, 0x10, 0 },
    { SDL_SCANCODE_LCTRL, 0x11, 0 }, { SDL_SCANCODE_RCTRL, 0x11, 1 },
    { SDL_SCANCODE_LALT, 0x12, 0 }, { SDL_SCANCODE_RALT, 0x12, 1 },   /* Alt destro = AltGr */
};

int sdl_scancode_to_vk(int scancode, int *extended)
{
    *extended = 0;
    for (size_t i = 0; i < sizeof key_table / sizeof key_table[0]; i++)
        if (key_table[i].sc == scancode) {
            *extended = key_table[i].ext;
            return key_table[i].vk;
        }
    return -1;
}

uint32_t sdl_utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p;
    if (!c) return 0;
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (n) c &= 0x3F >> n;
    p++;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}
