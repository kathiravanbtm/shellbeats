#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>

#define MAX_RESULTS 50
#define MAX_PLAYLISTS 50
#define MAX_PLAYLIST_ITEMS 500
#define IPC_SOCKET "/tmp/shellbeats_mpv.sock"
#define CONFIG_DIR ".shellbeats"
#define PLAYLISTS_DIR "playlists"
#define PLAYLISTS_INDEX "playlists.json"

// ============================================================================
// Data Structures
// ============================================================================

typedef struct {
    char *title;
    char *video_id;
    char *url;
    int duration;
} Song;

typedef struct {
    char *name;
    char *filename;
    Song items[MAX_PLAYLIST_ITEMS];
    int count;
} Playlist;

typedef enum {
    VIEW_SEARCH,
    VIEW_PLAYLISTS,
    VIEW_PLAYLIST_SONGS,
    VIEW_ADD_TO_PLAYLIST
} ViewMode;

typedef struct {
    // Search results
    Song search_results[MAX_RESULTS];
    int search_count;
    int search_selected;
    int search_scroll;
    char query[256];
    
    // Playlists
    Playlist playlists[MAX_PLAYLISTS];
    int playlist_count;
    int playlist_selected;
    int playlist_scroll;
    
    // Current playlist view
    int current_playlist_idx;
    int playlist_song_selected;
    int playlist_song_scroll;
    
    // Playback state
    int playing_index;
    bool playing_from_playlist;
    int playing_playlist_idx;
    bool paused;
    
    // UI state
    ViewMode view;
    int add_to_playlist_selected;
    int add_to_playlist_scroll;
    Song *song_to_add;
    
    // Playback timing (to ignore false end events during loading)
    time_t playback_started;
    
    // Config paths
    char config_dir[1024];
    char playlists_dir[1024];
    char playlists_index[1024];
} AppState;

// ============================================================================
// Globals
// ============================================================================

static pid_t mpv_pid = -1;
static int mpv_ipc_fd = -1;
static volatile sig_atomic_t got_sigchld = 0;

// ============================================================================
// Forward Declarations
// ============================================================================

static void save_playlists_index(AppState *st);
static void save_playlist(AppState *st, int idx);
static void load_playlists(AppState *st);

// ============================================================================
// Utility Functions
// ============================================================================

static char *trim_whitespace(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static bool file_exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0;
}

static bool dir_exists(const char *path) {
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static char *json_escape_string(const char *s) {
    if (!s) return strdup("");
    
    size_t len = strlen(s);
    size_t alloc = len * 2 + 1;
    char *out = malloc(alloc);
    if (!out) return NULL;
    
    size_t j = 0;
    for (size_t i = 0; i < len && j < alloc - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
        } else if (c == '\n') {
            out[j++] = '\\';
            out[j++] = 'n';
            continue;
        } else if (c == '\r') {
            out[j++] = '\\';
            out[j++] = 'r';
            continue;
        } else if (c == '\t') {
            out[j++] = '\\';
            out[j++] = 't';
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    return out;
}

// Simple JSON string extraction (finds "key":"value" and returns value)
static char *json_get_string(const char *json, const char *key) {
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    
    if (*p != '"') return NULL;
    p++;
    
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p++;
        p++;
    }
    
    size_t len = p - start;
    char *result = malloc(len + 1);
    if (!result) return NULL;
    
    // Unescape
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (start[i] == '\\' && i + 1 < len) {
            i++;
            if (start[i] == 'n') result[j++] = '\n';
            else if (start[i] == 'r') result[j++] = '\r';
            else if (start[i] == 't') result[j++] = '\t';
            else result[j++] = start[i];
        } else {
            result[j++] = start[i];
        }
    }
    result[j] = '\0';
    return result;
}

// ============================================================================
// Config Directory Management
// ============================================================================

static bool init_config_dirs(AppState *st) {
    const char *home = getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    
    snprintf(st->config_dir, sizeof(st->config_dir), "%s/%s", home, CONFIG_DIR);
    snprintf(st->playlists_dir, sizeof(st->playlists_dir), "%s/%s", st->config_dir, PLAYLISTS_DIR);
    snprintf(st->playlists_index, sizeof(st->playlists_index), "%s/%s", st->config_dir, PLAYLISTS_INDEX);
    
    st->config_dir[sizeof(st->config_dir) - 1] = '\0';
    st->playlists_dir[sizeof(st->playlists_dir) - 1] = '\0';
    st->playlists_index[sizeof(st->playlists_index) - 1] = '\0';
    
    // Create config directory if not exists
    if (!dir_exists(st->config_dir)) {
        if (mkdir(st->config_dir, 0755) != 0) {
            return false;
        }
    }
    
    // Create playlists directory if not exists
    if (!dir_exists(st->playlists_dir)) {
        if (mkdir(st->playlists_dir, 0755) != 0) {
            return false;
        }
    }
    
    // Create empty playlists index if not exists
    if (!file_exists(st->playlists_index)) {
        FILE *f = fopen(st->playlists_index, "w");
        if (f) {
            fprintf(f, "{\"playlists\":[]}\n");
            fclose(f);
        }
    }
    
    return true;
}

// ============================================================================
// Playlist Persistence
// ============================================================================

static void free_playlist_items(Playlist *pl) {
    for (int i = 0; i < pl->count; i++) {
        free(pl->items[i].title);
        free(pl->items[i].video_id);
        free(pl->items[i].url);
        pl->items[i].title = NULL;
        pl->items[i].video_id = NULL;
        pl->items[i].url = NULL;
    }
    pl->count = 0;
}

static void free_playlist(Playlist *pl) {
    free(pl->name);
    free(pl->filename);
    pl->name = NULL;
    pl->filename = NULL;
    free_playlist_items(pl);
}

static void free_all_playlists(AppState *st) {
    for (int i = 0; i < st->playlist_count; i++) {
        free_playlist(&st->playlists[i]);
    }
    st->playlist_count = 0;
}

static char *sanitize_filename(const char *name) {
    size_t len = strlen(name);
    char *out = malloc(len + 6); // .json + null
    if (!out) return NULL;
    
    size_t j = 0;
    for (size_t i = 0; i < len && j < len; i++) {
        char c = name[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_') {
            out[j++] = tolower((unsigned char)c);
        } else if (c == ' ') {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
    
    strcat(out, ".json");
    return out;
}

static void save_playlists_index(AppState *st) {
    FILE *f = fopen(st->playlists_index, "w");
    if (!f) return;
    
    fprintf(f, "{\n  \"playlists\": [\n");
    
    for (int i = 0; i < st->playlist_count; i++) {
        char *escaped_name = json_escape_string(st->playlists[i].name);
        char *escaped_file = json_escape_string(st->playlists[i].filename);
        
        fprintf(f, "    {\"name\": \"%s\", \"filename\": \"%s\"}%s\n",
                escaped_name ? escaped_name : "",
                escaped_file ? escaped_file : "",
                (i < st->playlist_count - 1) ? "," : "");
        
        free(escaped_name);
        free(escaped_file);
    }
    
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void save_playlist(AppState *st, int idx) {
    if (idx < 0 || idx >= st->playlist_count) return;
    
    Playlist *pl = &st->playlists[idx];
    
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", st->playlists_dir, pl->filename);
    
    FILE *f = fopen(path, "w");
    if (!f) return;
    
    fprintf(f, "{\n  \"name\": \"%s\",\n  \"songs\": [\n", pl->name);
    
    for (int i = 0; i < pl->count; i++) {
        char *escaped_title = json_escape_string(pl->items[i].title);
        char *escaped_id = json_escape_string(pl->items[i].video_id);
        
        fprintf(f, "    {\"title\": \"%s\", \"video_id\": \"%s\"}%s\n",
                escaped_title ? escaped_title : "",
                escaped_id ? escaped_id : "",
                (i < pl->count - 1) ? "," : "");
        
        free(escaped_title);
        free(escaped_id);
    }
    
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void load_playlist_songs(AppState *st, int idx) {
    if (idx < 0 || idx >= st->playlist_count) return;
    
    Playlist *pl = &st->playlists[idx];
    free_playlist_items(pl);
    
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", st->playlists_dir, pl->filename);
    
    FILE *f = fopen(path, "r");
    if (!f) return;
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return;
    }
    
    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return;
    }
    
    size_t read_size = fread(content, 1, fsize, f);
    content[read_size] = '\0';
    fclose(f);
    
    // Parse songs array - simple approach
    const char *p = strstr(content, "\"songs\"");
    if (!p) {
        free(content);
        return;
    }
    
    p = strchr(p, '[');
    if (!p) {
        free(content);
        return;
    }
    
    // Find each song object
    while (pl->count < MAX_PLAYLIST_ITEMS) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end) break;
        
        // Extract this object
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';
        
        char *title = json_get_string(obj, "title");
        char *video_id = json_get_string(obj, "video_id");
        
        if (title && video_id && video_id[0]) {
            pl->items[pl->count].title = title;
            pl->items[pl->count].video_id = video_id;
            
            char url[256];
            snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", video_id);
            pl->items[pl->count].url = strdup(url);
            pl->items[pl->count].duration = 0;
            
            pl->count++;
        } else {
            free(title);
            free(video_id);
        }
        
        free(obj);
        p = obj_end + 1;
    }
    
    free(content);
}

static void load_playlists(AppState *st) {
    free_all_playlists(st);
    
    FILE *f = fopen(st->playlists_index, "r");
    if (!f) return;
    
    // Read entire file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (fsize <= 0 || fsize > 1024 * 1024) {
        fclose(f);
        return;
    }
    
    char *content = malloc(fsize + 1);
    if (!content) {
        fclose(f);
        return;
    }
    
    size_t read_size = fread(content, 1, fsize, f);
    content[read_size] = '\0';
    fclose(f);
    
    // Parse playlists array
    const char *p = strstr(content, "\"playlists\"");
    if (!p) {
        free(content);
        return;
    }
    
    p = strchr(p, '[');
    if (!p) {
        free(content);
        return;
    }
    
    while (st->playlist_count < MAX_PLAYLISTS) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end) break;
        
        size_t obj_len = obj_end - obj_start + 1;
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';
        
        char *name = json_get_string(obj, "name");
        char *filename = json_get_string(obj, "filename");
        
        if (name && filename && name[0] && filename[0]) {
            st->playlists[st->playlist_count].name = name;
            st->playlists[st->playlist_count].filename = filename;
            st->playlists[st->playlist_count].count = 0;
            st->playlist_count++;
        } else {
            free(name);
            free(filename);
        }
        
        free(obj);
        p = obj_end + 1;
    }
    
    free(content);
}

static int create_playlist(AppState *st, const char *name) {
    if (st->playlist_count >= MAX_PLAYLISTS) return -1;
    if (!name || !name[0]) return -1;
    
    // Check for duplicate name
    for (int i = 0; i < st->playlist_count; i++) {
        if (strcasecmp(st->playlists[i].name, name) == 0) {
            return -2; // Already exists
        }
    }
    
    char *filename = sanitize_filename(name);
    if (!filename) return -1;
    
    // Check for duplicate filename
    for (int i = 0; i < st->playlist_count; i++) {
        if (strcmp(st->playlists[i].filename, filename) == 0) {
            // Add number suffix
            char *new_filename = malloc(strlen(filename) + 10);
            if (!new_filename) {
                free(filename);
                return -1;
            }
            snprintf(new_filename, strlen(filename) + 10, "%d_%s", 
                     st->playlist_count, filename);
            free(filename);
            filename = new_filename;
            break;
        }
    }
    
    int idx = st->playlist_count;
    st->playlists[idx].name = strdup(name);
    st->playlists[idx].filename = filename;
    st->playlists[idx].count = 0;
    st->playlist_count++;
    
    save_playlists_index(st);
    save_playlist(st, idx);
    
    return idx;
}

static bool delete_playlist(AppState *st, int idx) {
    if (idx < 0 || idx >= st->playlist_count) return false;
    
    // Delete the file
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", st->playlists_dir, st->playlists[idx].filename);
    unlink(path);
    
    // Free memory
    free_playlist(&st->playlists[idx]);
    
    // Shift remaining playlists
    for (int i = idx; i < st->playlist_count - 1; i++) {
        st->playlists[i] = st->playlists[i + 1];
    }
    st->playlist_count--;
    
    // Clear the last slot
    memset(&st->playlists[st->playlist_count], 0, sizeof(Playlist));
    
    save_playlists_index(st);
    return true;
}

static bool add_song_to_playlist(AppState *st, int playlist_idx, Song *song) {
    if (playlist_idx < 0 || playlist_idx >= st->playlist_count) return false;
    if (!song || !song->video_id) return false;
    
    Playlist *pl = &st->playlists[playlist_idx];
    
    // Load songs if not loaded
    if (pl->count == 0 && file_exists(st->playlists_dir)) {
        load_playlist_songs(st, playlist_idx);
    }
    
    if (pl->count >= MAX_PLAYLIST_ITEMS) return false;
    
    // Check for duplicate
    for (int i = 0; i < pl->count; i++) {
        if (pl->items[i].video_id && strcmp(pl->items[i].video_id, song->video_id) == 0) {
            return false; // Already in playlist
        }
    }
    
    int idx = pl->count;
    pl->items[idx].title = song->title ? strdup(song->title) : strdup("Unknown");
    pl->items[idx].video_id = strdup(song->video_id);
    
    char url[256];
    snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", song->video_id);
    pl->items[idx].url = strdup(url);
    pl->items[idx].duration = song->duration;
    
    pl->count++;
    
    save_playlist(st, playlist_idx);
    return true;
}

static bool remove_song_from_playlist(AppState *st, int playlist_idx, int song_idx) {
    if (playlist_idx < 0 || playlist_idx >= st->playlist_count) return false;
    
    Playlist *pl = &st->playlists[playlist_idx];
    if (song_idx < 0 || song_idx >= pl->count) return false;
    
    // Free song data
    free(pl->items[song_idx].title);
    free(pl->items[song_idx].video_id);
    free(pl->items[song_idx].url);
    
    // Shift remaining songs
    for (int i = song_idx; i < pl->count - 1; i++) {
        pl->items[i] = pl->items[i + 1];
    }
    pl->count--;
    
    // Clear last slot
    memset(&pl->items[pl->count], 0, sizeof(Song));
    
    save_playlist(st, playlist_idx);
    return true;
}

// ============================================================================
// MPV IPC Communication
// ============================================================================

static void mpv_disconnect(void) {
    if (mpv_ipc_fd >= 0) {
        close(mpv_ipc_fd);
        mpv_ipc_fd = -1;
    }
}

static bool mpv_connect(void) {
    if (mpv_ipc_fd >= 0) return true;
    if (!file_exists(IPC_SOCKET)) return false;
    
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    
    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCKET, sizeof(addr.sun_path) - 1);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }
    
    mpv_ipc_fd = fd;
    
    // Enable end-file event observation
    const char *observe_cmd = "{\"command\":[\"observe_property\",1,\"eof-reached\"]}\n";
    ssize_t w = write(mpv_ipc_fd, observe_cmd, strlen(observe_cmd));
    (void)w;
    
    return true;
}

static void mpv_send_command(const char *cmd) {
    if (!mpv_connect()) {
        // Try one-shot connection
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return;
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, IPC_SOCKET, sizeof(addr.sun_path) - 1);
        
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            ssize_t w = write(fd, cmd, strlen(cmd));
            w = write(fd, "\n", 1);
            (void)w;
        }
        close(fd);
        return;
    }
    
    ssize_t w = write(mpv_ipc_fd, cmd, strlen(cmd));
    w = write(mpv_ipc_fd, "\n", 1);
    (void)w;
}

static void mpv_toggle_pause(void) {
    mpv_send_command("{\"command\":[\"cycle\",\"pause\"]}");
}

static void mpv_stop_playback(void) {
    mpv_send_command("{\"command\":[\"stop\"]}");
}

static void mpv_load_url(const char *url) {
    char *escaped = NULL;
    size_t n = 0;
    FILE *mem = open_memstream(&escaped, &n);
    if (!mem) return;
    
    fputc('"', mem);
    for (const char *p = url; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', mem);
        fputc(*p, mem);
    }
    fputc('"', mem);
    fclose(mem);
    
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
             "{\"command\":[\"loadfile\",%s,\"replace\"]}", escaped);
    free(escaped);
    
    mpv_send_command(cmd);
}

static void mpv_start_if_needed(void) {
    if (file_exists(IPC_SOCKET) && mpv_connect()) return;
    
    unlink(IPC_SOCKET);
    mpv_disconnect();
    
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        execlp("mpv", "mpv",
               "--no-video",
               "--idle=yes",
               "--force-window=no",
               "--really-quiet",
               "--input-ipc-server=" IPC_SOCKET,
               (char *)NULL);
        _exit(127);
    }
    
    if (pid > 0) {
        mpv_pid = pid;
        for (int i = 0; i < 100; i++) {
            if (file_exists(IPC_SOCKET)) {
                usleep(50 * 1000);
                mpv_connect();
                break;
            }
            usleep(50 * 1000);
        }
    }
}

static void mpv_quit(void) {
    mpv_send_command("{\"command\":[\"quit\"]}");
    usleep(100 * 1000);
    
    mpv_disconnect();
    
    if (mpv_pid > 0) {
        kill(mpv_pid, SIGTERM);
        waitpid(mpv_pid, NULL, WNOHANG);
        mpv_pid = -1;
    }
    unlink(IPC_SOCKET);
}

// Check if mpv finished playing (returns true if track ended)
// Only returns true for genuine end-of-file, not loading states
static bool mpv_check_track_end(void) {
    if (mpv_ipc_fd < 0) return false;
    
    char buf[4096];
    ssize_t n = read(mpv_ipc_fd, buf, sizeof(buf) - 1);
    
    if (n <= 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // Connection lost
            mpv_disconnect();
        }
        return false;
    }
    
    buf[n] = '\0';
    
    // Only trigger on end-file event with reason "eof" (not "error" or "stop")
    // Format: {"event":"end-file","reason":"eof",...}
    if (strstr(buf, "\"event\":\"end-file\"") && strstr(buf, "\"reason\":\"eof\"")) {
        return true;
    }
    
    return false;
}

// ============================================================================
// Search Functions
// ============================================================================

static void free_search_results(AppState *st) {
    for (int i = 0; i < st->search_count; i++) {
        free(st->search_results[i].title);
        free(st->search_results[i].video_id);
        free(st->search_results[i].url);
        st->search_results[i].title = NULL;
        st->search_results[i].video_id = NULL;
        st->search_results[i].url = NULL;
    }
    st->search_count = 0;
    st->search_selected = 0;
    st->search_scroll = 0;
}

static int run_search(AppState *st, const char *raw_query) {
    free_search_results(st);
    
    char query_buf[256];
    strncpy(query_buf, raw_query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';
    char *query = trim_whitespace(query_buf);
    
    if (!query[0]) return 0;
    
    // Escape for shell
    char escaped_query[512];
    size_t j = 0;
    for (size_t i = 0; query[i] && j < sizeof(escaped_query) - 5; i++) {
        char c = query[i];
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            escaped_query[j++] = '\\';
        }
        escaped_query[j++] = c;
    }
    escaped_query[j] = '\0';
    
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "yt-dlp --flat-playlist "
             "--print '%%(title)s|||%%(id)s' "
             "\"ytsearch%d:%s\" 2>/dev/null",
             MAX_RESULTS, escaped_query);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    
    char *line = NULL;
    size_t cap = 0;
    int count = 0;
    
    while (count < MAX_RESULTS && getline(&line, &cap, fp) != -1) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        if (!line[0]) continue;
        if (strncmp(line, "ERROR", 5) == 0) continue;
        if (strncmp(line, "WARNING", 7) == 0) continue;
        
        char *sep = strstr(line, "|||");
        if (!sep) continue;
        *sep = '\0';
        
        const char *title = line;
        const char *video_id = sep + 3;
        if (!video_id[0]) continue;
        
        size_t id_len = strlen(video_id);
        if (id_len < 5 || id_len > 20) continue;
        
        st->search_results[count].title = strdup(title);
        st->search_results[count].video_id = strdup(video_id);
        
        char fullurl[256];
        snprintf(fullurl, sizeof(fullurl), 
                 "https://www.youtube.com/watch?v=%s", video_id);
        st->search_results[count].url = strdup(fullurl);
        st->search_results[count].duration = 0;
        
        if (st->search_results[count].title && 
            st->search_results[count].video_id &&
            st->search_results[count].url) {
            count++;
        } else {
            free(st->search_results[count].title);
            free(st->search_results[count].video_id);
            free(st->search_results[count].url);
        }
    }
    
    free(line);
    pclose(fp);
    
    st->search_count = count;
    st->search_selected = 0;
    st->search_scroll = 0;
    strncpy(st->query, query, sizeof(st->query) - 1);
    st->query[sizeof(st->query) - 1] = '\0';
    
    return count;
}

// ============================================================================
// Playback Functions
// ============================================================================

static void play_search_result(AppState *st, int idx) {
    if (idx < 0 || idx >= st->search_count) return;
    if (!st->search_results[idx].url) return;
    
    mpv_start_if_needed();
    mpv_load_url(st->search_results[idx].url);
    
    st->playing_index = idx;
    st->playing_from_playlist = false;
    st->playing_playlist_idx = -1;
    st->paused = false;
    st->playback_started = time(NULL);
}

static void play_playlist_song(AppState *st, int playlist_idx, int song_idx) {
    if (playlist_idx < 0 || playlist_idx >= st->playlist_count) return;
    
    Playlist *pl = &st->playlists[playlist_idx];
    if (song_idx < 0 || song_idx >= pl->count) return;
    if (!pl->items[song_idx].url) return;
    
    mpv_start_if_needed();
    mpv_load_url(pl->items[song_idx].url);
    
    st->playing_index = song_idx;
    st->playing_from_playlist = true;
    st->playing_playlist_idx = playlist_idx;
    st->paused = false;
    st->playback_started = time(NULL);
}

static void play_next(AppState *st) {
    if (st->playing_from_playlist && st->playing_playlist_idx >= 0) {
        Playlist *pl = &st->playlists[st->playing_playlist_idx];
        int next = st->playing_index + 1;
        if (next < pl->count) {
            play_playlist_song(st, st->playing_playlist_idx, next);
            st->playlist_song_selected = next;
        }
    } else if (st->search_count > 0) {
        int next = st->playing_index + 1;
        if (next < st->search_count) {
            play_search_result(st, next);
            st->search_selected = next;
        }
    }
}

static void play_prev(AppState *st) {
    if (st->playing_from_playlist && st->playing_playlist_idx >= 0) {
        int prev = st->playing_index - 1;
        if (prev >= 0) {
            play_playlist_song(st, st->playing_playlist_idx, prev);
            st->playlist_song_selected = prev;
        }
    } else if (st->search_count > 0) {
        int prev = st->playing_index - 1;
        if (prev >= 0) {
            play_search_result(st, prev);
            st->search_selected = prev;
        }
    }
}

// ============================================================================
// UI Drawing
// ============================================================================

static void format_duration(int sec, char out[16]) {
    if (sec <= 0) {
        snprintf(out, 16, "--:--");
        return;
    }
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;
    if (h > 0) {
        snprintf(out, 16, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(out, 16, "%02d:%02d", m, s);
    }
}

static void draw_header(int cols, ViewMode view) {
    attron(A_BOLD);
    mvprintw(0, 0, " ShellBeats v0.2 ");
    attroff(A_BOLD);
    
    switch (view) {
        case VIEW_SEARCH:
            printw("| /: search | Enter: play | Space: pause | n/p: next/prev | f: playlists | a: add | q: quit");
            break;
        case VIEW_PLAYLISTS:
            printw("| Enter: open | c: create | x: delete | Esc: back | q: quit");
            break;
        case VIEW_PLAYLIST_SONGS:
            printw("| Enter: play | d: remove song | Esc: back | q: quit");
            break;
        case VIEW_ADD_TO_PLAYLIST:
            printw("| Enter: add to playlist | c: create new | Esc: cancel");
            break;
    }
    
    mvhline(1, 0, ACS_HLINE, cols);
}

static void draw_now_playing(AppState *st, int rows, int cols) {
    mvhline(rows - 2, 0, ACS_HLINE, cols);
    
    const char *title = NULL;
    
    if (st->playing_from_playlist && st->playing_playlist_idx >= 0 &&
        st->playing_playlist_idx < st->playlist_count) {
        Playlist *pl = &st->playlists[st->playing_playlist_idx];
        if (st->playing_index >= 0 && st->playing_index < pl->count) {
            title = pl->items[st->playing_index].title;
        }
    } else if (st->playing_index >= 0 && st->playing_index < st->search_count) {
        title = st->search_results[st->playing_index].title;
    }
    
    if (title) {
        mvprintw(rows - 1, 0, " Now playing: ");
        attron(A_BOLD);
        
        int max_np = cols - 20;
        char npbuf[512];
        strncpy(npbuf, title, sizeof(npbuf) - 1);
        npbuf[sizeof(npbuf) - 1] = '\0';
        if ((int)strlen(npbuf) > max_np && max_np > 3) {
            npbuf[max_np - 3] = '.';
            npbuf[max_np - 2] = '.';
            npbuf[max_np - 1] = '.';
            npbuf[max_np] = '\0';
        }
        printw("%s", npbuf);
        attroff(A_BOLD);
        
        if (st->paused) {
            printw(" [PAUSED]");
        }
    }
}

static void draw_search_view(AppState *st, const char *status, int rows, int cols) {
    mvprintw(2, 0, "Query: ");
    attron(A_BOLD);
    printw("%s", st->query[0] ? st->query : "(none)");
    attroff(A_BOLD);
    
    mvprintw(2, cols - 20, "Results: %d", st->search_count);
    
    if (status && status[0]) {
        mvprintw(3, 0, ">>> %s", status);
    }
    
    mvhline(4, 0, ACS_HLINE, cols);
    
    int list_top = 5;
    int list_height = rows - list_top - 2;
    if (list_height < 1) list_height = 1;
    
    // Adjust scroll
    if (st->search_selected < st->search_scroll) {
        st->search_scroll = st->search_selected;
    } else if (st->search_selected >= st->search_scroll + list_height) {
        st->search_scroll = st->search_selected - list_height + 1;
    }
    
    for (int i = 0; i < list_height && (st->search_scroll + i) < st->search_count; i++) {
        int idx = st->search_scroll + i;
        bool is_selected = (idx == st->search_selected);
        bool is_playing = (!st->playing_from_playlist && idx == st->playing_index);
        
        int y = list_top + i;
        move(y, 0);
        clrtoeol();
        
        char mark = ' ';
        if (is_playing) {
            mark = st->paused ? '|' : '>';
            attron(A_BOLD);
        }
        if (is_selected) {
            attron(A_REVERSE);
        }
        
        char dur[16];
        format_duration(st->search_results[idx].duration, dur);
        
        int max_title = cols - 14;
        if (max_title < 20) max_title = 20;
        
        char titlebuf[1024];
        const char *title = st->search_results[idx].title ? st->search_results[idx].title : "(no title)";
        strncpy(titlebuf, title, sizeof(titlebuf) - 1);
        titlebuf[sizeof(titlebuf) - 1] = '\0';
        
        if ((int)strlen(titlebuf) > max_title && max_title > 3) {
            titlebuf[max_title - 3] = '.';
            titlebuf[max_title - 2] = '.';
            titlebuf[max_title - 1] = '.';
            titlebuf[max_title] = '\0';
        }
        
        mvprintw(y, 0, " %c %3d. [%s] %s", mark, idx + 1, dur, titlebuf);
        
        if (is_selected) {
            attroff(A_REVERSE);
        }
        if (is_playing) {
            attroff(A_BOLD);
        }
    }
}

static void draw_playlists_view(AppState *st, const char *status, int rows, int cols) {
    mvprintw(2, 0, "Playlists");
    mvprintw(2, cols - 20, "Total: %d", st->playlist_count);
    
    if (status && status[0]) {
        mvprintw(3, 0, ">>> %s", status);
    }
    
    mvhline(4, 0, ACS_HLINE, cols);
    
    int list_top = 5;
    int list_height = rows - list_top - 2;
    if (list_height < 1) list_height = 1;
    
    if (st->playlist_count == 0) {
        mvprintw(list_top + 1, 2, "No playlists yet. Press 'c' to create one.");
        return;
    }
    
    // Adjust scroll
    if (st->playlist_selected < st->playlist_scroll) {
        st->playlist_scroll = st->playlist_selected;
    } else if (st->playlist_selected >= st->playlist_scroll + list_height) {
        st->playlist_scroll = st->playlist_selected - list_height + 1;
    }
    
    for (int i = 0; i < list_height && (st->playlist_scroll + i) < st->playlist_count; i++) {
        int idx = st->playlist_scroll + i;
        bool is_selected = (idx == st->playlist_selected);
        
        int y = list_top + i;
        move(y, 0);
        clrtoeol();
        
        if (is_selected) {
            attron(A_REVERSE);
        }
        
        // Load song count if needed
        Playlist *pl = &st->playlists[idx];
        if (pl->count == 0) {
            load_playlist_songs(st, idx);
        }
        
        mvprintw(y, 0, "   %3d. %s (%d songs)", idx + 1, pl->name, pl->count);
        
        if (is_selected) {
            attroff(A_REVERSE);
        }
    }
}

static void draw_playlist_songs_view(AppState *st, const char *status, int rows, int cols) {
    if (st->current_playlist_idx < 0 || st->current_playlist_idx >= st->playlist_count) {
        return;
    }
    
    Playlist *pl = &st->playlists[st->current_playlist_idx];
    
    mvprintw(2, 0, "Playlist: ");
    attron(A_BOLD);
    printw("%s", pl->name);
    attroff(A_BOLD);
    
    mvprintw(2, cols - 20, "Songs: %d", pl->count);
    
    if (status && status[0]) {
        mvprintw(3, 0, ">>> %s", status);
    }
    
    mvhline(4, 0, ACS_HLINE, cols);
    
    int list_top = 5;
    int list_height = rows - list_top - 2;
    if (list_height < 1) list_height = 1;
    
    if (pl->count == 0) {
        mvprintw(list_top + 1, 2, "Playlist is empty. Search for songs and press 'a' to add.");
        return;
    }
    
    // Adjust scroll
    if (st->playlist_song_selected < st->playlist_song_scroll) {
        st->playlist_song_scroll = st->playlist_song_selected;
    } else if (st->playlist_song_selected >= st->playlist_song_scroll + list_height) {
        st->playlist_song_scroll = st->playlist_song_selected - list_height + 1;
    }
    
    for (int i = 0; i < list_height && (st->playlist_song_scroll + i) < pl->count; i++) {
        int idx = st->playlist_song_scroll + i;
        bool is_selected = (idx == st->playlist_song_selected);
        bool is_playing = (st->playing_from_playlist && 
                          st->playing_playlist_idx == st->current_playlist_idx &&
                          st->playing_index == idx);
        
        int y = list_top + i;
        move(y, 0);
        clrtoeol();
        
        char mark = ' ';
        if (is_playing) {
            mark = st->paused ? '|' : '>';
            attron(A_BOLD);
        }
        if (is_selected) {
            attron(A_REVERSE);
        }
        
        char dur[16];
        format_duration(pl->items[idx].duration, dur);
        
        int max_title = cols - 14;
        if (max_title < 20) max_title = 20;
        
        char titlebuf[1024];
        const char *title = pl->items[idx].title ? pl->items[idx].title : "(no title)";
        strncpy(titlebuf, title, sizeof(titlebuf) - 1);
        titlebuf[sizeof(titlebuf) - 1] = '\0';
        
        if ((int)strlen(titlebuf) > max_title && max_title > 3) {
            titlebuf[max_title - 3] = '.';
            titlebuf[max_title - 2] = '.';
            titlebuf[max_title - 1] = '.';
            titlebuf[max_title] = '\0';
        }
        
        mvprintw(y, 0, " %c %3d. [%s] %s", mark, idx + 1, dur, titlebuf);
        
        if (is_selected) {
            attroff(A_REVERSE);
        }
        if (is_playing) {
            attroff(A_BOLD);
        }
    }
}

static void draw_add_to_playlist_view(AppState *st, const char *status, int rows, int cols) {
    mvprintw(2, 0, "Add to playlist: ");
    if (st->song_to_add && st->song_to_add->title) {
        attron(A_BOLD);
        int max_title = cols - 20;
        char titlebuf[256];
        strncpy(titlebuf, st->song_to_add->title, sizeof(titlebuf) - 1);
        titlebuf[sizeof(titlebuf) - 1] = '\0';
        if ((int)strlen(titlebuf) > max_title && max_title > 3) {
            titlebuf[max_title - 3] = '.';
            titlebuf[max_title - 2] = '.';
            titlebuf[max_title - 1] = '.';
            titlebuf[max_title] = '\0';
        }
        printw("%s", titlebuf);
        attroff(A_BOLD);
    }
    
    if (status && status[0]) {
        mvprintw(3, 0, ">>> %s", status);
    }
    
    mvhline(4, 0, ACS_HLINE, cols);
    
    int list_top = 5;
    int list_height = rows - list_top - 2;
    if (list_height < 1) list_height = 1;
    
    if (st->playlist_count == 0) {
        mvprintw(list_top + 1, 2, "No playlists yet. Press 'c' to create one.");
        return;
    }
    
    // Adjust scroll
    if (st->add_to_playlist_selected < st->add_to_playlist_scroll) {
        st->add_to_playlist_scroll = st->add_to_playlist_selected;
    } else if (st->add_to_playlist_selected >= st->add_to_playlist_scroll + list_height) {
        st->add_to_playlist_scroll = st->add_to_playlist_selected - list_height + 1;
    }
    
    for (int i = 0; i < list_height && (st->add_to_playlist_scroll + i) < st->playlist_count; i++) {
        int idx = st->add_to_playlist_scroll + i;
        bool is_selected = (idx == st->add_to_playlist_selected);
        
        int y = list_top + i;
        move(y, 0);
        clrtoeol();
        
        if (is_selected) {
            attron(A_REVERSE);
        }
        
        Playlist *pl = &st->playlists[idx];
        mvprintw(y, 0, "   %3d. %s (%d songs)", idx + 1, pl->name, pl->count);
        
        if (is_selected) {
            attroff(A_REVERSE);
        }
    }
}

static void draw_ui(AppState *st, const char *status) {
    erase();
    
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    
    draw_header(cols, st->view);
    
    switch (st->view) {
        case VIEW_SEARCH:
            draw_search_view(st, status, rows, cols);
            break;
        case VIEW_PLAYLISTS:
            draw_playlists_view(st, status, rows, cols);
            break;
        case VIEW_PLAYLIST_SONGS:
            draw_playlist_songs_view(st, status, rows, cols);
            break;
        case VIEW_ADD_TO_PLAYLIST:
            draw_add_to_playlist_view(st, status, rows, cols);
            break;
    }
    
    draw_now_playing(st, rows, cols);
    
    refresh();
}

// ============================================================================
// Input Handling
// ============================================================================

static int get_string_input(char *buf, size_t bufsz, const char *prompt) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    
    int y = rows - 1;
    move(y, 0);
    clrtoeol();
    
    attron(A_BOLD);
    mvprintw(y, 0, "%s", prompt);
    attroff(A_BOLD);
    refresh();
    
    int prompt_len = strlen(prompt);
    int max_input = cols - prompt_len - 2;
    if (max_input > (int)bufsz - 1) max_input = bufsz - 1;
    if (max_input < 1) max_input = 1;
    
    // Disable timeout for blocking input
    timeout(-1);
    
    echo();
    curs_set(1);
    move(y, prompt_len);
    
    getnstr(buf, max_input);
    
    noecho();
    curs_set(0);
    
    // Re-enable timeout for poll-based event checking
    timeout(100);
    
    char *trimmed = trim_whitespace(buf);
    if (trimmed != buf) {
        memmove(buf, trimmed, strlen(trimmed) + 1);
    }
    
    return strlen(buf);
}

static void show_help(void) {
    erase();
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)cols;
    
    int y = 2;
    attron(A_BOLD);
    mvprintw(y++, 2, "ShellBeats v0.2 | Help");
    attroff(A_BOLD);
    y++;
    
    mvprintw(y++, 4, "GLOBAL CONTROLS:");
    mvprintw(y++, 6, "/           Search YouTube");
    mvprintw(y++, 6, "Enter       Play selected / Open playlist");
    mvprintw(y++, 6, "Space       Pause/Resume playback");
    mvprintw(y++, 6, "n           Next track");
    mvprintw(y++, 6, "p           Previous track");
    mvprintw(y++, 6, "x           Stop playback");
    mvprintw(y++, 6, "Up/Down/j/k Navigate list");
    mvprintw(y++, 6, "PgUp/PgDn   Page up/down");
    mvprintw(y++, 6, "g/G         Go to start/end");
    mvprintw(y++, 6, "h or ?      Show this help");
    mvprintw(y++, 6, "q           Quit");
    y++;
    
    mvprintw(y++, 4, "PLAYLIST CONTROLS:");
    mvprintw(y++, 6, "f           Open playlists menu");
    mvprintw(y++, 6, "a           Add song to playlist");
    mvprintw(y++, 6, "c           Create new playlist");
    mvprintw(y++, 6, "d           Remove song from playlist");
    mvprintw(y++, 6, "x           Delete playlist");
    mvprintw(y++, 6, "Esc         Go back");
    y++;
    
    mvprintw(y++, 4, "Requirements: yt-dlp, mpv");
    
    attron(A_REVERSE);
    mvprintw(rows - 2, 2, " Press any key to continue... ");
    attroff(A_REVERSE);
    
    refresh();
    getch();
}

static bool check_dependencies(char *errmsg, size_t errsz) {
    FILE *fp = popen("which yt-dlp 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        bool found = (fgets(buf, sizeof(buf), fp) != NULL && buf[0] == '/');
        pclose(fp);
        if (!found) {
            snprintf(errmsg, errsz, "yt-dlp not found! Install with: pip install yt-dlp");
            return false;
        }
    }
    
    fp = popen("which mpv 2>/dev/null", "r");
    if (fp) {
        char buf[256];
        bool found = (fgets(buf, sizeof(buf), fp) != NULL && buf[0] == '/');
        pclose(fp);
        if (!found) {
            snprintf(errmsg, errsz, "mpv not found! Install with: apt install mpv");
            return false;
        }
    }
    
    return true;
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    setlocale(LC_ALL, "");
    
    AppState st = {0};
    st.playing_index = -1;
    st.playing_playlist_idx = -1;
    st.current_playlist_idx = -1;
    st.view = VIEW_SEARCH;
    
    // Initialize config directories
    if (!init_config_dirs(&st)) {
        fprintf(stderr, "Failed to initialize config directory\n");
        return 1;
    }
    
    // Load playlists
    load_playlists(&st);
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    
    // Set timeout for non-blocking input (for poll-based event checking)
    timeout(100); // 100ms timeout
    
    char status[512] = "";
    
    if (!check_dependencies(status, sizeof(status))) {
        draw_ui(&st, status);
        timeout(-1);
        getch();
        endwin();
        fprintf(stderr, "%s\n", status);
        return 1;
    }
    
    snprintf(status, sizeof(status), "Press / to search, f for playlists, h for help.");
    draw_ui(&st, status);
    
    bool running = true;
    
    while (running) {
        // Check for track end via mpv IPC
        // Only check if we've been playing for at least 3 seconds (grace period for loading)
        if (st.playing_index >= 0 && mpv_ipc_fd >= 0) {
            time_t now = time(NULL);
            if (now - st.playback_started >= 3) {
                if (mpv_check_track_end()) {
                    // Auto-play next track
                    play_next(&st);
                    if (st.playing_index >= 0) {
                        const char *title = NULL;
                        if (st.playing_from_playlist && st.playing_playlist_idx >= 0) {
                            Playlist *pl = &st.playlists[st.playing_playlist_idx];
                            if (st.playing_index < pl->count) {
                                title = pl->items[st.playing_index].title;
                            }
                        } else if (st.playing_index < st.search_count) {
                            title = st.search_results[st.playing_index].title;
                        }
                        if (title) {
                            snprintf(status, sizeof(status), "Auto-playing: %s", title);
                        }
                    } else {
                        snprintf(status, sizeof(status), "Playback finished");
                    }
                    draw_ui(&st, status);
                }
            } else {
                // During grace period, still drain the socket buffer to avoid stale data
                char drain_buf[4096];
                while (read(mpv_ipc_fd, drain_buf, sizeof(drain_buf)) > 0) {
                    // Discard data during grace period
                }
            }
        }
        
        int ch = getch();
        
        if (ch == ERR) {
            // Timeout - just redraw and continue
            continue;
        }
        
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        (void)cols;
        int list_height = rows - 7;
        if (list_height < 1) list_height = 1;
        
        // Global keys
        switch (ch) {
            case 'q':
                running = false;
                continue;
            
            case ' ':
                if (st.playing_index >= 0 && file_exists(IPC_SOCKET)) {
                    mpv_toggle_pause();
                    st.paused = !st.paused;
                    snprintf(status, sizeof(status), st.paused ? "Paused" : "Playing");
                }
                break;
            
            case 'n':
                if (st.playing_index >= 0) {
                    play_next(&st);
                    snprintf(status, sizeof(status), "Next track");
                }
                break;
            
            case 'p':
                if (st.playing_index >= 0) {
                    play_prev(&st);
                    snprintf(status, sizeof(status), "Previous track");
                }
                break;
            
            case 'h':
            case '?':
                show_help();
                break;
            
            case 27: // Escape
                if (st.view == VIEW_PLAYLISTS) {
                    st.view = VIEW_SEARCH;
                    status[0] = '\0';
                } else if (st.view == VIEW_PLAYLIST_SONGS) {
                    st.view = VIEW_PLAYLISTS;
                    status[0] = '\0';
                } else if (st.view == VIEW_ADD_TO_PLAYLIST) {
                    st.view = VIEW_SEARCH;
                    st.song_to_add = NULL;
                    snprintf(status, sizeof(status), "Cancelled");
                }
                break;
            
            case KEY_RESIZE:
                clear();
                break;
            
            default:
                break;
        }
        
        // View-specific keys
        switch (st.view) {
            case VIEW_SEARCH: {
                switch (ch) {
                    case KEY_UP:
                    case 'k':
                        if (st.search_selected > 0) st.search_selected--;
                        break;
                    
                    case KEY_DOWN:
                    case 'j':
                        if (st.search_selected + 1 < st.search_count) st.search_selected++;
                        break;
                    
                    case KEY_PPAGE:
                        st.search_selected -= list_height;
                        if (st.search_selected < 0) st.search_selected = 0;
                        break;
                    
                    case KEY_NPAGE:
                        st.search_selected += list_height;
                        if (st.search_selected >= st.search_count) 
                            st.search_selected = st.search_count - 1;
                        if (st.search_selected < 0) st.search_selected = 0;
                        break;
                    
                    case KEY_HOME:
                    case 'g':
                        st.search_selected = 0;
                        st.search_scroll = 0;
                        break;
                    
                    case KEY_END:
                        if (st.search_count > 0) {
                            st.search_selected = st.search_count - 1;
                        }
                        break;
                    
                    case '\n':
                    case KEY_ENTER:
                        if (st.search_count > 0) {
                            play_search_result(&st, st.search_selected);
                            snprintf(status, sizeof(status), "Playing: %s",
                                     st.search_results[st.search_selected].title ?
                                     st.search_results[st.search_selected].title : "?");
                        }
                        break;
                    
                    case '/':
                    case 's': {
                        char q[256] = {0};
                        int len = get_string_input(q, sizeof(q), "Search: ");
                        if (len > 0) {
                            snprintf(status, sizeof(status), "Searching: %s ...", q);
                            draw_ui(&st, status);
                            
                            int r = run_search(&st, q);
                            if (r < 0) {
                                snprintf(status, sizeof(status), "Search error!");
                            } else if (r == 0) {
                                snprintf(status, sizeof(status), "No results for: %s", q);
                            } else {
                                snprintf(status, sizeof(status), "Found %d results for: %s", r, q);
                            }
                        } else {
                            snprintf(status, sizeof(status), "Search cancelled");
                        }
                        break;
                    }
                    
                    case 'x':
                        if (st.playing_index >= 0) {
                            mpv_stop_playback();
                            st.playing_index = -1;
                            st.playing_from_playlist = false;
                            st.playing_playlist_idx = -1;
                            st.paused = false;
                            snprintf(status, sizeof(status), "Playback stopped");
                        }
                        break;
                    
                    case 'f':
                        st.view = VIEW_PLAYLISTS;
                        st.playlist_selected = 0;
                        st.playlist_scroll = 0;
                        load_playlists(&st);
                        snprintf(status, sizeof(status), "Playlists");
                        break;
                    
                    case 'a':
                        if (st.search_count > 0) {
                            st.song_to_add = &st.search_results[st.search_selected];
                            st.add_to_playlist_selected = 0;
                            st.add_to_playlist_scroll = 0;
                            st.view = VIEW_ADD_TO_PLAYLIST;
                            snprintf(status, sizeof(status), "Select playlist");
                        } else {
                            snprintf(status, sizeof(status), "No song selected");
                        }
                        break;
                    
                    case 'c': {
                        char name[128] = {0};
                        int len = get_string_input(name, sizeof(name), "New playlist name: ");
                        if (len > 0) {
                            int idx = create_playlist(&st, name);
                            if (idx >= 0) {
                                snprintf(status, sizeof(status), "Created playlist: %s", name);
                            } else if (idx == -2) {
                                snprintf(status, sizeof(status), "Playlist already exists: %s", name);
                            } else {
                                snprintf(status, sizeof(status), "Failed to create playlist");
                            }
                        } else {
                            snprintf(status, sizeof(status), "Cancelled");
                        }
                        break;
                    }
                }
                break;
            }
            
            case VIEW_PLAYLISTS: {
                switch (ch) {
                    case KEY_UP:
                    case 'k':
                        if (st.playlist_selected > 0) st.playlist_selected--;
                        break;
                    
                    case KEY_DOWN:
                    case 'j':
                        if (st.playlist_selected + 1 < st.playlist_count) st.playlist_selected++;
                        break;
                    
                    case KEY_PPAGE:
                        st.playlist_selected -= list_height;
                        if (st.playlist_selected < 0) st.playlist_selected = 0;
                        break;
                    
                    case KEY_NPAGE:
                        st.playlist_selected += list_height;
                        if (st.playlist_selected >= st.playlist_count)
                            st.playlist_selected = st.playlist_count - 1;
                        if (st.playlist_selected < 0) st.playlist_selected = 0;
                        break;
                    
                    case '\n':
                    case KEY_ENTER:
                        if (st.playlist_count > 0) {
                            st.current_playlist_idx = st.playlist_selected;
                            load_playlist_songs(&st, st.current_playlist_idx);
                            st.playlist_song_selected = 0;
                            st.playlist_song_scroll = 0;
                            st.view = VIEW_PLAYLIST_SONGS;
                            snprintf(status, sizeof(status), "Opened: %s",
                                     st.playlists[st.current_playlist_idx].name);
                        }
                        break;
                    
                    case 'c': {
                        char name[128] = {0};
                        int len = get_string_input(name, sizeof(name), "New playlist name: ");
                        if (len > 0) {
                            int idx = create_playlist(&st, name);
                            if (idx >= 0) {
                                snprintf(status, sizeof(status), "Created playlist: %s", name);
                                st.playlist_selected = idx;
                            } else if (idx == -2) {
                                snprintf(status, sizeof(status), "Playlist already exists: %s", name);
                            } else {
                                snprintf(status, sizeof(status), "Failed to create playlist");
                            }
                        } else {
                            snprintf(status, sizeof(status), "Cancelled");
                        }
                        break;
                    }
                    
                    case 'x':
                    case 'd':
                        if (st.playlist_count > 0) {
                            char confirm[8] = {0};
                            char prompt[256];
                            snprintf(prompt, sizeof(prompt), "Delete '%s'? (y/n): ",
                                     st.playlists[st.playlist_selected].name);
                            get_string_input(confirm, sizeof(confirm), prompt);
                            if (confirm[0] == 'y' || confirm[0] == 'Y') {
                                if (delete_playlist(&st, st.playlist_selected)) {
                                    snprintf(status, sizeof(status), "Deleted playlist");
                                    if (st.playlist_selected >= st.playlist_count && st.playlist_count > 0) {
                                        st.playlist_selected = st.playlist_count - 1;
                                    }
                                } else {
                                    snprintf(status, sizeof(status), "Failed to delete");
                                }
                            } else {
                                snprintf(status, sizeof(status), "Cancelled");
                            }
                        }
                        break;
                }
                break;
            }
            
            case VIEW_PLAYLIST_SONGS: {
                Playlist *pl = NULL;
                if (st.current_playlist_idx >= 0 && st.current_playlist_idx < st.playlist_count) {
                    pl = &st.playlists[st.current_playlist_idx];
                }
                
                switch (ch) {
                    case KEY_UP:
                    case 'k':
                        if (st.playlist_song_selected > 0) st.playlist_song_selected--;
                        break;
                    
                    case KEY_DOWN:
                    case 'j':
                        if (pl && st.playlist_song_selected + 1 < pl->count) 
                            st.playlist_song_selected++;
                        break;
                    
                    case KEY_PPAGE:
                        st.playlist_song_selected -= list_height;
                        if (st.playlist_song_selected < 0) st.playlist_song_selected = 0;
                        break;
                    
                    case KEY_NPAGE:
                        if (pl) {
                            st.playlist_song_selected += list_height;
                            if (st.playlist_song_selected >= pl->count)
                                st.playlist_song_selected = pl->count - 1;
                            if (st.playlist_song_selected < 0) st.playlist_song_selected = 0;
                        }
                        break;
                    
                    case '\n':
                    case KEY_ENTER:
                        if (pl && pl->count > 0) {
                            play_playlist_song(&st, st.current_playlist_idx, st.playlist_song_selected);
                            snprintf(status, sizeof(status), "Playing: %s",
                                     pl->items[st.playlist_song_selected].title ?
                                     pl->items[st.playlist_song_selected].title : "?");
                        }
                        break;
                    
                    case 'd':
                        if (pl && pl->count > 0) {
                            const char *title = pl->items[st.playlist_song_selected].title;
                            if (remove_song_from_playlist(&st, st.current_playlist_idx, 
                                                         st.playlist_song_selected)) {
                                snprintf(status, sizeof(status), "Removed: %s", title ? title : "?");
                                if (st.playlist_song_selected >= pl->count && pl->count > 0) {
                                    st.playlist_song_selected = pl->count - 1;
                                }
                            } else {
                                snprintf(status, sizeof(status), "Failed to remove");
                            }
                        }
                        break;
                    
                    case 'x':
                        if (st.playing_index >= 0) {
                            mpv_stop_playback();
                            st.playing_index = -1;
                            st.playing_from_playlist = false;
                            st.playing_playlist_idx = -1;
                            st.paused = false;
                            snprintf(status, sizeof(status), "Playback stopped");
                        }
                        break;
                }
                break;
            }
            
            case VIEW_ADD_TO_PLAYLIST: {
                switch (ch) {
                    case KEY_UP:
                    case 'k':
                        if (st.add_to_playlist_selected > 0) st.add_to_playlist_selected--;
                        break;
                    
                    case KEY_DOWN:
                    case 'j':
                        if (st.add_to_playlist_selected + 1 < st.playlist_count)
                            st.add_to_playlist_selected++;
                        break;
                    
                    case '\n':
                    case KEY_ENTER:
                        if (st.playlist_count > 0 && st.song_to_add) {
                            if (add_song_to_playlist(&st, st.add_to_playlist_selected, st.song_to_add)) {
                                snprintf(status, sizeof(status), "Added to: %s",
                                         st.playlists[st.add_to_playlist_selected].name);
                            } else {
                                snprintf(status, sizeof(status), "Already in playlist or failed");
                            }
                            st.song_to_add = NULL;
                            st.view = VIEW_SEARCH;
                        }
                        break;
                    
                    case 'c': {
                        char name[128] = {0};
                        int len = get_string_input(name, sizeof(name), "New playlist name: ");
                        if (len > 0) {
                            int idx = create_playlist(&st, name);
                            if (idx >= 0) {
                                if (st.song_to_add) {
                                    add_song_to_playlist(&st, idx, st.song_to_add);
                                    snprintf(status, sizeof(status), "Created '%s' and added song", name);
                                    st.song_to_add = NULL;
                                    st.view = VIEW_SEARCH;
                                } else {
                                    snprintf(status, sizeof(status), "Created: %s", name);
                                }
                            } else if (idx == -2) {
                                snprintf(status, sizeof(status), "Playlist already exists: %s", name);
                            } else {
                                snprintf(status, sizeof(status), "Failed to create playlist");
                            }
                        } else {
                            snprintf(status, sizeof(status), "Cancelled");
                        }
                        break;
                    }
                }
                break;
            }
        }
        
        draw_ui(&st, status);
    }
    
    endwin();
    
    // Cleanup
    free_search_results(&st);
    free_all_playlists(&st);
    mpv_quit();
    
    return 0;
}
