/*
 * HelloWorld.ino - S1 + TR4 + immediate POF test
 *
 * Displays five text pages from the public-domain novel
 * "Alice's Adventures in Wonderland". Each page uses:
 *   S1 single-plane + TR4 fast waveform + immediate POF
 *
 * A new page is displayed every 2 seconds.
 */

#include <UC8179.h>

UC8179 epd;

// PON/POF CONFIGURATION: default production test is S1 + TR4 + immediate POF.
// Change to FAST_POF_AFTER_REFRESH only when testing stop-keeping-powered behavior.
static const UC8179::FastPowerPolicy FAST_POWER_POLICY =
    UC8179::FAST_KEEP_POWERED;

static const char* const pages[5] = {
    "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do: once or twice she had peeped into the book her sister was reading, but it had no pictures or conversations in it, and what is the use of a book, thought Alice, without pictures or conversations?",

    "So she was considering in her own mind, as well as she could, for the hot day made her feel very sleepy and stupid, whether the pleasure of making a daisy-chain would be worth the trouble of getting up and picking the daisies, when suddenly a White Rabbit with pink eyes ran close by her.",

    "There was nothing so very remarkable in that; nor did Alice think it so very much out of the way to hear the Rabbit say to itself, Oh dear! Oh dear! I shall be late! When she thought it over afterwards, it occurred to her that she ought to have wondered at this, but at the time it all seemed quite natural.",

    "But when the Rabbit actually took a watch out of its waistcoat-pocket, and looked at it, and then hurried on, Alice started to her feet, for it flashed across her mind that she had never before seen a rabbit with either a waistcoat-pocket, or a watch to take out of it.",

    "Burning with curiosity, she ran across the field after it, and fortunately was just in time to see it pop down a large rabbit-hole under the hedge. In another moment down went Alice after it, never once considering how in the world she was to get out again."
};

static void drawWrappedText(const char* text)
{
    const int16_t left = 28;
    const int16_t top = 42;
    const int16_t right = UC8179::WIDTH - 28;
    const int16_t lineHeight = 18;
    const int16_t charWidth = 12; // text size 2: 6 pixels * 2
    const int16_t maxChars = (right - left) / charWidth;

    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(2);
    epd.setCursor(left, top);

    int16_t column = 0;
    int16_t line = 0;
    for (const char* p = text; *p && line < 22; p++) {
        char c = *p;
        if (c == ' ' && column == 0) continue;
        if (c == ' ' && column >= maxChars) {
            epd.print('\n');
            column = 0;
            line++;
            continue;
        }
        epd.print(c);
        column++;
        if (column >= maxChars) {
            epd.print('\n');
            column = 0;
            line++;
        }
    }
}

static bool showPage(uint8_t page)
{
    epd.fillScreen(UC8179::INK_WHITE);
    epd.setTextColor(UC8179::INK_BLACK);
    epd.setTextSize(2);
    epd.setCursor(28, 12);
    epd.print("ALICE - PAGE ");
    epd.print(page + 1);
    epd.drawLine(28, 32, UC8179::WIDTH - 28, 32, UC8179::INK_BLACK);
    drawWrappedText(pages[page]);

    return epd.displayFast();
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    epd.setDebug(&Serial);
    epd.setFastPowerPolicy(FAST_POWER_POLICY);

    if (!epd.begin()) {
        Serial.print("epd.begin() failed: ");
        Serial.println(epd.errorText());
        while (true) delay(1000);
    }

    for (uint8_t page = 0; page < 5; page++) {
        Serial.print("S1 + TR4 + POF, page ");
        Serial.println(page + 1);
        if (!showPage(page)) {
            Serial.print("page failed: ");
            Serial.println(epd.errorText());
            break;
        }
        delay(2000);
        if (page != 4 && !epd.begin()) {
            Serial.print("re-init failed: ");
            Serial.println(epd.errorText());
            break;
        }
    }
}

void loop() {}
