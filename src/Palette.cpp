#include "TextEdit.h"

namespace ipte {
const Palette &DarkPalette() {
    static const Palette palette{
        IM_COL32(224, 224, 224, 255), // text
        IM_COL32(197, 134, 192, 255), // keyword
        IM_COL32(90, 179, 155, 255), // declaration
        IM_COL32(181, 206, 168, 255), // number
        IM_COL32(206, 145, 120, 255), // string
        IM_COL32(255, 255, 153, 255), // punctuation
        IM_COL32(64, 192, 128, 255), // preprocessor
        IM_COL32(156, 220, 254, 255), // identifier
        IM_COL32(79, 193, 255, 255), // known identifier
        IM_COL32(106, 153, 85, 255), // comment
        IM_COL32(30, 30, 30, 255), // background
        IM_COL32(224, 224, 224, 255), // cursor
        IM_COL32(32, 96, 160, 255), // selection
        IM_COL32(80, 80, 80, 255), // whitespace
        IM_COL32(70, 70, 70, 255), // matching bracket background
        IM_COL32(140, 140, 140, 255), // matching bracket active
        IM_COL32(246, 222, 36, 255), // matching bracket level 1
        IM_COL32(66, 120, 198, 255), // matching bracket level 2
        IM_COL32(213, 96, 213, 255), // matching bracket level 3
        IM_COL32(198, 8, 32, 255), // matching bracket error
        IM_COL32(128, 128, 144, 255), // line number
        IM_COL32(224, 224, 240, 255), // current line number
        IM_COL32(255, 255, 255, 8), // current line highlight
        IM_COL32(255, 255, 255, 16), // current line highlight border
    };
    return palette;
}

const Palette &LightPalette() {
    static const Palette palette{
        IM_COL32(64, 64, 64, 255), // text
        IM_COL32(170, 0, 220, 255), // keyword
        IM_COL32(65, 0, 255, 255), // declaration
        IM_COL32(40, 140, 90, 255), // number
        IM_COL32(160, 32, 32, 255), // string
        IM_COL32(0, 0, 0, 255), // punctuation
        IM_COL32(96, 96, 64, 255), // preprocessor
        IM_COL32(64, 64, 64, 255), // identifier
        IM_COL32(16, 96, 96, 255), // known identifier
        IM_COL32(35, 135, 5, 255), // comment
        IM_COL32(255, 255, 255, 255), // background
        IM_COL32(0, 0, 0, 255), // cursor
        IM_COL32(0, 0, 96, 64), // selection
        IM_COL32(144, 144, 144, 144), // whitespace
        IM_COL32(180, 180, 180, 144), // matching bracket background
        IM_COL32(72, 72, 72, 255), // matching bracket active
        IM_COL32(70, 0, 250, 255), // matching bracket level 1
        IM_COL32(80, 160, 70, 255), // matching bracket level 2
        IM_COL32(120, 60, 25, 255), // matching bracket level 3
        IM_COL32(198, 8, 32, 255), // matching bracket error
        IM_COL32(0, 80, 80, 255), // line number
        IM_COL32(0, 0, 0, 255), // current line number
        IM_COL32(0, 0, 0, 8), // current line highlight
        IM_COL32(0, 0, 0, 16), // current line highlight border
    };
    return palette;
}
} // namespace ipte
