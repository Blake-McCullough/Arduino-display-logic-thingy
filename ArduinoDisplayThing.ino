#include <TFT_eSPI.h>
#include <SPI.h>

TFT_eSPI tft = TFT_eSPI();

// Array of rainbow colors (16-bit RGB565 format)
const uint16_t rainbowColors[] = {
  TFT_RED,      TFT_ORANGE,   TFT_YELLOW,  TFT_GREEN,
  TFT_CYAN,     TFT_BLUE,     TFT_PURPLE,  TFT_PINK
};

const int numColors = 8;

void setup() {
  Serial.begin(115200);
  
  // Turn on backlight (CRITICAL for CYD)
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  delay(100);
  tft.init();
  tft.setRotation(1);  // Landscape mode (320x240)
  
  Serial.println("Rainbow display starting...");
}

void drawVerticalRainbow() {
  // Draw vertical stripes (each stripe 40 pixels wide for 320px width)
  int stripeWidth = 320 / numColors;
  
  for (int i = 0; i < numColors; i++) {
    tft.fillRect(i * stripeWidth, 0, stripeWidth, 240, rainbowColors[i]);
  }
}

void drawHorizontalRainbow() {
  // Draw horizontal stripes (each stripe 30 pixels high for 240px height)
  int stripeHeight = 240 / numColors;
  
  for (int i = 0; i < numColors; i++) {
    tft.fillRect(0, i * stripeHeight, 320, stripeHeight, rainbowColors[i]);
  }
}

void drawSmoothRainbow() {
  // Create a smooth gradient rainbow across the screen
  // Each column gets a slightly different hue
  
  for (int x = 0; x < 320; x++) {
    // Map x position to angle (0-360 degrees)
    float angle = (float)x / 320.0 * 360.0;
    
    // Convert HSV to RGB (simplified rainbow)
    // We'll use sin/cos to create smooth transitions between colors
    uint8_t r = (sin(angle * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    uint8_t g = (sin((angle + 120) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    uint8_t b = (sin((angle + 240) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    
    // Convert RGB to 16-bit RGB565 format
    uint16_t color = tft.color565(r, g, b);
    
    // Draw vertical line for this column
    tft.drawFastVLine(x, 0, 240, color);
  }
}

void drawDiagonalRainbow() {
  // Diagonal rainbow pattern that fills the screen
  
  for (int x = 0; x < 320; x++) {
    for (int y = 0; y < 240; y++) {
      // Use both x and y to create diagonal bands
      float angle = ((x + y) % 360) / 360.0 * 360.0;
      
      uint8_t r = (sin(angle * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
      uint8_t g = (sin((angle + 120) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
      uint8_t b = (sin((angle + 240) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
      
      tft.drawPixel(x, y, tft.color565(r, g, b));
    }
    delay(1);  // Small delay to show drawing progress
  }
}

void drawRainbowText() {
  // Draw rainbow text on a black background
  tft.fillScreen(TFT_BLACK);
  
  tft.setTextSize(3);
  
  String messages[] = {
    "RAINBOW!",
    "CYD TEST",
    "ESP32",
    "NOW PLAYING"
  };
  
  int yPos = 30;
  for (int i = 0; i < 4; i++) {
    // Each line gets a different rainbow color
    tft.setTextColor(rainbowColors[i % numColors], TFT_BLACK);
    tft.setCursor(20, yPos);
    tft.println(messages[i]);
    yPos += 40;
  }
}

void drawAnimatedRainbowWheel() {
  // Create a spinning rainbow effect
  static float hueOffset = 0;
  
  for (int x = 0; x < 320; x++) {
    float angle = ((float)x / 320.0 * 360.0 + hueOffset);
    
    uint8_t r = (sin(angle * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    uint8_t g = (sin((angle + 120) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    uint8_t b = (sin((angle + 240) * 3.14159 / 180.0) + 1.0) / 2.0 * 255;
    
    tft.drawFastVLine(x, 0, 240, tft.color565(r, g, b));
  }
  
  hueOffset += 5;
  if (hueOffset >= 360) hueOffset = 0;
}

void loop() {
  static int mode = 0;
  static unsigned long lastChange = 0;
  
  // Cycle through different rainbow patterns every 5 seconds
  if (millis() - lastChange > 5000) {
    mode = (mode + 1) % 6;
    lastChange = millis();
    tft.fillScreen(TFT_BLACK);
  }
  
  switch(mode) {
    case 0:
      tft.setCursor(0, 0);
      tft.setTextSize(1);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.println("Vertical Rainbow");
      drawVerticalRainbow();
      break;
      
    case 1:
      drawHorizontalRainbow();
      tft.setTextColor(TFT_BLACK, TFT_WHITE);  // Black text on white background
      tft.setCursor(10, 10);
      tft.setTextSize(2);
      tft.println("Horizontal");
      break;
      
    case 2:
      drawRainbowText();
      break;
      
    case 3:
      tft.fillScreen(TFT_BLACK);
      tft.setCursor(10, 10);
      tft.setTextColor(TFT_WHITE);
      tft.println("Smooth Gradient");
      drawSmoothRainbow();
      break;
      
    case 4:
      tft.fillScreen(TFT_BLACK);
      tft.setCursor(10, 10);
      tft.setTextColor(TFT_WHITE);
      tft.println("Diagonal Pattern");
      drawDiagonalRainbow();
      break;
      
    case 5:
      // Animated spinning rainbow
      tft.setCursor(10, 10);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.fillRect(10, 0, 150, 20, TFT_BLACK);  // Clear previous text area
      tft.setTextSize(1);
      tft.print("Spinning Rainbow ");
      tft.print((millis() / 1000) % 10);  // Show animation progress
      drawAnimatedRainbowWheel();
      delay(50);  // Faster animation speed
      return;  // Skip the delay below so animation runs smoothly
  }
  
  delay(5000);  // Wait 5 seconds before next pattern
}