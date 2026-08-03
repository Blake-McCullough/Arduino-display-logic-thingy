#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// Current values
String currentTitle = "";
String currentArtist = "";
String currentSource = "";
int currentDuration = 0;
int currentPosition = 0;
int initialPosition = 0;
bool isPlaying = false;
bool hasValidSong = false;
bool songEnded = false;

// Previous values for change detection
String lastTitle = "";
String lastArtist = "";
String currentSongKey = "";
String lastSongKey = "";

// Scrolling text variables
String scrollingTitle = "";
String scrollingArtist = "";
int titleScrollPos = 0;
int artistScrollPos = 0;
unsigned long lastScrollUpdate = 0;
#define SCROLL_DELAY_MS 500
#define SCROLL_PADDING 5

// Time tracking
unsigned long lastDataReceived = 0;
unsigned long lastPositionUpdate = 0;
unsigned long songStartTime = 0;
unsigned long lastPauseTime = 0;

#define THUMBNAIL_SIZE 12800
uint8_t thumbnailBuffer[THUMBNAIL_SIZE];
bool hasThumbnail = false;

// ---------------------------------------------------------------------
// Audio visualization (volume + 7-band spectrum, left/right channels)
// ---------------------------------------------------------------------
#define FREQ_BAR_COUNT 7
uint8_t leftBands[FREQ_BAR_COUNT] = {0};
uint8_t rightBands[FREQ_BAR_COUNT] = {0};
uint8_t lastLeftBands[FREQ_BAR_COUNT] = {0};
uint8_t lastRightBands[FREQ_BAR_COUNT] = {0};
uint8_t volumeLeft = 0;
uint8_t volumeRight = 0;

// Sub-Bass, Bass, Low Mids, Midrange, Upper Mids, Presence, Brilliance/Air
const uint16_t bandColors[FREQ_BAR_COUNT] = {
  0xF800, // Sub-Bass    - Red
  0xFB00, // Bass        - Orange-red
  0xFD20, // Low Mids    - Orange
  0xFFE0, // Midrange    - Yellow
  0x07E0, // Upper Mids  - Green
  0x07FF, // Presence    - Cyan
  0x001F  // Brilliance/Air - Blue
};

#define AUDIO_PACKET_SIZE 16 // volL, volR, 7 left bands, 7 right bands
uint8_t audioBuffer[AUDIO_PACKET_SIZE];
int audioBytesRead = 0;

// Simple state machine
enum State {
  WAITING_FOR_START,
  READING_METADATA,
  READING_THUMBNAIL,
  READING_AUDIO,
  READING_TIMING
};

State currentState = WAITING_FOR_START;
String metadataBuffer = "";
String timingBuffer = "";
int thumbnailBytesRead = 0;

// Timeout settings
#define DATA_TIMEOUT_MS 30000
#define POSITION_UPDATE_INTERVAL_MS 500

// Display dimensions
const int SCREEN_WIDTH = 320;
const int SCREEN_HEIGHT = 240;
const int THUMB_X = (SCREEN_WIDTH - 80) / 2;
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

const int MAX_TITLE_CHARS = 24;
const int MAX_ARTIST_CHARS = 35;

// Frequency bar layout - sits in the margins to the left/right of the
// thumbnail. Bars grow upward from a shared baseline that lines up with
// the bottom of the thumbnail. Structured (array in, array out) so it's
// straightforward to swap this for a proper bar-graph widget later.
#define FREQ_BAR_WIDTH 10
#define FREQ_BAR_GAP 3
#define FREQ_BAR_MAX_HEIGHT 80
const int LEFT_BARS_X = 8;
const int RIGHT_BARS_X = THUMB_X + 82 + 8;
const int BARS_BASELINE_Y = THUMB_Y + FREQ_BAR_MAX_HEIGHT;

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Starting...");
  
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  tft.invertDisplay(true);
  
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
  songEnded = false;

  resetAudioState();
}

void resetAudioState() {
  memset(leftBands, 0, sizeof(leftBands));
  memset(rightBands, 0, sizeof(rightBands));
  memset(lastLeftBands, 0, sizeof(lastLeftBands));
  memset(lastRightBands, 0, sizeof(lastRightBands));
  volumeLeft = 0;
  volumeRight = 0;
}

void renderFullScreen() {
  Serial.println("Rendering full screen for new song");
  tft.fillScreen(TFT_BLACK);
  
  tft.drawRect(THUMB_X - 1, THUMB_Y - 1, 82, 82, TFT_RED);
  
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
  
  initScrollingText();
  drawScrollingTitle();
  drawScrollingArtist();
  
  tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
  
  if (currentSource.length() > 0) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.setTextSize(1);
    int sourceX = SCREEN_WIDTH - 10 - (currentSource.length() * 6);
    if (sourceX < 120) sourceX = 120;
    tft.setCursor(sourceX, TIME_Y);
    tft.println(currentSource);
  }
  
  updateProgressAndTime();
  updatePlayPauseState();

  // Force a fresh draw of the frequency bars against the new background
  memset(lastLeftBands, 0, sizeof(lastLeftBands));
  memset(lastRightBands, 0, sizeof(lastRightBands));
  updateFrequencyDisplay();
  
  hasValidSong = true;
  songEnded = false;
}

void initScrollingText() {
  scrollingTitle = currentTitle;
  if (scrollingTitle.length() > MAX_TITLE_CHARS) {
    scrollingTitle = currentTitle + "   " + currentTitle + "   ";
  }
  
  scrollingArtist = currentArtist;
  if (scrollingArtist.length() > MAX_ARTIST_CHARS) {
    scrollingArtist = currentArtist + "   " + currentArtist + "   ";
  }
  
  titleScrollPos = 0;
  artistScrollPos = 0;
  lastScrollUpdate = millis();
}

void drawScrollingTitle() {
  tft.fillRect(TITLE_X, TITLE_Y - 20, SCREEN_WIDTH - 20, 30, TFT_BLACK);
  
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextSize(2);
  
  if (currentTitle.length() <= MAX_TITLE_CHARS) {
    tft.setCursor(TITLE_X, TITLE_Y);
    tft.println(currentTitle);
  } else {
    tft.setCursor(TITLE_X, TITLE_Y);
    String displayText = scrollingTitle.substring(titleScrollPos, titleScrollPos + MAX_TITLE_CHARS);
    tft.println(displayText);
  }
}

void drawScrollingArtist() {
  tft.fillRect(ARTIST_X, ARTIST_Y - 10, SCREEN_WIDTH - 20, 20, TFT_BLACK);
  
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextSize(1);
  
  if (currentArtist.length() <= MAX_ARTIST_CHARS) {
    tft.setCursor(ARTIST_X, ARTIST_Y);
    tft.println(currentArtist);
  } else {
    tft.setCursor(ARTIST_X, ARTIST_Y);
    String displayText = scrollingArtist.substring(artistScrollPos, artistScrollPos + MAX_ARTIST_CHARS);
    tft.println(displayText);
  }
}

void updateScrolling() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastScrollUpdate >= SCROLL_DELAY_MS) {
    bool needsUpdate = false;
    
    if (currentTitle.length() > MAX_TITLE_CHARS) {
      titleScrollPos++;
      if (titleScrollPos > (int)(currentTitle.length() + 3)) titleScrollPos = 0;
      needsUpdate = true;
    }
    
    if (currentArtist.length() > MAX_ARTIST_CHARS) {
      artistScrollPos++;
      if (artistScrollPos > (int)(currentArtist.length() + 3)) artistScrollPos = 0;
      needsUpdate = true;
    }
    
    if (needsUpdate) {
      drawScrollingTitle();
      drawScrollingArtist();
    }
    
    lastScrollUpdate = currentTime;
  }
}

// ---------------------------------------------------------------------
// Frequency bar graphs (left + right of thumbnail)
// ---------------------------------------------------------------------

// Draws one 7-bar column. Only redraws a bar if its height actually
// changed, to keep this cheap enough to run at ~20Hz. `bands`/`lastBands`
// let the same function serve both the left and right channel.
void drawFrequencyBars(bool leftSide, uint8_t bands[FREQ_BAR_COUNT], uint8_t lastBands[FREQ_BAR_COUNT]) {
  int baseX = leftSide ? LEFT_BARS_X : RIGHT_BARS_X;

  for (int i = 0; i < FREQ_BAR_COUNT; i++) {
    int newHeight = map(bands[i], 0, 100, 0, FREQ_BAR_MAX_HEIGHT);
    newHeight = constrain(newHeight, 0, FREQ_BAR_MAX_HEIGHT);
    int oldHeight = lastBands[i]; // already stored as pixel height, see below

    if (newHeight == oldHeight) continue;

    int barX = baseX + i * (FREQ_BAR_WIDTH + FREQ_BAR_GAP);

    // Clear the full column, then redraw. Simple and flicker-free enough
    // at this size/refresh rate - swap for a proper bar-graph widget
    // later if you want smoother interpolation between updates.
    tft.fillRect(barX, THUMB_Y, FREQ_BAR_WIDTH, FREQ_BAR_MAX_HEIGHT, TFT_BLACK);

    if (newHeight > 0) {
      int barY = BARS_BASELINE_Y - newHeight;
      tft.fillRect(barX, barY, FREQ_BAR_WIDTH, newHeight, bandColors[i]);
    }
  }
}

void updateFrequencyDisplay() {
  if (!hasValidSong) return;

  // Convert current 0-100 values to pixel heights once so the per-bar
  // comparison above is a plain integer compare.
  static uint8_t leftPx[FREQ_BAR_COUNT];
  static uint8_t rightPx[FREQ_BAR_COUNT];
  for (int i = 0; i < FREQ_BAR_COUNT; i++) {
    leftPx[i] = constrain(map(leftBands[i], 0, 100, 0, FREQ_BAR_MAX_HEIGHT), 0, FREQ_BAR_MAX_HEIGHT);
    rightPx[i] = constrain(map(rightBands[i], 0, 100, 0, FREQ_BAR_MAX_HEIGHT), 0, FREQ_BAR_MAX_HEIGHT);
  }

  drawFrequencyBars(true, leftBands, lastLeftBands);
  drawFrequencyBars(false, rightBands, lastRightBands);

  // Store the last-drawn heights (not the raw 0-100 values) so the change
  // check in drawFrequencyBars is comparing like-for-like pixel heights.
  memcpy(lastLeftBands, leftPx, FREQ_BAR_COUNT);
  memcpy(lastRightBands, rightPx, FREQ_BAR_COUNT);
}

void parseAudioPacket() {
  volumeLeft = audioBuffer[0];
  volumeRight = audioBuffer[1];
  for (int i = 0; i < FREQ_BAR_COUNT; i++) {
    leftBands[i] = audioBuffer[2 + i];
    rightBands[i] = audioBuffer[9 + i];
  }
  updateFrequencyDisplay();
}

void loop() {
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
  
  if (hasValidSong) {
    updateScrolling();
  }
  
  if (hasValidSong && currentDuration > 0 && !songEnded) {
    updatePositionFromTime();
  }
  
  delay(10);
}

void updatePositionFromTime() {
  if (!isPlaying) {
    if (millis() - lastPositionUpdate > POSITION_UPDATE_INTERVAL_MS) {
      updateProgressAndTime();
      updatePlayPauseState();
      lastPositionUpdate = millis();
    }
    return;
  }
  
  unsigned long currentMillis = millis();
  unsigned long elapsedMillis = currentMillis - songStartTime;
  int elapsedSeconds = elapsedMillis / 1000;
  
  int calculatedPosition = initialPosition + elapsedSeconds;
  
  if (calculatedPosition >= currentDuration) {
    calculatedPosition = currentDuration;
    
    if (!songEnded) {
      songEnded = true;
      Serial.println("Song reached end - stopping time tracking");
      updateProgressAndTime();
      updatePlayPauseState();
    }
  }
  
  if (calculatedPosition != currentPosition && !songEnded) {
    currentPosition = calculatedPosition;
    lastPositionUpdate = currentMillis;
    updateProgressAndTime();
    updatePlayPauseState();
  } else if (millis() - lastPositionUpdate > POSITION_UPDATE_INTERVAL_MS && !songEnded) {
    updateProgressAndTime();
    updatePlayPauseState();
    lastPositionUpdate = currentMillis;
  }
}

void handlePlayStateChange(bool newPlayingState) {
  if (newPlayingState == isPlaying) return;
  
  if (songEnded) {
    Serial.println("Song has ended - ignoring play/pause change");
    return;
  }
  
  isPlaying = newPlayingState;
  
  if (isPlaying) {
    if (lastPauseTime > 0) {
      unsigned long pauseDuration = millis() - lastPauseTime;
      songStartTime += pauseDuration;
      lastPauseTime = 0;
    }
    Serial.println("Playback resumed");
  } else {
    lastPauseTime = millis();
    Serial.println("Playback paused");
  }
  
  updatePlayPauseState();
}

void resetToWaitingState() {
  currentState = WAITING_FOR_START;
  metadataBuffer = "";
  timingBuffer = "";
  thumbnailBytesRead = 0;
  audioBytesRead = 0;
  hasThumbnail = false;
  isPlaying = false;
  currentDuration = 0;
  currentPosition = 0;
  initialPosition = 0;
  currentTitle = "";
  currentArtist = "";
  currentSource = "";
  hasValidSong = false;
  currentSongKey = "";
  lastSongKey = "";
  songStartTime = 0;
  lastPauseTime = 0;
  titleScrollPos = 0;
  artistScrollPos = 0;
  songEnded = false;
  resetAudioState();
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
        } else if (c == 'A') {
          currentState = READING_AUDIO;
          audioBytesRead = 0;
        } else if (c == 'T') {
          currentState = READING_TIMING;
          timingBuffer = "";
        }
        break;
        
      case READING_METADATA:
        if (c == '\n') {
          parseMetadata(metadataBuffer);
          currentState = READING_THUMBNAIL;
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
            lastDataReceived = millis();
            
            String newSongKey = currentTitle + "|" + currentArtist;
            
            if (newSongKey != lastSongKey) {
              Serial.println("New song detected - full refresh");
              lastSongKey = newSongKey;
              
              songStartTime = millis();
              initialPosition = currentPosition;
              lastPauseTime = 0;
              songEnded = false;
              
              renderFullScreen();
            } else {
              Serial.println("Same song - updating timing");
              
              if (songEnded) {
                songEnded = false;
              }
              
              if (isPlaying) {
                songStartTime = millis();
                initialPosition = currentPosition;
              }
              
              if (currentTitle != lastTitle || currentArtist != lastArtist) {
                initScrollingText();
                drawScrollingTitle();
                drawScrollingArtist();
                lastTitle = currentTitle;
                lastArtist = currentArtist;
              }
              
              updateProgressAndTime();
              updatePlayPauseState();
            }
            
            digitalWrite(4, HIGH);
            delay(200);
            digitalWrite(4, LOW);
          }
        }
        break;

      case READING_AUDIO:
        audioBuffer[audioBytesRead++] = (uint8_t)c;
        if (audioBytesRead >= AUDIO_PACKET_SIZE) {
          parseAudioPacket();
          currentState = WAITING_FOR_START;
        }
        break;

      case READING_TIMING:
        if (c == '\n') {
          parseTiming(timingBuffer);
          currentState = WAITING_FOR_START;
        } else {
          timingBuffer += c;
        }
        break;
    }
  }
}

// Lightweight update: position/duration/play-state only, no thumbnail or
// title/artist. Sent much more often than the full packet so the on-screen
// clock stays accurate without re-transmitting the thumbnail every time.
void parseTiming(String data) {
  if (!hasValidSong) return;

  int firstPipe = data.indexOf('|');
  int secondPipe = data.indexOf('|', firstPipe + 1);

  if (firstPipe <= 0 || secondPipe <= 0) {
    Serial.println("Failed to parse timing update!");
    return;
  }

  int newDuration = data.substring(0, firstPipe).toInt();
  int newPosition = data.substring(firstPipe + 1, secondPipe).toInt();
  bool newPlayingState = (data.substring(secondPipe + 1) == "True" || data.substring(secondPipe + 1) == "true");

  currentDuration = newDuration;
  currentPosition = newPosition;
  initialPosition = newPosition;
  songStartTime = millis();
  lastPauseTime = 0;

  if (currentPosition < currentDuration) {
    songEnded = false;
  }

  bool stateChanged = (newPlayingState != isPlaying);
  if (stateChanged) {
    handlePlayStateChange(newPlayingState);
  }

  updateProgressAndTime();
  if (!stateChanged) {
    updatePlayPauseState();
  }
}

void parseMetadata(String metadata) {
  int firstPipe = metadata.indexOf('|');
  int secondPipe = metadata.indexOf('|', firstPipe + 1);
  int thirdPipe = metadata.indexOf('|', secondPipe + 1);
  int fourthPipe = metadata.indexOf('|', thirdPipe + 1);
  int fifthPipe = metadata.indexOf('|', fourthPipe + 1);
  
  if (firstPipe > 0 && secondPipe > 0 && thirdPipe > 0 && fourthPipe > 0 && fifthPipe > 0) {
    String newTitle = metadata.substring(0, firstPipe);
    String newArtist = metadata.substring(firstPipe + 1, secondPipe);
    int newDuration = metadata.substring(secondPipe + 1, thirdPipe).toInt();
    int newPosition = metadata.substring(thirdPipe + 1, fourthPipe).toInt();
    String newSource = metadata.substring(fourthPipe + 1, fifthPipe);
    bool newPlayingState = (metadata.substring(fifthPipe + 1) == "True" || metadata.substring(fifthPipe + 1) == "true");
    
    String newSongKey = newTitle + "|" + newArtist;
    bool isNewSong = (newSongKey != lastSongKey);
    
    currentTitle = newTitle;
    currentArtist = newArtist;
    currentDuration = newDuration;
    currentSource = newSource;
    currentPosition = newPosition;
    initialPosition = newPosition;
      
    currentSource.replace("Microsoft.", "");
    currentSource.replace("AppleInc.", "Apple ");
    if (currentSource.length() > 15) {
      currentSource = currentSource.substring(0, 12) + "...";
    }
    
    if (isNewSong) {
      songEnded = false;
      handlePlayStateChange(newPlayingState);
    } else {
      if (newPlayingState != isPlaying && !songEnded) {
        handlePlayStateChange(newPlayingState);
      }
    }
  } else {
    Serial.println("Failed to parse metadata!");
  }
}

void updateProgressAndTime() {
  if (!hasValidSong) return;
  
  if (currentDuration > 0) {
    int progressWidth = (currentPosition * BAR_WIDTH) / currentDuration;
    
    tft.fillRect(BAR_X, BAR_Y, BAR_WIDTH, BAR_HEIGHT, TFT_DARKGREY);
    if (progressWidth > 0) {
      tft.fillRect(BAR_X, BAR_Y, progressWidth, BAR_HEIGHT, TFT_GREEN);
    }
    
    tft.fillRect(TIME_X, TIME_Y - 8, 100, 16, TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(TIME_X, TIME_Y);
    tft.println(formatTime(currentPosition) + " / " + formatTime(currentDuration));
  }
}

void updatePlayPauseState() {
  if (!hasValidSong) return;
  
  tft.fillRect(BAR_X, BAR_Y - 12, BAR_WIDTH, 12, TFT_BLACK);
  
  if (!songEnded && !isPlaying && currentPosition < currentDuration) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(BAR_X + (BAR_WIDTH / 2) - 30, BAR_Y - 2);
    tft.println("PAUSED");
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
