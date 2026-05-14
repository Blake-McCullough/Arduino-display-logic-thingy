#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// Current values
String currentTitle = "";
String currentArtist = "";
String currentSource = "";
int currentDuration = 0;
int currentPosition = 0;
bool isPlaying = false;
bool hasValidSong = false;

// Previous values for change detection
String lastTitle = "";
String lastArtist = "";
String lastSource = "";
int lastDuration = -1;
int lastPosition = -1;
bool lastIsPlaying = false;
bool lastHasThumbnail = false;
String currentSongKey = "";
String lastSongKey = "";

unsigned long lastDataReceived = 0;
unsigned long lastPositionUpdate = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
uint8_t lastThumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

// Simple state machine
enum State {
  WAITING_FOR_START,
  READING_METADATA,
  READING_THUMBNAIL
};

State currentState = WAITING_FOR_START;
String metadataBuffer = "";
int thumbnailBytesRead = 0;

// Timeout settings
#define DATA_TIMEOUT_MS 30000
#define POSITION_UPDATE_INTERVAL_MS 1000

// Screen positions (assuming 320x240 display)
const int THUMB_X = (320 - 80) / 2;
const int THUMB_Y = 10;
const int TITLE_X = 10;
const int TITLE_Y = 110;
const int ARTIST_X = 10;
const int ARTIST_Y = 145;
const int BAR_X = 10;
const int BAR_Y = 175;
const int BAR_WIDTH = 300;
const int BAR_HEIGHT = 8;
const int TIME_X = 10;
const int TIME_Y = 195;

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  
  showWaitingScreen();
  
  pinMode(4, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(4, HIGH);
    delay(100);
    digitalWrite(4, LOW);
    delay(100);
  }
  
  Serial.println("Ready to receive data");
  lastDataReceived = millis();
}

void showWaitingScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 120);
  tft.println("Waiting for");
  tft.setCursor(10, 150);
  tft.println("music...");
  
  hasValidSong = false;
  currentSongKey = "";
  lastSongKey = "";
}

void renderFullScreen() {
  Serial.println("Rendering full screen for new song");
  tft.fillScreen(TFT_BLACK);
  
  // Draw thumbnail border
  tft.drawRect(THUMB_X - 1, THUMB_Y - 1, 82, 82, TFT_RED);
  
  // Draw thumbnail
  if (hasThumbnail) {
    for (int y = 0; y < 80; y++) {
      for (int x = 0; x < 80; x++) {
        int idx = (y * 80 + x) * 2;
        if (idx + 1 < THUMBNAIL_SIZE) {
          uint16_t color = thumbnailBuffer[idx] | (thumbnailBuffer[idx + 1] << 8);
          tft.drawPixel(THUMB_X + x, THUMB_Y + y, color);
        }
      }
    }
  } else {
    tft.fillRect(THUMB_X, THUMB_Y, 80, 80, TFT_DARKGREY);
    tft.drawRect(THUMB_X, THUMB_Y, 80, 80, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
    tft.setTextSize(1);
    tft.setCursor(THUMB_X + 20, THUMB_Y + 35);
    tft.println("NO");
    tft.setCursor(THUMB_X + 15, THUMB_Y + 50);
    tft.println("ART");
  }
  
  // Draw title
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(TITLE_X, TITLE_Y);
  String displayTitle = currentTitle;
  if (displayTitle.length() > 24) {
    displayTitle = displayTitle.substring(0, 22) + "...";
  }
  tft.println(displayTitle);
  
  // Draw artist
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(ARTIST_X, ARTIST_Y);
  String displayArtist = currentArtist;
  if (displayArtist.length() > 35) {
    displayArtist = displayArtist.substring(0, 32) + "...";
  }
  tft.println(displayArtist);
  
  // Draw progress bar background
  tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
  
  // Draw source
  if (currentSource.length() > 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextSize(1);
    int sourceX = tft.width() - 10 - (currentSource.length() * 6);
    if (sourceX < 120) sourceX = 120;
    tft.setCursor(sourceX, TIME_Y);
    tft.println(currentSource);
  }
  
  // Draw initial time and progress
  updateProgressAndTime();
  updatePlayPauseState();
  
  hasValidSong = true;
}

void loop() {
  // Check for timeout
  if (millis() - lastDataReceived > DATA_TIMEOUT_MS) {
    if (hasValidSong) {
      Serial.println("Data timeout - showing waiting screen");
      showWaitingScreen();
      resetToWaitingState();
    }
  }
  
  if (Serial.available()) {
    processIncomingData();
    lastDataReceived = millis();
  }
  
  // Update position only if playing and we have a valid song
  if (hasValidSong && currentDuration > 0 && isPlaying) {
    updatePosition();
  }
  
  delay(10);
}

void resetToWaitingState() {
  currentState = WAITING_FOR_START;
  metadataBuffer = "";
  thumbnailBytesRead = 0;
  hasThumbnail = false;
  isPlaying = false;
  currentDuration = 0;
  currentPosition = 0;
  currentTitle = "";
  currentArtist = "";
  currentSource = "";
  hasValidSong = false;
  currentSongKey = "";
  lastSongKey = "";
}

void processIncomingData() {
  while (Serial.available()) {
    char c = Serial.read();
    
    switch (currentState) {
      case WAITING_FOR_START:
        if (c == 'S') {
          currentState = READING_METADATA;
          metadataBuffer = "";
          thumbnailBytesRead = 0;
          Serial.println("Start marker received");
        }
        break;
        
      case READING_METADATA:
        if (c == '\n') {
          parseMetadata(metadataBuffer);
          currentState = READING_THUMBNAIL;
          Serial.println("Metadata received, waiting for thumbnail...");
        } else {
          metadataBuffer += c;
        }
        break;
        
      case READING_THUMBNAIL:
        if (thumbnailBytesRead < THUMBNAIL_SIZE) {
          thumbnailBuffer[thumbnailBytesRead++] = c;
          
          if (thumbnailBytesRead == THUMBNAIL_SIZE) {
            hasThumbnail = true;
            currentState = WAITING_FOR_START;
            lastPositionUpdate = millis();
            lastDataReceived = millis();
            
            Serial.println("Thumbnail received complete!");
            
            // Check if this is a new song (different title + artist)
            String newSongKey = currentTitle + "|" + currentArtist;
            
            if (newSongKey != lastSongKey) {
              // New song - render everything
              Serial.println("New song detected - full refresh");
              lastSongKey = newSongKey;
              renderFullScreen();
            } else {
              // Same song - just update time and progress if needed
              Serial.println("Same song - updating only time/progress");
              updateProgressAndTime();
              updatePlayPauseState();
            }
            
            // Flash LED to show we got data
            digitalWrite(4, HIGH);
            delay(200);
            digitalWrite(4, LOW);
            
            Serial.println("Ready for next song");
          }
        }
        break;
    }
  }
}

void updatePosition() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastPositionUpdate >= POSITION_UPDATE_INTERVAL_MS) {
    if (currentPosition < currentDuration) {
      currentPosition++;
      lastPositionUpdate = currentTime;
      
      // Only update progress bar and time
      updateProgressAndTime();
      
      if (currentPosition >= currentDuration) {
        Serial.println("Song ended");
        isPlaying = false;
        updatePlayPauseState();
      }
    }
  }
}

void parseMetadata(String metadata) {
  Serial.print("Metadata: ");
  Serial.println(metadata);
  
  int firstPipe = metadata.indexOf('|');
  int secondPipe = metadata.indexOf('|', firstPipe + 1);
  int thirdPipe = metadata.indexOf('|', secondPipe + 1);
  int fourthPipe = metadata.indexOf('|', thirdPipe + 1);
  int fifthPipe = metadata.indexOf('|', fourthPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0 && fourthPipe > 0 && fifthPipe > 0) {
    currentTitle = metadata.substring(0, firstPipe);
    currentArtist = metadata.substring(firstPipe + 1, secondPipe);
    currentDuration = metadata.substring(secondPipe + 1, thirdPipe).toInt();
    currentPosition = metadata.substring(thirdPipe + 1, fourthPipe).toInt();
    currentSource = metadata.substring(fourthPipe + 1, fifthPipe);
    String playingStatus = metadata.substring(fifthPipe + 1);
    isPlaying = (playingStatus == "True" || playingStatus == "true");
    
    // Clean up source string
    currentSource.replace("Microsoft.", "");
    currentSource.replace("AppleInc.", "Apple ");
    if (currentSource.length() > 15) {
      currentSource = currentSource.substring(0, 12) + "...";
    }
    
    lastPositionUpdate = millis();
    
    Serial.println("Parsed values:");
    Serial.print("  Title: ");
    Serial.println(currentTitle);
    Serial.print("  Artist: ");
    Serial.println(currentArtist);
    Serial.print("  Duration: ");
    Serial.println(currentDuration);
    Serial.print("  Position: ");
    Serial.println(currentPosition);
    Serial.print("  Source: ");
    Serial.println(currentSource);
    Serial.print("  IsPlaying: ");
    Serial.println(isPlaying ? "Yes" : "No");
  } else {
    Serial.println("Failed to parse metadata!");
  }
}

void updateProgressAndTime() {
  if (!hasValidSong) return;
  
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * BAR_WIDTH) / currentDuration;
    
    // Update progress bar
    tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(BAR_X, BAR_Y, progressWidth, BAR_HEIGHT, TFT_GREEN);
    }
    
    // Update time text
    tft.fillRect(TIME_X, TIME_Y - 8, 100, 16, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(TIME_X, TIME_Y);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
    
    // Debug output throttled
    static int debugCounter = 0;
    if (debugCounter++ % 10 == 0) {
      Serial.print("Progress updated: ");
      Serial.print(currentPosition);
      Serial.print("/");
      Serial.println(currentDuration);
    }
  }
}

void updatePlayPauseState() {
  if (!hasValidSong) return;
  
  if (isPlaying != lastIsPlaying) {
    Serial.println(isPlaying ? "Playback resumed" : "Playback paused");
    
    // Clear the area where PAUSED text appears
    tft.fillRect(BAR_X, BAR_Y - 12, BAR_WIDTH, 12, TFT_BLACK);
    
    if (!isPlaying) {
      // Show PAUSED text
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextSize(1);
      tft.setCursor(BAR_X + (BAR_WIDTH / 2) - 30, BAR_Y - 2);
      tft.println("PAUSED");
    }
    
    lastIsPlaying = isPlaying;
  }
}

String formatTime(int seconds) {
  if (seconds < 0) seconds = 0;
  int mins = seconds / 60;
  int secs = seconds % 60;
  String result = "";
  if (mins < 10) result += "0";
  result += String(mins) + ":";
  if (secs < 10) result += "0";
  result += String(secs);
  return result;
}