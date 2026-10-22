#include "screen_draw.h"
#include <string.h>
#include <stdio.h>
#include "gray_motion.h"

static void black_pixel(uint8_t *frame, unsigned x, unsigned y)
{
    frame[y * (SSD1681_WIDTH / 8) + x / 8] &= ~(0x80u >> (x % 8));
}

/* Compact uppercase 5x7 glyphs, encoded as five columns, low bit at the top. */
static const uint8_t letters[26][5] = {
    {126,9,9,9,126}, {127,73,73,73,54}, {62,65,65,65,34},
    {127,65,65,34,28}, {127,73,73,73,65}, {127,9,9,9,1},
    {62,65,73,73,122}, {127,8,8,8,127}, {65,65,127,65,65},
    {32,64,65,63,1}, {127,8,20,34,65}, {127,64,64,64,64},
    {127,2,12,2,127}, {127,4,8,16,127}, {62,65,65,65,62},
    {127,9,9,9,6}, {62,65,81,33,94}, {127,9,25,41,70},
    {38,73,73,73,50}, {1,1,127,1,1}, {63,64,64,64,63},
    {31,32,64,32,31}, {63,64,56,64,63}, {99,20,8,20,99},
    {3,4,120,4,3}, {97,81,73,69,67},
};

static void text_at(uint8_t *frame, unsigned x, unsigned y, const char *text, unsigned scale)
{
    size_t length = strlen(text);
    if (!length || !scale || length > SSD1681_WIDTH / (6 * scale) ||
        y + 7 * scale > SSD1681_HEIGHT ||
        x + (length * 6 - 1) * scale > SSD1681_WIDTH) return;
    for (; *text; ++text, x += 6 * scale) {
        uint8_t special[5] = {0};
        const uint8_t *glyph = special;
        if (*text >= 'A' && *text <= 'Z') glyph = letters[*text - 'A'];
        else switch (*text) {
            case '0': special[0]=62; special[1]=81; special[2]=73; special[3]=69; special[4]=62; break;
            case '1': special[1]=66; special[2]=127; special[3]=64; break;
            case '2': special[0]=98; special[1]=81; special[2]=73; special[3]=73; special[4]=70; break;
            case '3': special[0]=34; special[1]=65; special[2]=73; special[3]=73; special[4]=54; break;
            case '4': special[0]=24; special[1]=20; special[2]=18; special[3]=127; special[4]=16; break;
            case '5': special[0]=39; special[1]=69; special[2]=69; special[3]=69; special[4]=57; break;
            case '7': special[0]=1; special[1]=113; special[2]=9; special[3]=5; special[4]=3; break;
            case '9': special[0]=6; special[1]=73; special[2]=73; special[3]=73; special[4]=62; break;
            case '-': memset(special, 8, sizeof(special)); break;
            case ':': special[2]=20; break;
            case '6': special[0]=62; special[1]=73; special[2]=73; special[3]=73; special[4]=50; break;
            case '8': special[0]=54; special[1]=73; special[2]=73; special[3]=73; special[4]=54; break;
            case '/': special[0]=64; special[1]=32; special[2]=8; special[3]=2; special[4]=1; break;
            case '.': special[2]=64; break;
            case '>': special[0]=65; special[1]=34; special[2]=20; special[3]=8; break;
            case '_': memset(special, 64, sizeof(special)); break;
            case '*': special[0]=20; special[1]=8; special[2]=62; special[3]=8; special[4]=20; break;
            case '=':
                for (unsigned i=0;i<5;++i) { special[i]=20; }
                break;
        }
        for (unsigned col=0; col<5; ++col) for (unsigned row=0; row<7; ++row)
            if (glyph[col] & (1u << row))
                for (unsigned dy=0; dy<scale; ++dy) for (unsigned dx=0; dx<scale; ++dx)
                    black_pixel(frame, x + col*scale + dx, y + row*scale + dy);
    }
}

static void centered_text(uint8_t *frame, unsigned y, const char *text, unsigned scale)
{
    size_t length=strlen(text);
    if(!length || !scale || length>SSD1681_WIDTH/(6*scale))return;
    unsigned width=(length*6-1)*scale;
    text_at(frame,(SSD1681_WIDTH-width)/2,y,text,scale);
}

void screen_draw_message(uint8_t *frame, const char *title,
                                  const char *line1, const char *line2, const char *footer)
{
    memset(frame, 0xff, SSD1681_FRAME_BYTES);
    centered_text(frame, 55, title, 2);
    centered_text(frame, 94, line1, 1);
    centered_text(frame, 115, line2, 1);
    centered_text(frame, 180, footer, 1);
}

/* User filenames can contain lowercase or characters outside this tiny font. */
static void player_text_scaled(uint8_t *frame, unsigned y, const char *text, unsigned limit, unsigned scale)
{
    char clipped[33];
    unsigned i = 0;
    for (; i < limit && text[i]; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        clipped[i] = ch >= 32 && ch < 127 ? (char)ch : '-';
    }
    clipped[i] = 0;
    centered_text(frame, y, clipped, scale);
}

static void player_text(uint8_t *frame, unsigned y, const char *text, unsigned limit)
{
    player_text_scaled(frame, y, text, limit, 1);
}

void screen_draw_menu(uint8_t *frame, const char *title, const char *subtitle,
                              const char *const *items, unsigned count, unsigned selected)
{
    memset(frame, 255, SSD1681_FRAME_BYTES);
    (void)title; (void)subtitle;
    unsigned scale = 2;
    for (unsigned i=0;i<count;++i) if (strlen(items[i])>16) scale=1;
    unsigned first = (selected / 6) * 6;
    unsigned visible = count - first < 6 ? count - first : 6;
    unsigned top = (SSD1681_HEIGHT - visible * 28) / 2;
    for (unsigned row = 0; row < visible; ++row) {
        unsigned y = top + row * 28 + (scale==2 ? 7 : 10);
        player_text_scaled(frame, y, items[first + row], scale==2 ? 16 : 30, scale);
        if (first + row == selected)
            for (unsigned yy = top + row * 28 + 4; yy < top + row * 28 + 24; ++yy)
                for (unsigned x = 5; x < 195; ++x) frame[yy * 25 + x / 8] ^= 0x80u >> (x % 8);
    }

}

/* Classic gameplay conversion fills x=20..179, y=28..171. The remaining pixels
 * form a 20px side bezel and a 28px top/bottom bezel. FILL leaves 10px strips. */
void screen_draw_hud(uint8_t *frame, game_border_t border)
{
    const bool black = border == BORDER_BLACK_LOGO;
    memset(frame, black ? 0 : 0xff, SSD1681_FRAME_BYTES);
    if (border != BORDER_BLACK_LOGO && border != BORDER_WHITE_LOGO) return;
    /* Bitmap glyphs for I N K B O Y, 7 bits wide by 9 rows. */
    static const uint8_t glyphs[6][9] = {
        {127,127,28,28,28,28,28,127,127},
        {99,115,123,123,111,111,103,99,99},
        {99,103,110,124,120,124,110,103,99},
        {126,127,99,99,126,127,99,127,126},
        {62,127,99,99,99,99,99,127,62},
        {99,99,99,54,28,28,28,28,28},
    };
    unsigned left = 47;
    for (unsigned letter = 0; letter < 6; ++letter) {
        if (letter == 3) left += 8;
        for (unsigned row = 0; row < 9; ++row)
            for (unsigned col = 0; col < 7; ++col) {
                if (!(glyphs[letter][row] & (0x40u >> col))) continue;
                unsigned x = left + 2 * (col + (8 - row) / 3);
                unsigned y = 5 + 2 * row;
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        uint8_t *byte = &frame[(y + dy) * 25 + (x + dx) / 8];
                        const uint8_t bit = 0x80u >> ((x + dx) % 8);
                        if (black) *byte |= bit;
                        else *byte &= (uint8_t)~bit;
                    }
            }
        left += 16;
    }
}

void screen_draw_fps(uint8_t *frame, unsigned tenths, game_border_t border)
{
    char label[16];
    if (tenths>999) tenths=999;
    snprintf(label,sizeof(label),"FPS %2u.%u",tenths/10,tenths%10);
    /* Render black text on white in the bottom bezel only; the viewport and top logo are
     * untouched. FILL has a 10px strip, so it uses the small font. */
    const unsigned top = border == BORDER_FILL ? 190 : 172;
    memset(frame+top*25,0xff,(200-top)*25);
    if (border == BORDER_FILL) centered_text(frame,192,label,1);
    else centered_text(frame,180,label,2);
    if (border == BORDER_BLACK_LOGO)
        for (unsigned i=top*25;i<SSD1681_FRAME_BYTES;++i) frame[i]^=0xff;
}

void screen_draw_gray_motion_labels(uint8_t *frame, unsigned state, bool paused)
{
    static const char *names[] = {"K", "D", "L", "W"};
    char status[40];
    memset(frame, 255, SSD1681_FRAME_BYTES);
    snprintf(status,sizeof(status),"GRAYSCALE TEST / %s",
             paused ? "PAUSED" : state==0 ? "PREPARE" : state==1 ? "5S SWEEP" : "1S HOLD");
    player_text(frame, 3, status, 32);
    player_text(frame, 16, "LEFT BG / RIGHT PATCH", 32);
    for (unsigned bg=0; bg<4; ++bg) {
        text_at(frame, 5, 28+bg*36+14, names[bg], 1);
        for (unsigned lane=0; lane<3; ++lane)
            text_at(frame, 188, 28+bg*36+lane*12+2, names[gray_motion_patch(bg,lane)], 1);
    }
    player_text(frame, 178, "BOOT/A PAUSE PWR/START REPLAY", 32);
    player_text(frame, 190, "SELECT CLEAN HOLD PWR BACK", 32);
}

void screen_draw_value_menu(uint8_t *frame, const char *value, const char *const *items, unsigned selected)
{
    memset(frame, 255, SSD1681_FRAME_BYTES);
    player_text(frame, 65, value, 32);
    for (unsigned row = 0; row < 2; ++row) {
        unsigned y = 105 + row * 30;
        player_text(frame, y, items[row], 30);
        if (row == selected)
            for (unsigned yy = y - 6; yy < y + 14; ++yy)
                for (unsigned x = 5; x < 195; ++x) frame[yy * 25 + x / 8] ^= 0x80u >> (x % 8);
    }
}

static bool controller_shape(int x, int y)
{
    if (y<1 || y>11 || x<0 || x>17) return false;
    if (y==1) return x>=4 && x<=13;
    if (y==2) return x>=2 && x<=15;
    if (y<=8) return x>=1 && x<=16;
    if (y==9) return true;
    if (y==10) return x<=4 || x>=13;
    return (x>=1 && x<=3) || (x>=14 && x<=16);
}

/* A compact status badge above the centered Home/Controller list. */
void screen_draw_controller_status(uint8_t *frame, controller_status_t status)
{
    static const char *labels[] = {"BLE OFF", "BLE WAIT", "BLE", "USB OFF", "USB WAIT", "USB", "USB: PC"};
    if ((unsigned)status > CONTROLLER_USB_PC) return;
    bool connected = status == CONTROLLER_BLE_CONNECTED || status == CONTROLLER_USB_CONNECTED;
    unsigned width = 20 + (unsigned)strlen(labels[status]) * 6;
    unsigned left = (SSD1681_WIDTH - width) / 2;
    if (status == CONTROLLER_USB_PC) {
        for (unsigned y=0;y<10;++y) for(unsigned x=0;x<16;++x)
            if (!x || x==15 || !y || y==9) black_pixel(frame,left+x,10+y);
        for(unsigned x=0;x<18;++x) black_pixel(frame,left+x,21);
    } else {
        /* Gamepad silhouette, solid when connected and crossed when waiting/off. */
        for(unsigned y=0;y<12;++y) for(unsigned x=0;x<18;++x) {
            bool body = controller_shape((int)x,(int)y);
            bool outline = body && (!controller_shape((int)x-1,(int)y) || !controller_shape((int)x+1,(int)y) ||
                                    !controller_shape((int)x,(int)y-1) || !controller_shape((int)x,(int)y+1));
            bool controls = (x==5 && y>=4 && y<=7) || (y==5 && x>=3 && x<=7) ||
                            (x==12 && y==5) || (x==14 && y==7);
            if (body && (connected ? !controls : outline || controls)) black_pixel(frame,left+x,10+y);
        }
        if (!connected) for(unsigned i=0;i<16;++i) black_pixel(frame,left+1+i,9+i*12/16);
    }
    text_at(frame,left+23,13,labels[status],1);
}
