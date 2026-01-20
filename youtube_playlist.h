#ifndef YOUTUBE_PLAYLIST_H
#define YOUTUBE_PLAYLIST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *title;
    char *video_id;
    char *url;
    int duration;
} Song;

int fetch_youtube_playlist(const char *url, Song *songs, int max_songs, 
                           char *playlist_title, size_t title_size);

bool validate_youtube_playlist_url(const char *url);

#endif
