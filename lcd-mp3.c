/*
 *	lcd-mp3
 *	mp3 player with output to a 16x2 lcd display for song information
 *
 *      requires:
 *		pthread
 *		wiringPi
 *		ao
 *		mpg123
 *	John Wiggins (jcwiggi@gmail.com)
 *
 *
 *	25-11-2014	added delay in button read
 *	23-11-2014	add button support
 *	22-11-2014	moved from an array of songs to a linked list (playlist) of songs.
 *	18-11-2014	lots of re-work, added more info to curses display and the ability to
 *			swap what is shown on the second row; album vs artist
 *	17-11-2014	attempt to add previous song
 *	17-11-2014	added quit
 *	16-11-2014	added skip song / next song
 *	14-11-2014	added ability to pause thread/song; using ncurses, got keboard commands
 *	12-11-2014	added playback of multiple songs
 *	10-11-2014	added ID3 parsing of MP3 file
 *	04-11-2014	made the song playing part a thread
 *	02-11-2014	able to play a mp3 file using mpg123 and ao libraries
 *	28-10-2014	worked on scrolling text on lcd
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <libgen.h>

#include <wiringPi.h>
#include <lcd.h>

#include "lcd-mp3.h"

#define playButtonPin 0
#define prevButtonPin 1
#define nextButtonPin 2
#define infoButtonPin 5
#define quitButtonPin 7

#define DeLaY 50

const int buttonPins[] = { 
	playButtonPin,
	nextButtonPin,
	prevButtonPin,
	infoButtonPin,
	quitButtonPin
	};

// Variables for the LCD display
const int BS = 4;	// Bits (4 or 8)
const int CO = 16;	// Number of columns
const int RO = 2;	// Number of rows
const int RS = 3;	// 
const int EN = 14;	// 
const int D0 = 4;	// 
const int D1 = 12;	// 
const int D2 = 13;	// 
const int D3 = 6;	// 

/*
 * linked list / playlist functions
 */

int playlist_init(playlist_t *playlistptr)
{
  *playlistptr = NULL;
  return 1;
}

int playlist_add_song(int index, void *songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev, *new;
  int found = FALSE;

  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      free(cur->songptr);
      cur->songptr = songptr;
      found = TRUE;
      break;
    }
    else if (cur->index > index)
      break;
  }
  if (!found)
  {
    new = (playlist_node_t *)malloc(sizeof(playlist_node_t));
    new->index = index;
    new->songptr = songptr;
    new->nextptr = cur;
    if (cur == *playlistptr)
      *playlistptr = new;
    else
      prev->nextptr = new;
  }
  return 1;
}

int playlist_get_song(int index, void **songptr, playlist_t *playlistptr)
{
  playlist_node_t *cur, *prev;

  // Initialize to "not found"
  *songptr = NULL;
  // Look through index for our entry
  for (cur = prev = *playlistptr; cur != NULL; prev = cur, cur = cur->nextptr)
  {
    if (cur->index == index)
    {
      *songptr = cur->songptr;
      break;
    }
    else if (cur->index > index)
      break;
  }
  return 1;
}


// pthread stuff
void nextSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = NEXT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void prevSong()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PREV;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void quitMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = QUIT;
	cur_song.song_over = TRUE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void pauseMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PAUSE;
	pthread_mutex_unlock(&cur_song.pauseMutex);
}


void playMe()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	cur_song.play_status = PLAY;
	pthread_cond_broadcast(&cur_song.m_resumeCond);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

void checkPause()
{
	pthread_mutex_lock(&cur_song.pauseMutex);
	while (cur_song.play_status == PAUSE)
		pthread_cond_wait(&cur_song.m_resumeCond, &cur_song.pauseMutex);
	pthread_mutex_unlock(&cur_song.pauseMutex);
}

// ID3 stuff

// Split up a number of lines separated by \n, \r, both or just zero byte
//   and print out each line with specified prefix.
void make_id(mpg123_string *inlines, int type)
{
	size_t i;
	int hadcr = 0, hadlf = 0;
	char *lines = NULL;
	char *line  = NULL;
	size_t len = 0;
	char tmp_name[100];

	if (inlines != NULL && inlines->fill)
	{
		lines = inlines->p;
		len = inlines->fill;
	}
	else
		return;
	line = lines;
	for (i = 0; i < len; ++i)
	{
		if (lines[i] == '\n' || lines[i] == '\r' || lines[i] == 0)
		{
			// saving, changing, restoring a byte in the data
			char save = lines[i];
			if (save == '\n')
				++hadlf;
			if (save == '\r')
				++hadcr;
			if ((hadcr || hadlf) && hadlf % 2 == 0 && hadcr % 2 == 0)
				line = "";
			if (line)
			{
				lines[i] = 0;
				strncpy(tmp_name, line, 100);
				line = NULL;
				lines[i] = save;
			}
		}
		else
		{
			hadlf = hadcr = 0;
			if (line == NULL)
				line = lines + i;
		}
	}
	switch (type)
	{
		case  TITLE: strcpy(cur_song.title,  tmp_name); break;
		case ARTIST: strcpy(cur_song.artist, tmp_name); break;
		case  GENRE: strcpy(cur_song.genre,  tmp_name); break;
		case  ALBUM: strcpy(cur_song.album,  tmp_name); break;
	}
}

int id3_tagger()
{
	int meta;
	mpg123_handle* m;
	mpg123_id3v1 *v1;
	mpg123_id3v2 *v2;

	// ID3 tag info for the song
	mpg123_init();
	m = mpg123_new(NULL, NULL);
	if (mpg123_open(m, cur_song.filename) != MPG123_OK)
	{
		fprintf(stderr, "Cannot open %s: %s\n", cur_song.filename, mpg123_strerror(m));
		return 1;
	}
	mpg123_scan(m);
	meta = mpg123_meta_check(m);
	if (meta & MPG123_ID3 && mpg123_id3(m, &v1, &v2) == MPG123_OK)
	{
		make_id(v2->title, TITLE);
		make_id(v2->artist, ARTIST);
		make_id(v2->album, ALBUM);
		make_id(v2->genre, GENRE);
	}
	else
	{
		sprintf(cur_song.title, "UNKNOWN");
		sprintf(cur_song.artist, "UNKNOWN");
		sprintf(cur_song.album, "UNKNOWN");
		sprintf(cur_song.genre, "UNKNOWN");
	}
	// if there is no title to be found, set title to the song file name.
	if (strlen(cur_song.title) == 0)
		strcpy(cur_song.title, cur_song.base_filename);
	if (strlen(cur_song.artist) == 0)
		sprintf(cur_song.artist, "UNKNOWN");
	if (strlen(cur_song.album) == 0)
		sprintf(cur_song.album, "UNKNOWN");
	// set the second row to be the artist by default.
	strcpy(cur_song.second_row_text, cur_song.artist);
	mpg123_close(m);
	mpg123_delete(m);
	mpg123_exit();
	// the following two lines are just to see when the scrolling should pause
	strncpy(cur_song.scroll_firstRow, cur_song.title, 15);
	strncpy(cur_song.scroll_secondRow, cur_song.second_row_text, 16);
	return 0;
}

int printLcdFirstRow()
{
	int flag = TRUE;
	// have to set to 15 because of music note
	if (strlen(cur_song.title) < 15)
	{
		lcdCharDef(lcdHandle, 2, musicNote);
		lcdPosition(lcdHandle, 0, 0);
		lcdPutchar(lcdHandle, 2);
		lcdPosition(lcdHandle, 1, 0);
		lcdPuts(lcdHandle, cur_song.title);
		flag = FALSE;
	}
	return flag;
}

int printLcdSecondRow()
{
	int flag = TRUE;
	if (strlen(cur_song.second_row_text) < 16)
	{
		lcdPosition(lcdHandle, 0, 1);
		lcdPuts(lcdHandle, cur_song.second_row_text);
		flag = FALSE;
	}
	return flag;
}

int usage(const char *progName)
{
	fprintf (stderr, "Usage: %s song-name\n", progName);
	return EXIT_FAILURE;
}

void scrollMessage_firstRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 15;
	char my_songname[MAXDATALEN];

	strcpy(my_songname, spaces);
	strncat(my_songname, cur_song.title, strlen(cur_song.title));
	strcat(my_songname, spaces);
	my_songname[strlen(my_songname) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_songname[position], width);
	buf[width] = 0;
	lcdCharDef(lcdHandle, 2, musicNote);
	lcdPosition(lcdHandle, 0, 0);
	lcdPutchar(lcdHandle, 2);
	lcdPosition(lcdHandle, 1, 0);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_firstRow) == 0)
		delay(1500);
	if (position == (strlen(my_songname) - width))
		position = 0;
}

void scrollMessage_secondRow(void)
{
	char buf[32];
	static int position = 0;
	static int timer = 0;
	int width = 16;
	char my_string[MAXDATALEN];

	strcpy(my_string, spaces);
	strncat(my_string, cur_song.second_row_text, strlen(cur_song.second_row_text));
	strcat(my_string, spaces);
	my_string[strlen(my_string) + 1] = 0;
	if (millis() < timer)
		return;
	timer = millis() + 200;
	strncpy(buf, &my_string[position], width);
	buf[width] = 0;
	lcdPosition(lcdHandle, 0, 1);
	lcdPuts(lcdHandle, buf);
	position++;
	// pause briefly when text reaches begining line before continuing
	if (strcmp(buf, cur_song.scroll_secondRow) == 0)
		delay(1500);
	if (position == (strlen(my_string) - width))
		position = 0;
}

// The actual thing that plays the song
void *play_song(void *arguments)
{
	struct song_info *args = (struct song_info *)arguments;
	mpg123_handle *mh;
	mpg123_pars *mpar;
	unsigned char *buffer;
	size_t buffer_size;
	size_t done;
	int err;

	int driver;
	ao_device *dev;
	ao_sample_format format;
	int channels, encoding;
	long rate;

	ao_initialize();
	driver = ao_default_driver_id();
	mpg123_init();
	// try to not show error messages
	mh = mpg123_new(NULL, &err);
	mpar = mpg123_new_pars(&err);
	mpg123_par(mpar, MPG123_ADD_FLAGS, MPG123_QUIET, 0);
	mh = mpg123_parnew(mpar, NULL, &err);
	buffer_size = mpg123_outblock(mh);
	buffer = (unsigned char*) malloc(buffer_size * sizeof(unsigned char));
	// open the file and get the decoding format
	mpg123_open(mh, args->filename);
	mpg123_getformat(mh, &rate, &channels, &encoding);
	// set the output format and open the output device
	format.bits = mpg123_encsize(encoding) * 8;
	format.rate = rate;
	format.channels = channels;
	format.byte_format = AO_FMT_NATIVE;
	format.matrix = 0;
	dev = ao_open_live(driver, &format, NULL);
	// decode and play
	while (mpg123_read(mh, buffer, buffer_size, &done) == MPG123_OK)
	{
		checkPause();
		ao_play(dev, buffer, done);
		// stop playing if the user pressed quit, next, or prev buttons
		if (cur_song.play_status == QUIT || cur_song.play_status == NEXT || cur_song.play_status == PREV)
			break;
	}
	// clean up
	free(buffer);
	ao_close(dev);
	mpg123_close(mh);
	mpg123_delete(mh);
	mpg123_exit();
	ao_shutdown();
	pthread_mutex_lock(&(cur_song.writeMutex));
	args->song_over = TRUE;
	// only set the status to play if the song finished normally
	if (cur_song.play_status != QUIT && cur_song.play_status != NEXT && cur_song.play_status != PREV)
		args->play_status = PLAY;
	cur_status.song_over = TRUE;
	pthread_mutex_unlock(&(cur_song.writeMutex));
}

int main (int argc, char **argv)
{
	char *basec, *bname;
	pthread_t song_thread;
	int scroll_firstRow_flag, scroll_secondRow_flag;
	playlist_t cur_playlist;
	int index;
	char *string;
	int song_index;
	char lcd_clear[] = "                ";
	int i;

	// Initializations
	playlist_init(&cur_playlist);
	cur_song.song_over = FALSE;
	scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
	if (argc > 1)
	{
		for (index = 1; index < argc; index++)
		{
			string = malloc(MAXDATALEN);
			if (string == NULL)
				perror("malloc");
			strcpy(string, argv[index]);
			playlist_add_song(index, string, &cur_playlist);
		}
	}
	else
		return usage(argv[0]);
  	if (wiringPiSetup () == -1)
  	{
	    fprintf(stdout, "oops: %s\n", strerror(errno));
	    return 1;
	}
	for (i = 0; i < 5; i++)
	{
		pinMode(buttonPins[i], INPUT);
		pullUpDnControl(buttonPins[i], PUD_UP);
	}
	lcdHandle = lcdInit(RO, CO, BS, RS, EN, D0, D1, D2, D3, D0, D1, D2, D3);
	if (lcdHandle < 0)
	{
		fprintf(stderr, "%s: lcdInit failed\n", argv[0]);
		return -1;
	}
	song_index = 1;
	cur_song.play_status = PLAY;
	while (cur_song.play_status != QUIT && song_index < argc)
	{
		string = malloc(MAXDATALEN);
		if (string == NULL) perror("malloc");
		playlist_get_song(song_index, &string, &cur_playlist);
		if (string != NULL)
		{
			basec = strdup(string);
			bname = basename(basec);
			strcpy(cur_song.filename, string);
			strcpy(cur_song.base_filename, bname);
			// See if we can get the song info from the file.
			id3_tagger();
			// play the song as a thread
			pthread_create(&song_thread, NULL, (void *) play_song, (void *) &cur_song);
			// The following displays stuff to the LCD without scrolling
			scroll_firstRow_flag = printLcdFirstRow();
			scroll_secondRow_flag = printLcdSecondRow();
			// loop to play the song
			while (cur_song.song_over == FALSE)
			{
				// Following code is to scroll the song info
				if (scroll_firstRow_flag == TRUE)
					scrollMessage_firstRow();
				if (scroll_secondRow_flag == TRUE)
					scrollMessage_secondRow();
				if (digitalRead(playButtonPin) == 0)
				{
					// test again because of switch bouncing
					delay(DeLaY);
					if (digitalRead(playButtonPin) == 0)
					{
						if (cur_song.play_status == PAUSE)
							playMe();
						else
							pauseMe();
					}
				}
				if (digitalRead(prevButtonPin) == 0)
				{
					delay(DeLaY);
					if (digitalRead(prevButtonPin) == 0)
					{
						if (song_index - 1 != 0)
						{
							prevSong();
							song_index--;
						}
					}
				}
				if (digitalRead(nextButtonPin) == 0)
				{
					delay(DeLaY);
					if (digitalRead(nextButtonPin) == 0)
					{
						if (song_index + 1 < argc)
						{
							nextSong();
							song_index++;
						}
					}
				}
				if (digitalRead(infoButtonPin) == 0)
				{
					delay(DeLaY);
					if (digitalRead(infoButtonPin) == 0)
					{
						// toggle what to display
						pthread_mutex_lock(&cur_song.pauseMutex);
						strcpy(cur_song.second_row_text, (strcmp(cur_song.second_row_text, cur_song.artist) == 0 ? cur_song.album : cur_song.artist));
						// first clear just the second row, then re-display the second row
						lcdPosition(lcdHandle, 0, 1);
						lcdPuts(lcdHandle, lcd_clear);
						scroll_secondRow_flag = printLcdSecondRow();
						pthread_mutex_unlock(&cur_song.pauseMutex);
					}
				}
				if (digitalRead(quitButtonPin) == 0)
				{
					delay(DeLaY);
					if (digitalRead(quitButtonPin) == 0)
					{
						quitMe();
					}
				}
			}
			if (pthread_join(song_thread, NULL) != 0)
				perror("join error\n");
			// clear the lcd for next song.
			lcdClear(lcdHandle);
		}
		lcdClear(lcdHandle);
		// reset all the flags.
		scroll_firstRow_flag = scroll_secondRow_flag = FALSE;
		// increment the song_index if the song is over but the next/prev wasn't hit
		if (cur_song.song_over == TRUE && cur_song.play_status == PLAY)
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
			song_index++;
		}
		else if (cur_song.song_over == TRUE && (cur_song.play_status == NEXT || cur_song.play_status == PREV))
		{
			pthread_mutex_lock(&cur_song.pauseMutex);
			// empty out song/artist data
			strcpy(cur_song.title, "");
			strcpy(cur_song.artist, "");
			strcpy(cur_song.album, "");
			cur_song.play_status = PLAY;
			cur_song.song_over = FALSE;
			pthread_mutex_unlock(&cur_song.pauseMutex);
		}
	}
	lcdClear(lcdHandle);
	lcdPosition(lcdHandle, 0, 0);
	lcdPuts(lcdHandle, "Good Bye!");
	delay(1000);
	lcdClear(lcdHandle);
	return 0;
}
