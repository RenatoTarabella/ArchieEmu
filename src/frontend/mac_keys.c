/*
 * mac_keys.c - Tastiera del Mac (vedi mac_keys.h)
 */
#include "mac_keys.h"
#include <Carbon/Carbon.h>

/* posizione sulla tastiera del Mac -> codice virtuale di Windows (disposizione UK) */
static const struct { unsigned char mac, vk, ext; } key_table[] = {
    { kVK_ANSI_A, 'A', 0 }, { kVK_ANSI_B, 'B', 0 }, { kVK_ANSI_C, 'C', 0 }, { kVK_ANSI_D, 'D', 0 },
    { kVK_ANSI_E, 'E', 0 }, { kVK_ANSI_F, 'F', 0 }, { kVK_ANSI_G, 'G', 0 }, { kVK_ANSI_H, 'H', 0 },
    { kVK_ANSI_I, 'I', 0 }, { kVK_ANSI_J, 'J', 0 }, { kVK_ANSI_K, 'K', 0 }, { kVK_ANSI_L, 'L', 0 },
    { kVK_ANSI_M, 'M', 0 }, { kVK_ANSI_N, 'N', 0 }, { kVK_ANSI_O, 'O', 0 }, { kVK_ANSI_P, 'P', 0 },
    { kVK_ANSI_Q, 'Q', 0 }, { kVK_ANSI_R, 'R', 0 }, { kVK_ANSI_S, 'S', 0 }, { kVK_ANSI_T, 'T', 0 },
    { kVK_ANSI_U, 'U', 0 }, { kVK_ANSI_V, 'V', 0 }, { kVK_ANSI_W, 'W', 0 }, { kVK_ANSI_X, 'X', 0 },
    { kVK_ANSI_Y, 'Y', 0 }, { kVK_ANSI_Z, 'Z', 0 },
    { kVK_ANSI_0, '0', 0 }, { kVK_ANSI_1, '1', 0 }, { kVK_ANSI_2, '2', 0 }, { kVK_ANSI_3, '3', 0 },
    { kVK_ANSI_4, '4', 0 }, { kVK_ANSI_5, '5', 0 }, { kVK_ANSI_6, '6', 0 }, { kVK_ANSI_7, '7', 0 },
    { kVK_ANSI_8, '8', 0 }, { kVK_ANSI_9, '9', 0 },
    { kVK_ANSI_Minus, 0xBD, 0 }, { kVK_ANSI_Equal, 0xBB, 0 },
    { kVK_ANSI_LeftBracket, 0xDB, 0 }, { kVK_ANSI_RightBracket, 0xDD, 0 },
    { kVK_ANSI_Semicolon, 0xBA, 0 }, { kVK_ANSI_Quote, 0xC0, 0 },         /* ' e' VK_OEM_3 su UK */
    { kVK_ANSI_Comma, 0xBC, 0 }, { kVK_ANSI_Period, 0xBE, 0 }, { kVK_ANSI_Slash, 0xBF, 0 },
    { kVK_Return, 0x0D, 0 }, { kVK_Tab, 0x09, 0 }, { kVK_Space, 0x20, 0 },
    { kVK_Delete, 0x08, 0 }, { kVK_Escape, 0x1B, 0 },
    { kVK_Shift, 0x10, 0 }, { kVK_RightShift, 0x10, 0 },
    { kVK_Control, 0x11, 0 }, { kVK_RightControl, 0x11, 1 },
    { kVK_Option, 0x12, 1 }, { kVK_RightOption, 0x12, 1 },               /* Option = AltGr */
    { kVK_CapsLock, 0x14, 0 },
    { kVK_F1, 0x70, 0 }, { kVK_F2, 0x71, 0 }, { kVK_F3, 0x72, 0 }, { kVK_F4, 0x73, 0 },
    { kVK_F5, 0x74, 0 }, { kVK_F6, 0x75, 0 }, { kVK_F7, 0x76, 0 }, { kVK_F8, 0x77, 0 },
    { kVK_F9, 0x78, 0 }, { kVK_F10, 0x79, 0 }, { kVK_F11, 0x7A, 0 }, { kVK_F12, 0x7B, 0 },
    { kVK_F13, 0x2C, 0 }, { kVK_F14, 0x91, 0 }, { kVK_F15, 0x13, 0 },     /* Print, Scroll Lock, Break */
    { kVK_Help, 0x2D, 1 }, { kVK_Home, 0x24, 1 }, { kVK_PageUp, 0x21, 1 },
    { kVK_ForwardDelete, 0x2E, 1 }, { kVK_End, 0x23, 1 }, { kVK_PageDown, 0x22, 1 },
    { kVK_LeftArrow, 0x25, 1 }, { kVK_RightArrow, 0x27, 1 },
    { kVK_DownArrow, 0x28, 1 }, { kVK_UpArrow, 0x26, 1 },
    { kVK_ANSI_Keypad0, 0x60, 0 }, { kVK_ANSI_Keypad1, 0x61, 0 }, { kVK_ANSI_Keypad2, 0x62, 0 },
    { kVK_ANSI_Keypad3, 0x63, 0 }, { kVK_ANSI_Keypad4, 0x64, 0 }, { kVK_ANSI_Keypad5, 0x65, 0 },
    { kVK_ANSI_Keypad6, 0x66, 0 }, { kVK_ANSI_Keypad7, 0x67, 0 }, { kVK_ANSI_Keypad8, 0x68, 0 },
    { kVK_ANSI_Keypad9, 0x69, 0 },
    { kVK_ANSI_KeypadMultiply, 0x6A, 0 }, { kVK_ANSI_KeypadPlus, 0x6B, 0 },
    { kVK_ANSI_KeypadMinus, 0x6D, 0 }, { kVK_ANSI_KeypadDecimal, 0x6E, 0 },
    { kVK_ANSI_KeypadDivide, 0x6F, 0 }, { kVK_ANSI_KeypadEnter, 0x0D, 1 },
    { kVK_ANSI_KeypadClear, 0x90, 0 },                                    /* Clear = Num Lock */
};

static int iso_keyboard(void)
{
    return KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
}

int mac_key_to_vk(unsigned short keycode, int *extended)
{
    *extended = 0;
    /* i tre tasti che cambiano fra tastiera ANSI e ISO: sulla ISO il tasto
       accanto a Shift sinistro e' il \ dell'Archimedes, quello in alto a
       sinistra e' `, quello accanto a Invio e' # (la sterlina) */
    if (keycode == kVK_ISO_Section) return 0xDF;
    if (keycode == kVK_ANSI_Grave) return iso_keyboard() ? 0xE2 : 0xDF;
    if (keycode == kVK_ANSI_Backslash) return iso_keyboard() ? 0xDE : 0xDC;
    for (size_t i = 0; i < sizeof key_table / sizeof key_table[0]; i++)
        if (key_table[i].mac == keycode) { *extended = key_table[i].ext; return key_table[i].vk; }
    return -1;
}

int mac_key_chars(unsigned short keycode, unsigned long flags, uint32_t *dead, uint16_t *out, int max)
{
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    if (!src) return 0;
    CFDataRef data = TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData);
    UniCharCount n = 0;
    if (data) {
        /* modificatori nel formato di Carbon (EventModifiers >> 8) */
        UInt32 mods = 0;
        if (flags & (1UL << 17)) mods |= shiftKey >> 8;          /* NSEventModifierFlagShift */
        if (flags & (1UL << 16)) mods |= alphaLock >> 8;         /* CapsLock */
        if (flags & (1UL << 19)) mods |= optionKey >> 8;         /* Option */
        if (flags & (1UL << 18)) mods |= controlKey >> 8;        /* Control */
        const UCKeyboardLayout *layout = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
        if (UCKeyTranslate(layout, keycode, kUCKeyActionDown, mods, LMGetKbdType(), 0,
                           dead, (UniCharCount)max, &n, out) != noErr)
            n = 0;
    }
    CFRelease(src);
    return (int)n;
}

int mac_modifier_down(unsigned short keycode, unsigned long flags)
{
    /* bit dei singoli tasti (NX_DEVICE*KEYMASK) */
    switch (keycode) {
    case kVK_Shift:        return (flags & 0x0002) != 0;
    case kVK_RightShift:   return (flags & 0x0004) != 0;
    case kVK_Control:      return (flags & 0x0001) != 0;
    case kVK_RightControl: return (flags & 0x2000) != 0;
    case kVK_Option:       return (flags & 0x0020) != 0;
    case kVK_RightOption:  return (flags & 0x0040) != 0;
    case kVK_Command:      return (flags & 0x0008) != 0;
    case kVK_RightCommand: return (flags & 0x0010) != 0;
    case kVK_CapsLock:     return (flags & (1UL << 16)) != 0;
    default:               return 0;
    }
}
