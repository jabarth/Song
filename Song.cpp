/*
 * example sketch to play audio file(s) in a directory, using the mp3 library
 * for playback and the arduino sd library to read files from a microsd card.
 * pins are setup to work well for teensy 2.0. double-check if using arduino.
 * 
 * originally based on frank zhao's player: http://frank.circleofcurrent.com/
 * utilities adapted from previous versions of the functions by matthew seal.
 *
 * (c) 2011 david sirkin sirkin@cdr.stanford.edu
 *          akil srinivasan akils@stanford.edu
 */

// first step is to include (arduino) sd, eeprom, and (our own) mp3 and lcd libraries.
#include <SD.h>
#include <EEPROM.h>

#include <mp3.h>
#include <mp3conf.h>
#include <Song.h>

// setup microsd, decoder, and lcd chip pins

#define sd_cs         12         // 'chip select' for microsd card
#define mp3_cs        21         // 'command chip select' to cs pin

#define dcs           20         // 'data chip select' to bsync pin
#define rst           18         // 'reset' to decoder's reset pin
#define dreq          19         // 'data request line' to dreq pin

// read_buffer is the amount of data read from microsd, then sent to decoder.

#define read_buffer   256        // size of the microsd read buffer
#define mp3_vol       175        // default volume: 0=min, 254=max
#define MAX_VOL       254

#define EEPROM_INIT_ID    33
#define EEPROM_FIRSTRUN 0
#define EEPROM_VOLUME   1
#define EEPROM_TRACK    2
#define EEPROM_STATE    3
#define EEPROM_POSITION 4

// file names are 13 bytes max (8 + '.' + 3 + '\0'), and the file list should
// fit into the eeprom. for example, 13 * 40 = 520 bytes of eeprom are needed
// to store a list of 40 songs. if you use shorter file names, or if your mcu
// has more eeprom, you can change these.

#define FILE_NAMES_START 32 //leave some room for persisting play info (vol, track, etc.)
#define max_name_len  13
#define max_num_songs 30

// id3v2 tags have variable-length song titles. that length is indicated in 4
// bytes within the tag. id3v1 tags also have variable-length song titles, up
// to 30 bytes maximum, but the length is not indicated within the tag. using
// 60 bytes here is a compromise between holding most titles and saving sram.

// if you increase this above 255, look for and change 'for' loop index types
// so as to not to overflow the unsigned char data type.

#define max_title_len 60

// next steps, declare the variables used later to represent microsd objects.

Sd2Card  card;                   // top-level represenation of card
SdVolume volume;                 // sd partition, not audio volume
SdFile   sd_root, sd_file;       // sd_file is the child of sd_root

// store the number of songs in this directory, and the current song to play.

unsigned char num_songs = 0, current_song = 0;

// an array to hold the current_song's file name in ram. every file's name is
// stored longer-term in the eeprom. this array is used for sd_file.open().

char fn[max_name_len];

Id3Tag tag;

// the program runs as a state machine. the 'state' enum includes the states.
// 'current_state' is the default as the program starts. add new states here.

enum state { 
  DIR_PLAY, MP3_PLAY, IDLE };
state current_state = DIR_PLAY;
state last_state = DIR_PLAY;

bool repeat = true;

// you must open any song file that you want to play using sd_file_open prior
// to fetching song data from the file. you can only open one file at a time.

int mp3Volume = mp3_vol;

//positions to keep track of % of song played
int currPosition = -1;
uint32_t bytesPlayed = 0;

void Song::sendPlayerState(){
  handler->addKeyValuePair("command", "CONNECTED", true);
  handler->addKeyValuePair("volume", getVolume());
  sendSongInfo();
}

void Song::sendSongInfo(){
	sendSongInfo(false);
}

void Song::sendSongInfo(bool first){
  handler->addKeyValuePair("title", getTitle(), first);
  handler->addKeyValuePair("artist", getArtist());
  handler->addKeyValuePair("album", getAlbum());
  handler->addKeyValuePair("songNumber", current_song);
  //handler->addKeyValuePair("filename", fn);
  //handler->addKeyValuePair("time", getTime());
  handler->addKeyValuePair("position", currPosition);
  handler->addKeyValuePair("state", isPlaying() ? "PLAYING" : "PAUSED" );
}

void Song::sd_file_open() {

Serial.println("sd_file_open()");
	sd_file.close();

  //reset position
  currPosition = 0;
  bytesPlayed = 0;

  map_current_song_to_fn();
  sd_file.open(&sd_root, fn, FILE_READ);

  // if you prefer to work with the current song index (only) instead of file
  // names, this version of the open command should also work for you:
  //sd_file.open(&sd_root, current_song, FILE_READ);
  tag.scan(&sd_file);
  sendSongInfo();
}

void Song::setSong(int songNumber){
	current_song = songNumber;
	sd_file_open();
	EEPROM.write(EEPROM_TRACK, current_song);
}

bool Song::nextFileExists(){
  if (current_song < (num_songs - 1) || repeat){
    return true; 
  }
  return false;
}

bool Song::nextFile(){
	Serial.println("nextFile()");
  if (!nextFileExists()){
	  return false;
  }

  current_song = (current_song + 1) % num_songs; 
  Serial.println(current_song);
  Serial.println(num_songs);
  sd_file_open();

  EEPROM.write(EEPROM_TRACK, current_song);
  return true;
}

bool Song::prevFileExists(){
  if (current_song > 0){
    return true; 
  }
  return false;
}

bool Song::prevFile(){
  if (!prevFileExists()) {
	  return false;
  }

  current_song--;
  sd_file_open();
  
  EEPROM.write(EEPROM_TRACK, current_song);
  return true;
}
bool seeked;

void Song::mp3_play() {
  unsigned char bytes[read_buffer]; // buffer to read and send to the decoder
  unsigned int bytes_to_read;       // number of bytes read from microsd card

    // send read_buffer bytes to be played. Mp3.play() tracks the index pointer
  // within the song being played of where to get the next read_buffer bytes.
  
  bytes_to_read = sd_file.read(bytes, read_buffer);
  Mp3.play(bytes, bytes_to_read);

  bytesPlayed += bytes_to_read;

  int pos = (bytesPlayed * 100)/getFileSize();
  if ( pos > currPosition){
	  currPosition = pos;
	  handler->addKeyValuePair("command", "SEEK", true);
	  handler->addKeyValuePair("position", currPosition);
	  handler->respond();
  }

  // bytes_to_read should only be less than read_buffer when the song's over.

  if(bytes_to_read < read_buffer) {
    sd_file.close();
    current_state = IDLE;
  }
}

uint32_t Song::getFileSize(){
	return sd_file.fileSize();
}

int Song::seek(int percent) {
  if (percent < 0 || percent > 100) return 0;
  uint32_t size = sd_file.fileSize();
  uint32_t seekPos = percent * (getFileSize() / 100);
  seeked = sd_file.seekSet(seekPos);
  currPosition = percent;
  bytesPlayed = seekPos;
  EEPROM.write(EEPROM_POSITION, currPosition);
  return percent;
}

// continue to play the current (playing) song, until there are no more songs
// in the directory to play. 

void Song::dir_play() {
  if (current_song < num_songs) {
    mp3_play();

    // if current_state is IDLE, then the currently playing song just ended.
    // in that case, increment to get the next song to play, open that file,
    // and return to the DIR_PLAY state (which will then play that song).
    // if we played the last part of the last song, we don't do anything,
    // and the current_state is already set to IDLE from mp3_play()

    if (current_state == IDLE && nextFileExists()) {
	  current_state = DIR_PLAY;
	  handler->addKeyValuePair("message","Next Song", true);
      nextFile();
	  handler->respond();
    }
  }
}

bool Song::isPlaying(){
	return current_state == MP3_PLAY || current_state == DIR_PLAY;
}

double Song::setVolume(int volume_percentage){
	double vol = volume_percentage /100.0;
	double vol2 = pow(2.7182818, vol) * 93.8;
	mp3Volume = vol2;
	Mp3.volume(mp3Volume);
	EEPROM.write(EEPROM_VOLUME, volume_percentage);
	return mp3Volume;
}

int Song::getVolume(){
	double toReturn = mp3Volume/ 93.8;
	toReturn = log(toReturn) * 100;
	return toReturn;
}

// setup is pretty straightforward. initialize serial communication (used for
// the following error messages), microsd card objects, mp3 library, and open
// the first song in the root library to play.

Song::Song() {
	tag = Id3Tag();
}

void Song::initPlayerStateFromEEPROM(){
  if (EEPROM.read(EEPROM_FIRSTRUN) == EEPROM_INIT_ID){
	//read persisted states from EEPROM
	mp3Volume = EEPROM.read(EEPROM_VOLUME);
	current_song = EEPROM.read(EEPROM_TRACK);
	current_state = (state)EEPROM.read(EEPROM_STATE);
	Serial.println("Reading player state from EEPROM");
	Serial.print("Volume: ");
	Serial.println(mp3Volume);
	Serial.print("Song: ");
	Serial.println(current_song);
	Serial.print("State: ");
	Serial.println(current_state);
  }
  else{
	  mp3Volume = mp3_vol;
	  current_song = 0;
	  current_state = DIR_PLAY;
	  currPosition = 0;
	  EEPROM.write(EEPROM_FIRSTRUN, EEPROM_INIT_ID);
	  EEPROM.write(EEPROM_VOLUME, mp3Volume);
	  EEPROM.write(EEPROM_TRACK, current_song);
	  EEPROM.write(EEPROM_STATE, current_state);
	  EEPROM.write(EEPROM_POSITION, currPosition);
	  Serial.println("First run: Initializing EEPROM state");
  }
}

void Song::setup(JsonHandler *_handler){
  Serial.begin(9600);

  handler = _handler;

  initPlayerStateFromEEPROM();

  pinMode(SS_PIN, OUTPUT);  //SS_PIN must be output to use SPI

  // the default state of the mp3 decoder chip keeps the SPI bus from 
  // working with other SPI devices, so we have to initialize it first.
  Mp3.begin(mp3_cs, dcs, rst, dreq);

  // initialize the microsd (which checks the card, volume and root objects).
  sd_card_setup();

  // initialize the mp3 library, and set default volume. 'mp3_cs' is the chip
  // select, 'dcs' is data chip select, 'rst' is reset and 'dreq' is the data
  // request. the decoder raises the dreq line (automatically) to signal that
  // it's input buffer can accommodate 32 more bytes of incoming song data.
  // we need to set the SPI speed with the mp3 initialize function since
  // it is the limiting factor, so we call its init function again.

  Mp3.begin(mp3_cs, dcs, rst, dreq);
  setVolume(mp3Volume);

  // putting all of the root directory's songs into eeprom saves flash space.

  sd_dir_setup();
//  sd_file_open();

  //can't be read with other EEPROM settings b/c sd_file_open resets currPosition
  //no need to worry about reading un-inited value b/c the initEEPROM case sets currPos
  currPosition = EEPROM.read(EEPROM_POSITION);
  seek(currPosition);

  // the program is setup to enter DIR_PLAY mode immediately, so this call to
  // open the root directory before reaching the state machine is needed.

  Serial.println("Song setup");
}

void Song::pause(){
	if (current_state != IDLE){
		last_state = current_state;
		current_state = IDLE;
		EEPROM.write(EEPROM_STATE, current_state);
	}
}

void Song::play(){
	if (current_state == IDLE){
		//set current_state to last_state unless last_state was also IDLE, then set to DIR_PLAY
		current_state = last_state != IDLE ? last_state : DIR_PLAY;
		EEPROM.write(EEPROM_STATE, current_state);
	}
}

// the state machine is setup (at least, at first) to open the microsd card's
// root directory, play all of the songs within it, close the root directory,
// and then stop playing. change these, or add new actions here.

// the DIR_PLAY state plays all of the songs in a directory and then switches
// into IDLE when done. the MP3_PLAY state plays one specified song, and then
// switches into IDLE. this example program doesn't enter the MP3_PLAY state,
// as its goal (for now) is just to play all the songs. you can change that.

void Song::loop() {
  switch(current_state) {

  case DIR_PLAY:
//	  Serial.println("DIR");
    dir_play();
    break;

  case MP3_PLAY:
	  //Serial.println("MP#");
    mp3_play();
    break;

  case IDLE:
	  //Serial.println("IDLE");
    break;
  }
}





// check that the microsd card is present, can be initialized and has a valid
// root volume. a pointer to the card's root object is returned as sd_root.

void Song::sd_card_setup() {
  if (!card.init(SPI_HALF_SPEED, sd_cs)) {
    Serial.println("Card found, but initialization failed.");
    return;
  }
  if (!volume.init(card)) {
    Serial.println("Initialized, but couldn't find partition.");
    return;
  }
  if (!sd_root.openRoot(&volume)) {
    Serial.println("Partition found, but couldn't open root");
    return;
  }
}

// for each song file in the current directory, store its file name in eeprom
// for later retrieval. this saves on using program memory for the same task,
// which is helpful as you add more functionality to the program. it also allows
// users to change the songs on the SD card, and not have to change the code to
// play new songs. if you would like to store subdirectories, talk to an 
// instructor.

void Song::sd_dir_setup() {
	int oldCurrentSong = current_song;
  handler->respondString("{\"command\": \"LIBRARY\",\"songs\":[");
  dir_t p;
  num_songs = 0;
  
  sd_root.rewind();
  
  while (sd_root.readDir(&p) > 0 && num_songs < max_num_songs) {
    // break out of while loop when we wrote all files (past the last entry).

    if (p.name[0] == DIR_NAME_FREE) {
      break;
    }
    
    // only store current (not deleted) file entries, and ignore the . and ..
    // sub-directory entries. also ignore any sub-directories.
    
    if (p.name[0] == DIR_NAME_DELETED || p.name[0] == '.' || !DIR_IS_FILE(&p)) {
      continue;
    }

    // only store mp3 or wav files in eeprom (for now). if you add other file
    // types, you should add their extension here.

    // it's okay to hard-code the 8, 9 and 10 as indices here, since SdFatLib
    // pads shorter file names with a ' ' to fill 8 characters. the result is
    // that file extensions are always stored in the last 3 positions.
    
    if ((p.name[8] == 'M' && p.name[9] == 'P' && p.name[10] == '3') ||
        (p.name[8] == 'W' && p.name[9] == 'A' && p.name[10] == 'V')) {
	  if(num_songs != 0){
		handler->respondString(",");
	  }
      // store each character of the file name into an individual byte in the
      // eeprom. sd_file->name doesn't return the '.' part of the name, so we
      // add that back later when we read the file from eeprom.
    
      unsigned char pos = 0;

      for (unsigned char i = 0; i < 11; i++) {
        if (p.name[i] != ' ') {
          EEPROM.write(FILE_NAMES_START + num_songs * max_name_len + pos, p.name[i]);
          pos++;
        }
      }
    
      // add an 'end of string' character to signal the end of the file name.
    
      EEPROM.write(FILE_NAMES_START + num_songs * max_name_len + pos, '\0');
	  current_song = num_songs;
	  map_current_song_to_fn();
	  //Serial.println("-------------------------------");
	  //Serial.println(fn);
	  sd_file.close();
	  sd_file.open(&sd_root, fn, FILE_READ);
	  
	  tag.scan(&sd_file);
	  sendSongInfo(true);
	  handler->respond(false);
	  num_songs++;
    }
  }
  //Serial.println("NM");
  //Serial.println(num_songs);
  handler->respondString("]}!");
  current_song = oldCurrentSong;
}

char* Song::getTitle(){
	return tag.getTitle();
}

char* Song::getArtist(){
	return tag.getArtist();
}

char* Song::getAlbum(){
	return tag.getAlbum();
}

char* Song::getTime(){
	return tag.getTime();
}

// given the numerical index of a particular song to play, go to its location
// in eeprom, retrieve its file name and set the global variable 'fn' to it.

void Song::map_current_song_to_fn() {
  int null_index = max_name_len - 1;
  
  // based on the current_song index, get song's name and null index position from eeprom.
  
  for (int i = 0; i < max_name_len; i++) {
    fn[i] = EEPROM.read(FILE_NAMES_START + current_song * max_name_len + i);
    
    // break if we reach the end of the file name.
    // keep track of the null index position, so we can put the '.' back.
    
    if (fn[i] == '\0') {
      null_index = i;
      break;
    }
  }
  
  // now restore the '.' that dir_t->name didn't store in its array for us.
  
  if (null_index > 3) {
    fn[null_index + 1] = '\0';
    fn[null_index]     = fn[null_index - 1];
    fn[null_index - 1] = fn[null_index - 2];
    fn[null_index - 2] = fn[null_index - 3];
    fn[null_index - 3] = '.';
  }
}