#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();
String currentTitle = "Waiting...";
String currentArtist = "Play music on PC";

void updateScreen() {
  tft.fillScreen(TFT_BLACK);
  
  // Header
  tft.fillRect(0, 0, 320, 35, TFT_NAVY);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("NOW PLAYING");
  
  // Title
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 70);
  
  String displayTitle = currentTitle;
  if (displayTitle.length() > 28) displayTitle = displayTitle.substring(0, 25) + "...";
  tft.println(displayTitle);
  
  // Artist
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  
  String displayArtist = currentArtist;
  if (displayArtist.length() > 28) displayArtist = displayArtist.substring(0, 25) + "...";
  tft.println(displayArtist);
}

void setup() {
  Serial.begin(115200);
  
  // Backlight ON
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(1);
  updateScreen();
  
  // Show connection status
  tft.fillRect(0, 170, 320, 40, TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(10, 180);
  tft.println("Connected via USB");
  tft.setCursor(10, 195);
  tft.println("Waiting for data...");
}

void loop() {
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    
    // Parse "Title|Artist" format
    int separator = input.indexOf('|');
    if (separator > 0) {
      currentTitle = input.substring(0, separator);
      currentArtist = input.substring(separator + 1);
      
      // Clean up common artifacts
      currentTitle.replace(" - YouTube", "");
      currentTitle.replace(" | Spotify", "");
      
      updateScreen();
      
      // Flash the RGB LED to show activity (optional)
      pinMode(4, OUTPUT);
      digitalWrite(4, HIGH);
      delay(50);
      digitalWrite(4, LOW);
    }
  }
}