/*
 * hdhomerun_scanner.c
 *
 * C-based scanner for HDHomeRun devices that reports to rabbitears.info
 * Converted from scan_tuner.pl with enhancements for ATSC 3.0
 *
 * Copyright (C) 2025 - Mark J. Colombo
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by

 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // For strcasecmp
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h> // For file stat in FTP upload
#include <stdbool.h>
#include <stdint.h>
#include <getopt.h>
#include <bzlib.h>    // For bzip2 compression

// NEW: Headers for socket programming
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

#include "hdhomerun.h"
#include "hdhomerun_device.h"

#define MAX_CHANNELS 256
#define MAX_CONFIG_LINE 512
#define MAX_FILENAME 256
#define DEFAULT_CONFIG_FILE "scanner.conf"
#define DEFAULT_SPOT_URL "http://www.rabbitears.info/tvdx/raw_spot"
#define DEFAULT_MIN_CHANNEL 2U
#define DEFAULT_MAX_CHANNEL 69U
#define DEFAULT_TUNER_ID "FFFFFFFF"
#define DEFAULT_TUNER_INDEX 0
#define DEFAULT_SEEK_TIME 3.0
#define DEFAULT_RETRY_DELAY 10

// Configuration structure
struct scanner_config {
    char tuner_id[16];
    /* FIX: actual device ID discovered at connect time when tuner_id is the
     * FFFFFFFF wildcard. Used for user_id only (as the Perl did); tuning
     * still uses tuner_id so a swapped device is picked up next loop. */
    char resolved_tuner_id[16];
    int tuner_index;
    unsigned int min_channel;
    unsigned int max_channel;
    char spot_url[512];
    char user_agent[256];
    double seek_timeout;
    int psip_wait_time;  // Time to wait for PSIP data after lock (ATSC 1.0)
    int atsc3_psip_wait_time;  // Max time to wait for SLT data on ATSC 3.0
    int retry_delay;
    bool debug;
    bool use_channel_map;
    char override_callsign[16];
    char override_location[64];
    // Per-channel callsign overrides (Perl scan_tuner.pl -o equivalent).
    // Independent of override_callsign above; applied per RF channel match.
    struct {
        unsigned int channel;
        char callsign[16];
    } channel_overrides[64];
    int num_channel_overrides;
};

// Channel scan result structure
struct channel_result {
    unsigned int rf_channel;
    unsigned int frequency;
    char modulation[32];
    int signal_strength;
    int signal_quality;
    int symbol_error_quality;
    long tsid;
    long bsid;  // For ATSC 3.0
    bool is_atsc3;
    json_object *virtual_channels;
    json_object *plp_info;  // For ATSC 3.0 PLP details
    char l1detail[1024]; // To store l1detail string for ATSC 3.0
    char reporter_callsign[16];
};

// HTTP response structure for curl
struct http_response {
    char *data;
    size_t size;
};

// NEW: Structure for server-requested captures
struct capture_request {
    unsigned int channel;
    int duration;
    int attempts;
};

#define MAX_CAPTURE_REQUESTS 16
static struct capture_request pending_captures[MAX_CAPTURE_REQUESTS];
static int pending_capture_count = 0;


// Function prototypes
void load_config(const char *config_file, struct scanner_config *config);
void print_usage(const char *program_name);
long parse_status_value(const char *status_str, const char *key);
long parse_db_value(const char *status_str, const char *key);
void sanitize_string(char *str);
json_object *parse_plp_string_to_json(const char *plp_line);
bool seek_and_lock_channel(struct hdhomerun_device_t *hd, unsigned int channel, 
                          double timeout, int psip_wait, bool debug);
bool collect_channel_info(struct hdhomerun_device_t *hd, struct channel_result *result, 
                         struct scanner_config *config, bool debug);
bool attempt_atsc3_all_plps(struct hdhomerun_device_t *hd, struct channel_result *result,
                           struct scanner_config *config, bool debug);
json_object *create_scan_json(struct scanner_config *config, 
                             struct channel_result *results, int result_count);
size_t write_callback(void *contents, size_t size, size_t nmemb, struct http_response *response);
struct http_response submit_results(const char *url, json_object *scan_data, bool debug);
void signal_handler(int sig);

// NEW: Function prototypes for capture and FTP
void parse_capture_requests(const char *json_str, bool debug);
bool record_channel_capture(struct hdhomerun_device_t *hd, const struct scanner_config *config, unsigned int channel, int duration, char *filename, bool debug);
bool upload_file_ftp(const char *filename, bool debug);
static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *stream);


// Global flag for graceful shutdown
volatile sig_atomic_t keep_running = 1;

/* FIX: network timeout settings (seconds) */
#define HTTP_CONNECT_TIMEOUT   30L
#define HTTP_TOTAL_TIMEOUT    300L   /* hard ceiling for the scan POST */
#define NET_LOW_SPEED_TIME    180L   /* < 1 byte/s for this long = stalled (LWP default) */
#define FTP_CONNECT_TIMEOUT    30L
#define FTP_RESPONSE_TIMEOUT   60L
#define MIN_CAPTURE_DURATION    5
#define MAX_CAPTURE_DURATION  120

/*
 * FIX: progress callback that lets SIGINT/SIGTERM abort an in-flight
 * transfer. musl's signal() uses SA_RESTART, so without this a signal
 * would not interrupt a blocked curl_easy_perform().
 */
static int curl_abort_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                               curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return keep_running ? 0 : 1;   /* non-zero aborts the transfer */
}

/*
 * FIX: apply common timeout/keepalive options to a curl handle.
 * total_timeout of 0 means no hard ceiling (low-speed limit still applies).
 */
static void apply_curl_timeouts(CURL *curl, long connect_timeout, long total_timeout) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, NET_LOW_SPEED_TIME);
    if (total_timeout > 0) {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_timeout);
    }
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_abort_callback);
}

/*
 * attempt_atsc3_all_plps - Try to tune ATSC 3.0 signal with multiple PLPs.
 *
 * Strategy:
 *   1. Build a list of all available PLPs (excluding PLP 16, the L1 detail PLP).
 *   2. Try to tune to all of them at once.
 *   3. After a brief settling period, check which PLPs actually locked.
 *   4. If some failed to lock, retune with only the ones that did.
 *      That way, a marginal PLP doesn't prevent us from getting service
 *      info from the PLPs that *are* lockable.
 *   5. Re-parse streaminfo and rebuild virtual_channels — but only if
 *      the new streaminfo actually contains program lines. If it doesn't
 *      (weak signal, no data even from the PLPs we kept), preserve whatever
 *      virtual_channels we had from the original PLP0-only lock.
 */
bool attempt_atsc3_all_plps(struct hdhomerun_device_t *hd, struct channel_result *result,
                           struct scanner_config *config, bool debug) {
    if (!result->is_atsc3) return false;

    /* ---- Step 1: enumerate available PLPs (skip PLP 16) ---- */
    char *plpinfo;
    if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) <= 0) {
        if (debug) printf("No PLP info available for ATSC 3.0 channel %u\n", result->rf_channel);
        return false;
    }

    /* Collect PLP IDs into an array so we can rebuild tune strings later */
    int candidate_plps[32];
    int candidate_count = 0;
    {
        char *copy = strdup(plpinfo);
        if (copy) {
            char *line = strtok(copy, "\n");
            while (line != NULL && candidate_count < 32) {
                if (strncmp(line, "bsid=", 5) != 0) {
                    int plp_id;
                    if (sscanf(line, "%d:", &plp_id) == 1 && plp_id != 16) {
                        candidate_plps[candidate_count++] = plp_id;
                    }
                }
                line = strtok(NULL, "\n");
            }
            free(copy);
        }
    }

    if (candidate_count == 0) {
        if (debug) printf("No suitable PLPs found for ATSC 3.0 channel %u\n", result->rf_channel);
        return false;
    }

    /* Build initial tune string: "0+1+3+..." */
    char plp_str[128] = {0};
    for (int i = 0; i < candidate_count; i++) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%d", (i == 0) ? "" : "+", candidate_plps[i]);
        if (strlen(plp_str) + strlen(tmp) >= sizeof(plp_str)) break;
        strcat(plp_str, tmp);
    }

    if (debug) printf("Tuning ATSC 3.0 channel %u with all PLPs: %s\n", result->rf_channel, plp_str);

    /* ---- Step 2: tune to all candidate PLPs ---- */
    char tune_str[256];
    snprintf(tune_str, sizeof(tune_str), "atsc3:%u:%s", result->rf_channel, plp_str);

    if (hdhomerun_device_set_tuner_channel(hd, tune_str) < 0) {
        if (debug) printf("Failed to retune channel %u with PLPs\n", result->rf_channel);
        return false;
    }

    struct hdhomerun_tuner_status_t lock_status;
    if (hdhomerun_device_wait_for_lock(hd, &lock_status) <= 0) {
        if (debug) printf("Failed to lock ATSC 3.0 channel %u with PLPs\n", result->rf_channel);
        return false;
    }

    /* Wait briefly for PLPs to settle and report lock state.
     * We don't need the full atsc3_psip_wait_time here — we just need
     * the lock=1/lock=0 status, which is reported quickly. */
    sleep(2);

    /* ---- Step 3: check which PLPs actually locked ---- */
    int locked_plps[32];
    int locked_count = 0;
    {
        char *fresh_plpinfo;
        if (hdhomerun_device_get_tuner_plpinfo(hd, &fresh_plpinfo) > 0) {
            char *copy = strdup(fresh_plpinfo);
            if (copy) {
                char *line = strtok(copy, "\n");
                while (line != NULL && locked_count < 32) {
                    if (strncmp(line, "bsid=", 5) != 0) {
                        int plp_id;
                        if (sscanf(line, "%d:", &plp_id) == 1 &&
                            plp_id != 16 &&
                            strstr(line, "lock=1") != NULL) {
                            locked_plps[locked_count++] = plp_id;
                        }
                    }
                    line = strtok(NULL, "\n");
                }
                free(copy);
            }
        }
    }

    if (locked_count == 0) {
        if (debug) printf("No PLPs locked on channel %u after retune\n", result->rf_channel);
        return false;
    }

    /* ---- Step 4: if not all PLPs locked, retune with only the locked ones ----
     * But only if more than one PLP locked. If just one PLP locked, retuning to
     * that single PLP gives no new information vs. what we have now (and indeed
     * is a no-op from the device's perspective if it dropped the unlockable PLPs
     * already). Skip the retune in that case. */
    if (locked_count < candidate_count && locked_count > 1) {
        char locked_str[128] = {0};
        for (int i = 0; i < locked_count; i++) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%s%d", (i == 0) ? "" : "+", locked_plps[i]);
            if (strlen(locked_str) + strlen(tmp) >= sizeof(locked_str)) break;
            strcat(locked_str, tmp);
        }

        if (debug) {
            printf("Only %d of %d PLPs locked; retuning with locked subset: %s\n",
                   locked_count, candidate_count, locked_str);
        }

        snprintf(tune_str, sizeof(tune_str), "atsc3:%u:%s", result->rf_channel, locked_str);
        if (hdhomerun_device_set_tuner_channel(hd, tune_str) < 0) {
            if (debug) printf("Failed to retune channel %u with locked PLP subset\n", result->rf_channel);
            /* Don't bail — we may still have usable streaminfo from the previous tune */
        } else {
            if (hdhomerun_device_wait_for_lock(hd, &lock_status) <= 0) {
                if (debug) printf("Failed to lock channel %u with locked PLP subset\n", result->rf_channel);
            }
            /* Poll streaminfo for program data, up to atsc3_psip_wait_time seconds. */
            int max_wait = config->atsc3_psip_wait_time;
            int waited = 0;
            bool got_progs = false;
            while (waited < max_wait) {
                sleep(1);
                waited++;
                char *si = NULL;
                if (hdhomerun_device_get_tuner_streaminfo(hd, &si) > 0 && si) {
                    bool has_progs = false;
                    char *si_copy = strdup(si);
                    if (si_copy) {
                        char *l = strtok(si_copy, "\n");
                        while (l) {
                            while (isspace((unsigned char)*l)) l++;
                            if (strncmp(l, "program ", 8) == 0 ||
                                strncmp(l, "program=", 8) == 0 ||
                                (isdigit((unsigned char)*l) && strchr(l, ':'))) {
                                has_progs = true;
                                break;
                            }
                            l = strtok(NULL, "\n");
                        }
                        free(si_copy);
                    }
                    if (has_progs) {
                        if (debug) printf("Locked-subset program data appeared after %ds\n", waited);
                        got_progs = true;
                        break;
                    }
                }
            }
            if (debug && !got_progs) {
                printf("Locked-subset retune timed out after %ds\n", waited);
            }
        }
    } else if (locked_count == 1 && candidate_count > 1) {
        if (debug) printf("Only 1 of %d PLPs locked; no retune needed (already minimal)\n", candidate_count);
    } else if (debug) {
        printf("All %d PLPs locked successfully\n", locked_count);
    }

    /* ---- Step 5: re-collect streaminfo and rebuild virtual_channels ---- */
    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) <= 0) {
        if (debug) printf("Failed to get streaminfo after retune on channel %u\n", result->rf_channel);
        return false;
    }

    if (debug) {
        printf("Streaminfo after PLP retune:\n%s\n", streaminfo);
    }

    /* First scan the new streaminfo to see if it has any program data.
     * If it doesn't (e.g. "none\n"), keep whatever virtual_channels we
     * already had from the initial PLP0-only lock — don't clobber good
     * data with empty data.
     */
    bool retune_has_programs = false;
    {
        char *probe = strdup(streaminfo);
        if (probe) {
            char *l = strtok(probe, "\n");
            while (l != NULL) {
                while (isspace((unsigned char)*l)) l++;
                if (strncmp(l, "program ", 8) == 0 ||
                    strncmp(l, "program=", 8) == 0 ||
                    (isdigit((unsigned char)*l) && strchr(l, ':'))) {
                    retune_has_programs = true;
                    break;
                }
                l = strtok(NULL, "\n");
            }
            free(probe);
        }
    }

    if (retune_has_programs) {
        /* Replace virtual_channels with what we found in the new streaminfo */
        json_object_put(result->virtual_channels);
        result->virtual_channels = json_object_new_object();

        char *streaminfo_copy = strdup(streaminfo);
        if (streaminfo_copy) {
            char *line = strtok(streaminfo_copy, "\n");
            while (line != NULL) {
                while (isspace((unsigned char)*line)) line++;
                if (strlen(line) == 0) {
                    line = strtok(NULL, "\n");
                    continue;
                }

                if (debug) printf("Processing line: '%s'\n", line);

                if (strncmp(line, "program ", 8) == 0 ||
                    (isdigit((unsigned char)*line) && strchr(line, ':'))) {

                    int prog_num = 0;
                    char vchan[32] = {0}, name[128] = {0};
                    int parsed = 0;

                    char *colon_pos = strchr(line, ':');
                    if (colon_pos) {
                        if (strncmp(line, "program ", 8) == 0) {
                            sscanf(line, "program %d:", &prog_num);
                        } else {
                            sscanf(line, "%d:", &prog_num);
                        }

                        char *vchan_start = colon_pos + 1;
                        while (isspace((unsigned char)*vchan_start)) vchan_start++;

                        int vchan_len = 0;
                        while (vchan_start[vchan_len] && !isspace((unsigned char)vchan_start[vchan_len]) && vchan_len < 31) {
                            vchan[vchan_len] = vchan_start[vchan_len];
                            vchan_len++;
                        }
                        vchan[vchan_len] = '\0';

                        char *name_start = vchan_start + vchan_len;
                        while (isspace((unsigned char)*name_start)) name_start++;

                        if (strlen(name_start) > 0) {
                            strncpy(name, name_start, sizeof(name) - 1);
                            name[sizeof(name) - 1] = '\0';
                            char *end = name + strlen(name) - 1;
                            while (end > name && isspace((unsigned char)*end)) *end-- = '\0';
                        }
                        sanitize_string(name);

                        if (prog_num > 0 && strlen(vchan) > 0) {
                            parsed = 1;
                        }
                    }

                    if (parsed && strcmp(vchan, "0") != 0) {
                        json_object *prog_obj = json_object_new_object();
                        json_object *chan_obj = json_object_new_string(vchan);
                        json_object *name_obj = json_object_new_string(strlen(name) > 0 ? name : "");
                        json_object_object_add(prog_obj, "channel", chan_obj);
                        json_object_object_add(prog_obj, "name", name_obj);

                        char prog_key[16];
                        snprintf(prog_key, sizeof(prog_key), "%d", prog_num);
                        json_object_object_add(result->virtual_channels, prog_key, prog_obj);

                        if (debug) {
                            printf("ATSC 3.0 Re-scan Program %d: %s %s\n", prog_num, vchan, name);
                        }
                    }
                }
                else if (strncmp(line, "program=", 8) == 0) {
                    if (debug) printf("Found program= line: %s\n", line);
                    int prog_num = 0, plp_id = 0;
                    char name[128] = "";

                    if (sscanf(line, "program=%d plp=%d", &prog_num, &plp_id) >= 2) {
                        char *plp_pattern = " plp=";
                        char *plp_pos = strstr(line, plp_pattern);
                        if (plp_pos) {
                            char *name_start = strchr(plp_pos + strlen(plp_pattern), ' ');
                            if (name_start) {
                                name_start++;
                                while (isspace((unsigned char)*name_start)) name_start++;
                                if (strlen(name_start) > 0) {
                                    strncpy(name, name_start, sizeof(name) - 1);
                                    name[sizeof(name) - 1] = '\0';
                                    char *end = name + strlen(name) - 1;
                                    while (end > name && isspace((unsigned char)*end)) *end-- = '\0';
                                }
                            }
                        }
                        sanitize_string(name);

                        json_object *prog_obj = json_object_new_object();
                        json_object *plp_obj = json_object_new_int(plp_id);
                        json_object *name_obj = json_object_new_string(strlen(name) > 0 ? name : "");
                        json_object_object_add(prog_obj, "plp", plp_obj);
                        json_object_object_add(prog_obj, "name", name_obj);

                        char prog_key[16];
                        snprintf(prog_key, sizeof(prog_key), "%d", prog_num);
                        json_object_object_add(result->virtual_channels, prog_key, prog_obj);

                        if (debug) {
                            printf("ATSC 3.0 Program %d (PLP %d): %s\n", prog_num, plp_id, name);
                        }
                    }
                }
                line = strtok(NULL, "\n");
            }
            free(streaminfo_copy);
        }
    } else if (debug) {
        printf("All-PLP retune produced no program data; keeping original virtual_channels.\n");
    }

    /* ---- Refresh plp_info and l1detail regardless of whether we updated virtual_channels ---- */
    /* Get l1detail string */
    char var_name[64];
    snprintf(var_name, sizeof(var_name), "/tuner%d/l1detail", config->tuner_index);
    char *l1detail_str = NULL;
    char *error_str = NULL;
    if (hdhomerun_device_get_var(hd, var_name, &l1detail_str, &error_str) > 0 && l1detail_str) {
        strncpy(result->l1detail, l1detail_str, sizeof(result->l1detail) - 1);
        result->l1detail[sizeof(result->l1detail) - 1] = '\0';
        if (debug) printf("Re-fetched l1detail string.\n");
    }

    /* Re-fetch plp_info */
    char *new_plpinfo;
    if (hdhomerun_device_get_tuner_plpinfo(hd, &new_plpinfo) > 0) {
        if (result->plp_info) {
            json_object_put(result->plp_info);
        }
        result->plp_info = json_object_new_object();
        char *new_plpinfo_copy = strdup(new_plpinfo);
        if (new_plpinfo_copy) {
            char *line = strtok(new_plpinfo_copy, "\n");
            while (line != NULL) {
                if (strncmp(line, "bsid=", 5) != 0) {
                    int plp_id;
                    if (sscanf(line, "%d:", &plp_id) == 1) {
                        json_object *plp_obj = parse_plp_string_to_json(line);
                        char plp_key[16];
                        snprintf(plp_key, sizeof(plp_key), "%d", plp_id);
                        json_object_object_add(result->plp_info, plp_key, plp_obj);
                    }
                }
                line = strtok(NULL, "\n");
            }
            free(new_plpinfo_copy);
        }
        if (debug) printf("Re-fetched plp_info.\n");
    }

    return true;
}

/*
 * parse_status_value - Borrowed from hdhomerun_tui.c
 */
long parse_status_value(const char *status_str, const char *key) {
    const char *found = strstr(status_str, key);
    if (found) {
        return strtol(found + strlen(key), NULL, 0);
    }
    return -999;
}

/*
 * parse_db_value - Borrowed from hdhomerun_tui.c
 */
long parse_db_value(const char *status_str, const char *key) {
    const char *key_found = strstr(status_str, key);
    if (key_found) {
        const char *paren_open = strchr(key_found, '(');
        if (paren_open) {
            return strtol(paren_open + 1, NULL, 10);
        }
    }
    return -999;
}

/*
 * NEW: sanitize_string
 * Iterates through a string and replaces any non-printable or non-ASCII
 * characters with a '?' to prevent malformed UTF-8 errors in JSON.
 */
void sanitize_string(char *str) {
    if (!str) return;
    for (char *p = str; *p; p++) {
        // Replace any character that is not a standard printable ASCII character (space to ~)
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126) {
            *p = '?';
        }
    }
}

/*
 * NEW: parse_plp_string_to_json
 * Parses a raw PLP info string into a structured JSON object.
 */
json_object *parse_plp_string_to_json(const char *plp_line) {
    json_object *plp_obj = json_object_new_object();
    if (!plp_line) return plp_obj;

    char *line_copy = strdup(plp_line);
    if (!line_copy) return plp_obj;

    // Find the data part after "XX: "
    char *data_part = strchr(line_copy, ':');
    if (data_part) {
        data_part++; // Move past the colon
        while (*data_part == ' ') data_part++; // Skip spaces

        // Tokenize by space to get "key=value" pairs
        char *token = strtok(data_part, " ");
        while (token) {
            char *key = token;
            char *value = strchr(token, '=');

            if (value) {
                *value = '\0'; // Null-terminate the key
                value++;       // Move to the start of the value

                if (strcmp(key, "sfi") == 0 || strcmp(key, "lls") == 0 || strcmp(key, "lock") == 0) {
                    json_object_object_add(plp_obj, key, json_object_new_int(atoi(value)));
                } else {
                    json_object_object_add(plp_obj, key, json_object_new_string(value));
                }
            }
            token = strtok(NULL, " ");
        }
    }

    free(line_copy);
    return plp_obj;
}

/*
 * load_config - Load configuration from file
 */
void load_config(const char *config_file, struct scanner_config *config) {
    /* FIX: zero the struct so strncpy(..., size-1) of a full-length value
     * is always NUL-terminated. */
    memset(config, 0, sizeof(*config));

    // Set defaults
    strcpy(config->tuner_id, DEFAULT_TUNER_ID);
    config->tuner_index = DEFAULT_TUNER_INDEX;
    config->min_channel = DEFAULT_MIN_CHANNEL;
    config->max_channel = DEFAULT_MAX_CHANNEL;
    strcpy(config->spot_url, DEFAULT_SPOT_URL);
    strcpy(config->user_agent, "HDHomeRun Scanner C/1.0");
    config->seek_timeout = DEFAULT_SEEK_TIME;
    config->psip_wait_time = 2;  // Default 2 seconds for PSIP collection
    config->atsc3_psip_wait_time = 10;  // Up to 10 seconds for ATSC 3.0 SLT
    config->retry_delay = DEFAULT_RETRY_DELAY;
    config->debug = false;
    config->use_channel_map = false;
    config->override_callsign[0] = '\0';
    config->override_location[0] = '\0';
    config->num_channel_overrides = 0;
    
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        if (config->debug) {
            printf("Config file %s not found, using defaults\n", config_file);
        }
        return;
    }
    
    char line[MAX_CONFIG_LINE];
    while (fgets(line, sizeof(line), fp)) {
        // Remove comments and whitespace
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        
        // Trim whitespace
        char *start = line;
        while (isspace(*start)) start++;
        char *end = start + strlen(start) - 1;
        while (end > start && isspace(*end)) end--;
        *(end + 1) = '\0';
        
        if (strlen(start) == 0) continue;
        
        // Parse key=value pairs
        char *equals = strchr(start, '=');
        if (!equals) continue;
        
        *equals = '\0';
        char *key = start;
        char *value = equals + 1;
        
        // Trim key and value
        end = key + strlen(key) - 1;
        while (end > key && isspace(*end)) end--;
        *(end + 1) = '\0';
        
        while (isspace(*value)) value++;
        
        // Process configuration options
        if (strcmp(key, "tuner_id") == 0) {
            strncpy(config->tuner_id, value, sizeof(config->tuner_id) - 1);
        } else if (strcmp(key, "tuner_index") == 0) {
            config->tuner_index = atoi(value);
        } else if (strcmp(key, "min_channel") == 0) {
            config->min_channel = (unsigned int)atoi(value);
        } else if (strcmp(key, "max_channel") == 0) {
            config->max_channel = (unsigned int)atoi(value);
        } else if (strcmp(key, "spot_url") == 0) {
            strncpy(config->spot_url, value, sizeof(config->spot_url) - 1);
        } else if (strcmp(key, "user_agent") == 0) {
            strncpy(config->user_agent, value, sizeof(config->user_agent) - 1);
        } else if (strcmp(key, "seek_timeout") == 0) {
            config->seek_timeout = atof(value);
        } else if (strcmp(key, "psip_wait_time") == 0) {
            config->psip_wait_time = atoi(value);
        } else if (strcmp(key, "atsc3_psip_wait_time") == 0) {
            config->atsc3_psip_wait_time = atoi(value);
        } else if (strcmp(key, "retry_delay") == 0) {
            config->retry_delay = atoi(value);
        } else if (strcmp(key, "debug") == 0) {
            config->debug = (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "use_channel_map") == 0) {
            config->use_channel_map = (strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0);
        } else if (strcmp(key, "override_callsign") == 0) {
            strncpy(config->override_callsign, value, sizeof(config->override_callsign) - 1);
        } else if (strcmp(key, "override_location") == 0) {
            strncpy(config->override_location, value, sizeof(config->override_location) - 1);
        } else if (strcmp(key, "override_channel") == 0) {
            // Format: override_channel=NN,CALLSIGN
            // Repeatable; each line adds one entry.
            if (config->num_channel_overrides <
                (int)(sizeof(config->channel_overrides) / sizeof(config->channel_overrides[0]))) {
                char *comma = strchr(value, ',');
                if (comma) {
                    *comma = '\0';
                    char *ch_str = value;
                    char *call = comma + 1;
                    // Trim spaces around channel number and callsign
                    while (isspace(*ch_str)) ch_str++;
                    while (isspace(*call)) call++;
                    char *cend = ch_str + strlen(ch_str) - 1;
                    while (cend > ch_str && isspace(*cend)) { *cend = '\0'; cend--; }
                    char *kend = call + strlen(call) - 1;
                    while (kend > call && isspace(*kend)) { *kend = '\0'; kend--; }
                    int ch_num = atoi(ch_str);
                    if (ch_num >= 2 && ch_num <= 69 && *call != '\0') {
                        int idx = config->num_channel_overrides;
                        config->channel_overrides[idx].channel = (unsigned int)ch_num;
                        strncpy(config->channel_overrides[idx].callsign, call,
                                sizeof(config->channel_overrides[idx].callsign) - 1);
                        config->channel_overrides[idx].callsign[
                            sizeof(config->channel_overrides[idx].callsign) - 1] = '\0';
                        config->num_channel_overrides++;
                    }
                }
            }
        }
    }
    
    fclose(fp);
}

/*
 * collect_channel_info - Collect all information about the current channel
 * Fixed to properly wait for PSIP and use consistent TSID reporting
 */
bool collect_channel_info(struct hdhomerun_device_t *hd, struct channel_result *result, 
                         struct scanner_config *config, bool debug) {
    struct hdhomerun_tuner_status_t status;
    char *raw_status_str;
    
    if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) <= 0) {
        return false;
    }
    
    // Parse channel number from status
    char *p = strchr(status.channel, ':');
    if (!p) p = status.channel; else p++;
    if (isdigit((unsigned char)*p)) {
        result->rf_channel = strtoul(p, NULL, 10);
    }
    
    // Copy modulation and signal info
    strncpy(result->modulation, status.lock_str, sizeof(result->modulation) - 1);
    result->modulation[sizeof(result->modulation) - 1] = '\0';
    result->signal_strength = status.signal_strength;
    result->signal_quality = status.signal_to_noise_quality;
    result->symbol_error_quality = status.symbol_error_quality;
    
    // Check if this is ATSC 3.0
    result->is_atsc3 = (strstr(status.lock_str, "atsc3") != NULL);
    result->l1detail[0] = '\0'; // Initialize l1detail string
    
    // Initialize TSID/BSID - we'll always use tsid field for server compatibility
    result->tsid = -999;
    result->bsid = -999;
    
    // Get TSID from streaminfo first (works for both ATSC 1.0 and 3.0)
    char *streaminfo;
    if (hdhomerun_device_get_tuner_streaminfo(hd, &streaminfo) > 0) {
        result->tsid = parse_status_value(streaminfo, "tsid=");
        if (debug && result->tsid != -999) {
            printf("Found TSID: %ld (0x%lx)\n", result->tsid, result->tsid);
        }
        
        // Parse virtual channels from streaminfo with better error handling
        if (result->virtual_channels) {
            json_object_put(result->virtual_channels);
        }
        result->virtual_channels = json_object_new_object();
        char *streaminfo_copy = strdup(streaminfo);
        if (streaminfo_copy) {
            char *line = strtok(streaminfo_copy, "\n");
            while (line != NULL) {
                // Skip empty lines and trim whitespace
                while (isspace(*line)) line++;
                if (strlen(line) == 0) {
                    line = strtok(NULL, "\n");
                    continue;
                }
                
                // Look for ATSC 1.0 program lines: "program 1: 13.1 WNYTTV" or "1: 13.1 WNYTTV"
                if (strncmp(line, "program ", 8) == 0 || 
                    (isdigit((unsigned char)*line) && strchr(line, ':'))) {
                    
                    int prog_num = 0;
                    char vchan[32] = {0}, name[128] = {0};
                    int parsed = 0;
                    
                    // Parse program number and virtual channel first
                    char *colon_pos = strchr(line, ':');
                    if (colon_pos) {
                        if (strncmp(line, "program ", 8) == 0) {
                            sscanf(line, "program %d:", &prog_num);
                        } else {
                            sscanf(line, "%d:", &prog_num);
                        }
                        
                        // Find the start of virtual channel info after the colon
                        char *vchan_start = colon_pos + 1;
                        while (isspace(*vchan_start)) vchan_start++;
                        
                        // Extract virtual channel (until first space)
                        int vchan_len = 0;
                        while (vchan_start[vchan_len] && !isspace(vchan_start[vchan_len]) && vchan_len < 31) {
                            vchan[vchan_len] = vchan_start[vchan_len];
                            vchan_len++;
                        }
                        vchan[vchan_len] = '\0';
                        
                        // Extract program name (everything after the virtual channel)
                        char *name_start = vchan_start + vchan_len;
                        while (isspace(*name_start)) name_start++;
                        
                        if (strlen(name_start) > 0) {
                            strncpy(name, name_start, sizeof(name) - 1);
                            name[sizeof(name) - 1] = '\0';
                            // Trim trailing whitespace only
                            char *end = name + strlen(name) - 1;
                            while (end > name && isspace(*end)) *end-- = '\0';
                        }
                        sanitize_string(name);
                        
                        if (prog_num > 0 && strlen(vchan) > 0) {
                            parsed = 1;
                        }
                    }
                    
                    // Only add if we got valid data (not "0" channel indicating failed PSIP)
                    if (parsed && strcmp(vchan, "0") != 0) {
                        json_object *prog_obj = json_object_new_object();
                        json_object *chan_obj = json_object_new_string(vchan);
                        json_object *name_obj = json_object_new_string(strlen(name) > 0 ? name : "");
                        json_object_object_add(prog_obj, "channel", chan_obj);
                        json_object_object_add(prog_obj, "name", name_obj);
                        
                        char prog_key[16];
                        sprintf(prog_key, "%d", prog_num);
                        json_object_object_add(result->virtual_channels, prog_key, prog_obj);
                        
                        if (debug) {
                            printf("Program %d: %s %s\n", prog_num, vchan, name);
                        }
                    }
                }
                
                // Look for ATSC 3.0 program lines: "program=1 plp=0 ..."
                else if (strncmp(line, "program=", 8) == 0) {
                    int prog_num = 0, plp_id = 0;
                    char name[128] = "";
                    
                    // Parse program= and plp= fields
                    if (sscanf(line, "program=%d plp=%d", &prog_num, &plp_id) >= 2) {
                        // Extract everything after "plp=X " as the program name
                        char *plp_pattern = " plp=";
                        char *plp_pos = strstr(line, plp_pattern);
                        if (plp_pos) {
                            // Find the space after the plp number
                            char *name_start = strchr(plp_pos + strlen(plp_pattern), ' ');
                            if (name_start) {
                                name_start++;
                                while (isspace(*name_start)) name_start++;
                                if (strlen(name_start) > 0) {
                                    strncpy(name, name_start, sizeof(name) - 1);
                                    name[sizeof(name) - 1] = '\0';
                                    // Trim trailing whitespace only - keep all parentheses
                                    char *end = name + strlen(name) - 1;
                                    while (end > name && isspace(*end)) *end-- = '\0';
                                }
                            }
                        }
                        sanitize_string(name);
                        
                        json_object *prog_obj = json_object_new_object();
                        json_object *plp_obj = json_object_new_int(plp_id);
                        json_object *name_obj = json_object_new_string(strlen(name) > 0 ? name : "");
                        json_object_object_add(prog_obj, "plp", plp_obj);
                        json_object_object_add(prog_obj, "name", name_obj);
                        
                        char prog_key[16];
                        sprintf(prog_key, "%d", prog_num);
                        json_object_object_add(result->virtual_channels, prog_key, prog_obj);
                        
                        if (debug) {
                            printf("ATSC 3.0 Program %d (PLP %d): %s\n", prog_num, plp_id, name);
                        }
                    }
                }
                
                line = strtok(NULL, "\n");
            }
            free(streaminfo_copy);
        }
    }
    
    // Handle all ATSC 3.0 specific data
    if (result->is_atsc3) {
        // Get l1detail string
        char var_name[64];
        sprintf(var_name, "/tuner%d/l1detail", config->tuner_index);
        char *l1detail_str = NULL;
        char *error_str = NULL;
        if (hdhomerun_device_get_var(hd, var_name, &l1detail_str, &error_str) > 0 && l1detail_str) {
            strncpy(result->l1detail, l1detail_str, sizeof(result->l1detail) - 1);
            result->l1detail[sizeof(result->l1detail) - 1] = '\0';
            if (debug) printf("Found l1detail string.\n");
        } else if (debug) {
            /* Not all HDHomeRun models support the l1detail variable; this is
             * not an error condition, just an informational note. plp_info is
             * still reported normally for these devices. */
            printf("l1detail not supported by this tuner (this is normal for some models)\n");
        }
        
        // If we don't have TSID from streaminfo, try to get BSID from plpinfo
        if (result->tsid == -999) {
            char *plpinfo;
            if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo) > 0) {
                result->bsid = parse_status_value(plpinfo, "bsid=");
                // Use BSID as TSID for server compatibility
                if (result->bsid != -999) {
                    result->tsid = result->bsid;
                }
                if (debug && result->bsid != -999) {
                    printf("Found BSID: %ld (0x%lx), using as TSID\n", result->bsid, result->bsid);
                }
                
                // Store PLP info for potential retry logic
                if (result->plp_info) {
                    json_object_put(result->plp_info);
                }
                result->plp_info = json_object_new_object();
                char *plpinfo_copy = strdup(plpinfo);
                if (plpinfo_copy) {
                    char *line = strtok(plpinfo_copy, "\n");
                    while (line != NULL) {
                        if (strncmp(line, "bsid=", 5) != 0) {
                            int plp_id;
                            if (sscanf(line, "%d:", &plp_id) == 1) {
                                json_object *plp_obj = parse_plp_string_to_json(line);
                                char plp_key[16];
                                sprintf(plp_key, "%d", plp_id);
                                json_object_object_add(result->plp_info, plp_key, plp_obj);
                            }
                        }
                        line = strtok(NULL, "\n");
                    }
                    free(plpinfo_copy);
                }
            }
        }
    }
    
    return true;
}

/*
 * create_scan_json - Create JSON object for submission to rabbitears.info
 */
json_object *create_scan_json(struct scanner_config *config, 
                             struct channel_result *results, int result_count) {
    json_object *scan = json_object_new_object();
    
    // Create user ID
    /* FIX: use the real device ID (like the Perl's discover step) rather
     * than a literal "FFFFFFFF" wildcard. */
    const char *id_for_user = (config->resolved_tuner_id[0] != '\0')
                              ? config->resolved_tuner_id : config->tuner_id;
    char user_id[64];
    snprintf(user_id, sizeof(user_id), "TunerID_%s_tuner%d",
             id_for_user, config->tuner_index);
    json_object *user_id_obj = json_object_new_string(user_id);
    json_object_object_add(scan, "user_id", user_id_obj);
    
    // Add override fields if configured
    if (strlen(config->override_callsign) > 0) {
        json_object *callsign_obj = json_object_new_string(config->override_callsign);
        json_object_object_add(scan, "override_callsign", callsign_obj);
    }
    if (strlen(config->override_location) > 0) {
        json_object *location_obj = json_object_new_string(config->override_location);
        json_object_object_add(scan, "override_location", location_obj);
    }
    
    // Create rf_channel object
    json_object *rf_channels = json_object_new_object();
    
    for (int i = 0; i < result_count; i++) {
        struct channel_result *result = &results[i];
        
        json_object *channel_obj = json_object_new_object();
        
        json_object *mod_obj = json_object_new_string(result->modulation);
        json_object_object_add(channel_obj, "modulation", mod_obj);
        
        /* Match Perl's JSON output exactly: strength/sig_noise/symbol_err
         * are emitted as strings (the Perl gets these from regex captures,
         * which Perl's JSON encoder serializes as strings). The previous
         * comment claimed numeric types were "CORRECTED" but the working
         * Perl reference uses strings, and the server appears to expect them. */
        char numbuf[16];
        
        snprintf(numbuf, sizeof(numbuf), "%d", result->signal_strength);
        json_object_object_add(channel_obj, "strength", json_object_new_string(numbuf));
        
        snprintf(numbuf, sizeof(numbuf), "%d", result->signal_quality);
        json_object_object_add(channel_obj, "sig_noise", json_object_new_string(numbuf));
        
        snprintf(numbuf, sizeof(numbuf), "%d", result->symbol_error_quality);
        json_object_object_add(channel_obj, "symbol_err", json_object_new_string(numbuf));
        
        // TSID: number when present, empty string when not
        if (result->tsid != -999) {
            json_object *tsid_obj = json_object_new_int64(result->tsid);
            json_object_object_add(channel_obj, "tsid", tsid_obj);
        } else {
            json_object *tsid_obj = json_object_new_string("");
            json_object_object_add(channel_obj, "tsid", tsid_obj);
        }
        
        json_object_object_add(channel_obj, "virtual", result->virtual_channels);
        
        if (result->is_atsc3 && result->plp_info) {
            json_object_object_add(channel_obj, "plp_info", result->plp_info);
        }
        
        if (result->is_atsc3 && strlen(result->l1detail) > 0) {
            json_object *l1detail_obj = json_object_new_string(result->l1detail);
            json_object_object_add(channel_obj, "l1detail", l1detail_obj);
        }
        
        // Omit the reporter_callsign key if it's empty
        if (strlen(result->reporter_callsign) > 0) {
            json_object *callsign_obj = json_object_new_string(result->reporter_callsign);
            json_object_object_add(channel_obj, "reporter_callsign", callsign_obj);
        }
        
        char channel_key[16];
        sprintf(channel_key, "%u", result->rf_channel);
        json_object_object_add(rf_channels, channel_key, channel_obj);
    }
    
    json_object_object_add(scan, "rf_channel", rf_channels);
    
    return scan;
}

/*
 * upload_state - holds buffer and offset for POST body upload
 */
struct upload_state {
    const char *data;
    size_t size;
    size_t offset;
};

/*
 * upload_read_callback - libcurl pulls POST body bytes from us via this.
 * Returning bytes from a callback is the most binary-safe upload method.
 */
static size_t upload_read_callback(char *buffer, size_t size, size_t nitems, void *userp) {
    struct upload_state *state = (struct upload_state *)userp;
    size_t available = state->size - state->offset;
    size_t to_copy = size * nitems;
    if (to_copy > available) to_copy = available;
    if (to_copy > 0) {
        memcpy(buffer, state->data + state->offset, to_copy);
        state->offset += to_copy;
    }
    return to_copy;
}

/*
 * write_callback - Callback for curl to handle response data
 */
size_t write_callback(void *contents, size_t size, size_t nmemb, struct http_response *response) {
    size_t realsize = size * nmemb;
    char *ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }
    
    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = 0;
    
    return realsize;
}

/*
 * compress_json_bzip2 - Compress JSON string using bzip2
 * Returns the compressed data and sets compressed_size
 * Caller must free the returned buffer
 */
/*
 * compress_json_bzip2 - Compress JSON string using bzip2
 * Returns the compressed data and sets compressed_size
 * Caller must free the returned buffer
 */
char *compress_json_bzip2(const char *json_str, size_t *compressed_size, bool debug) {
    size_t json_len = strlen(json_str);
    
    if (debug) {
        printf("Input JSON length: %zu bytes\n", json_len);
    }
    
    /* Output format mimics Perl's Compress::Bzip2::memBzip:
     *   byte 0:    0xF0 (magic)
     *   bytes 1-4: original (uncompressed) length, big-endian 32-bit
     *   bytes 5+:  standard bzip2 stream
     * The server-side handler uses Compress::Bzip2::memBunzip which expects
     * this framing; sending raw bzip2 (BZh...) without the prefix causes
     * intermittent decompression failures on the server, which then reports
     * "missing user_id" because the JSON is mangled. */
    
    const size_t header_size = 5;
    
    // BZ2 worst case: input size + 1% + 600 bytes; plus header
    size_t max_compressed_size = json_len + (json_len / 100) + 600 + header_size;
    char *compressed_data = malloc(max_compressed_size);
    if (!compressed_data) {
        printf("Failed to allocate memory for compression\n");
        return NULL;
    }
    
    /* Write the memBzip header */
    unsigned char *p = (unsigned char *)compressed_data;
    p[0] = 0xF0;
    p[1] = (unsigned char)((json_len >> 24) & 0xFF);
    p[2] = (unsigned char)((json_len >> 16) & 0xFF);
    p[3] = (unsigned char)((json_len >>  8) & 0xFF);
    p[4] = (unsigned char)( json_len        & 0xFF);
    
    /* Compress into the buffer starting after the 5-byte header */
    unsigned int out_size = (unsigned int)(max_compressed_size - header_size);
    int bz_result = BZ2_bzBuffToBuffCompress(
        compressed_data + header_size,
        &out_size,
        (char *)json_str,
        (unsigned int)json_len,
        9,  // blockSize100k (9 = best compression)
        0,  // verbosity
        30  // workFactor
    );
    
    if (bz_result != BZ_OK) {
        printf("BZ2 compression failed with error code: %d\n", bz_result);
        free(compressed_data);
        return NULL;
    }
    
    *compressed_size = out_size + header_size;
    
    if (debug) {
        printf("Compressed to %u bytes (%.1f%% of original)\n", 
               out_size, (out_size * 100.0) / json_len);
    }
    
    return compressed_data;
}

/*
 * submit_results - Submits JSON to the server.
 */
struct http_response submit_results(const char *url, json_object *scan_data, bool debug) {
    CURL *curl;
    CURLcode res;
    struct http_response response = {0};

    const char *json_string = json_object_to_json_string_ext(scan_data, JSON_C_TO_STRING_PLAIN);
    size_t json_len = strlen(json_string);

    if (debug) {
        printf("Original JSON length: %zu bytes\n", json_len);
        /* FIX: print the complete payload (pretty-printed for reading,
         * like the Perl's debug output). The bytes actually submitted are
         * still the compact form above. Note: json-c reuses its internal
         * string buffer per object, so json_string is re-fetched after. */
        printf("JSON (full, pretty-printed for display; sent compact):\n%s\n",
               json_object_to_json_string_ext(scan_data, JSON_C_TO_STRING_PRETTY |
                                                         JSON_C_TO_STRING_NOSLASHESCAPE));
        json_string = json_object_to_json_string_ext(scan_data, JSON_C_TO_STRING_PLAIN);
        json_len = strlen(json_string);
    }

    // Compress the JSON data
    size_t compressed_size = 0;
    char *compressed_data = compress_json_bzip2(json_string, &compressed_size, debug);
    
    if (!compressed_data) {
        printf("Failed to compress JSON data\n");
        return response;
    }
    
    if (compressed_size == 0) {
        printf("Compression resulted in zero bytes\n");
        free(compressed_data);
        return response;
    }

    curl = curl_easy_init();
    if (!curl) {
        printf("Failed to initialize curl\n");
        free(compressed_data);
        return response;
    }

    /* Use a read callback for the POST body. This is the most binary-safe
     * approach in libcurl — bytes are pulled via a callback that knows the
     * exact size, with no opportunity for any code path to apply strlen()
     * to the buffer. */
    struct upload_state ustate = { compressed_data, compressed_size, 0 };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)compressed_size);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, upload_read_callback);
    curl_easy_setopt(curl, CURLOPT_READDATA, &ustate);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    /* FIX: timeouts. Without these, a server that accepts the connection
     * but never answers (or a half-open TCP connection) blocks forever.
     * LWP::UserAgent's default is a 180 s *inactivity* timeout; the
     * low-speed limit below is libcurl's equivalent. */
    apply_curl_timeouts(curl, HTTP_CONNECT_TIMEOUT, HTTP_TOTAL_TIMEOUT);

    /* Set User-Agent. Match LWP's default since the server is known to
     * accept submissions from the Perl version. */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "libwww-perl/6.00");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream; charset=binary");
    /* FIX: suppress "Expect: 100-continue". LWP never sends it, and a
     * server that doesn't answer it makes libcurl pause before the body. */
    headers = curl_slist_append(headers, "Expect:");
    /* Do NOT manually set Content-Length — libcurl generates it from
     * CURLOPT_POSTFIELDSIZE. A manual header here can produce duplicate
     * or malformed Content-Length on the wire. */
    /* Do NOT manually set Connection: close either — let libcurl manage it. */

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (debug) {
        printf("Submitting compressed scan results to %s\n", url);
        printf("Compressed size: %zu bytes\n", compressed_size);
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        printf("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        if (response.data) free(response.data);
        response.data = NULL;
        response.size = 0;
    } else {
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (debug) {
            printf("HTTP response code: %ld\n", response_code);
        }
        if (response_code < 200 || response_code >= 300) {
            printf("HTTP error: %ld\n", response_code);
            if (response.data) {
                printf("Server response body: %s\n", response.data);
            }
            if (response.data) free(response.data);
            response.data = NULL;
            response.size = 0;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(compressed_data);

    return response;
}

/*
 * signal_handler - Handle graceful shutdown
 */
void signal_handler(int sig) {
    /* FIX: printf() is not async-signal-safe; use write() instead. */
    (void)sig;
    static const char msg[] = "\nReceived signal, shutting down gracefully...\n";
    ssize_t rc = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)rc;
    keep_running = 0;
}

/*
 * NEW: read_callback for FTP upload
 * This reads data from the local file for libcurl to send.
 */
static size_t read_callback(void *ptr, size_t size, size_t nmemb, void *stream) {
    FILE *read_file = (FILE *)stream;
    if (!read_file) {
        return CURL_READFUNC_ABORT;
    }
    size_t retcode = fread(ptr, size, nmemb, read_file);
    return retcode;
}

/*
 * NEW: upload_file_ftp
 * Uploads a single file to the specified FTP server.
 */
bool upload_file_ftp(const char *filename, bool debug) {
    CURL *curl;
    CURLcode res;
    FILE *file_to_upload;
    struct stat file_info;
    bool success = false;

    if (stat(filename, &file_info) != 0) {
        printf("Error getting file stats for %s\n", filename);
        return false;
    }

    file_to_upload = fopen(filename, "rb");
    if (!file_to_upload) {
        printf("Error opening file %s for reading\n", filename);
        return false;
    }

    curl = curl_easy_init();
    if (curl) {
        char url[512];
        // Get just the filename part for the URL to avoid path issues
        const char *basename = strrchr(filename, '/');
        if (basename) {
            basename++; // Move past the slash
        } else {
            basename = filename; // No slash, filename is the basename
        }

        snprintf(url, sizeof(url), "ftp://www.rabbitears.info/lbupload/%s", basename);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_USERPWD, "tsupload:uploader");
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, file_to_upload);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_info.st_size);

        /* FIX: timeouts for FTP. Control connection, server responses,
         * and the data connection could all previously hang forever.
         * No hard total ceiling: large captures on a slow uplink are
         * legitimate; the low-speed limit catches a stalled transfer. */
        apply_curl_timeouts(curl, FTP_CONNECT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_SERVER_RESPONSE_TIMEOUT, FTP_RESPONSE_TIMEOUT);
        /* FIX: go straight to PASV like Perl's Net::FTP; EPSV is a common
         * source of hung data connections behind NAT/firewalls. */
        curl_easy_setopt(curl, CURLOPT_FTP_USE_EPSV, 0L);

        if (debug) {
            printf("Uploading %s to %s\n", filename, url);
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed for FTP: %s\n", curl_easy_strerror(res));
        } else {
            printf("FTP upload of %s successful\n", filename);
            success = true;
        }

        curl_easy_cleanup(curl);
    }

    fclose(file_to_upload);

    // Clean up the local file after a successful upload
    if (success) {
        if(remove(filename) != 0) {
            printf("Warning: could not delete local capture file %s\n", filename);
        }
    }

    return success;
}

/*
 * NEW: record_channel_capture
 * Tunes to a channel, records for a duration, and saves to a file using a direct socket connection.
 */
bool record_channel_capture(struct hdhomerun_device_t *hd, const struct scanner_config *config, unsigned int channel, int duration, char *filename, bool debug) {
    printf("Starting capture of channel %u for %d seconds to %s\n", channel, duration, filename);

    // Tune to the channel
    char tune_str[64];
    sprintf(tune_str, "auto:%u", channel);
    if (hdhomerun_device_set_tuner_channel(hd, tune_str) < 0) {
        if (debug) printf("Failed to tune to channel %u for capture\n", channel);
        return false;
    }

    // Wait for lock
    struct hdhomerun_tuner_status_t status;
    if (hdhomerun_device_wait_for_lock(hd, &status) <= 0) {
        if (debug) printf("No lock on channel %u for capture\n", channel);
        return false;
    }
    if (debug) printf("Lock achieved on channel %u for capture (%s)\n", channel, status.lock_str);

    bool is_atsc3 = (strstr(status.lock_str, "atsc3") != NULL);

    // Get device IP
    uint32_t device_ip_int = hdhomerun_device_get_device_ip(hd);
    if (device_ip_int == 0) {
        printf("Error: Could not get device IP address.\n");
        return false;
    }
    struct in_addr addr;
    addr.s_addr = htonl(device_ip_int);
    const char *ip_addr = inet_ntoa(addr);

    // --- Direct Socket Implementation ---
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        printf("Error: Could not create socket.\n");
        return false;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(5004);
    if (inet_pton(AF_INET, ip_addr, &serv_addr.sin_addr) <= 0) {
        printf("Error: Invalid IP address '%s'.\n", ip_addr);
        close(sock);
        return false;
    }

    /* FIX: bound the connect() — on Linux SO_SNDTIMEO applies to connect.
     * Otherwise an unreachable device blocks for the kernel's SYN retry
     * period (~2 minutes). */
    {
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        printf("Error: Could not connect to device at %s:5004.\n", ip_addr);
        close(sock);
        return false;
    }

    // NEW: Set the socket to non-blocking mode
    fcntl(sock, F_SETFL, O_NONBLOCK);

    char request[512];
    if (is_atsc3) {
        // Check if this is a dev tuner to decide on capture format
        char *raw_status_str;
        struct hdhomerun_tuner_status_t temp_status;
        hdhomerun_device_get_tuner_status(hd, &raw_status_str, &temp_status);
        
        bool is_dev_tuner = (parse_db_value(raw_status_str, "ss=") != -999);
        const char *format = is_dev_tuner ? "alp-pcap" : "dbg";
        const char *ext = is_dev_tuner ? ".pcap" : ".dbg";

        if (debug) {
            printf("Device is %s a dev unit. Using format '%s'.\n", is_dev_tuner ? "" : "not", format);
        }

        // Since the filename is passed in, we must modify its extension here
        char *dot = strrchr(filename, '.');
        if (dot != NULL) {
            strcpy(dot, ext);
            if (debug) {
                printf("Updated capture filename to: %s\n", filename);
            }
        }

        // --- NEW ATSC 3.0 LOGIC ---
        // 1. Get all available PLPs and construct a multi-PLP tune string
        char all_plps_tune_str[128] = {0};
        char *plpinfo_str;
        if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str) > 0) {
            char *plpinfo_copy = strdup(plpinfo_str);
            if (plpinfo_copy) {
                bool first = true;
                char *line = strtok(plpinfo_copy, "\n");
                while (line != NULL) {
                    if (strncmp(line, "bsid=", 5) != 0) {
                        int plp_id;
                        if (sscanf(line, "%d:", &plp_id) == 1 && plp_id != 16) {
                            if (!first) strcat(all_plps_tune_str, "+");
                            char temp[8];
                            sprintf(temp, "%d", plp_id);
                            strcat(all_plps_tune_str, temp);
                            first = false;
                        }
                    }
                    line = strtok(NULL, "\n");
                }
                free(plpinfo_copy);
            }
        }

        // 2. Retune the device to lock all available PLPs
        if (strlen(all_plps_tune_str) > 0) {
            char new_tune_cmd[256];
            snprintf(new_tune_cmd, sizeof(new_tune_cmd), "atsc3:%u:%s", channel, all_plps_tune_str);
            if (debug) printf("Retuning to lock all available PLPs: %s\n", new_tune_cmd);
            hdhomerun_device_set_tuner_channel(hd, new_tune_cmd);
            hdhomerun_device_wait_for_lock(hd, &status); // Wait for the new lock
            sleep(1); // Give it a second to settle
        }

        // 3. Now, get the list of currently locked PLPs for the URL
        char plp_url_str[128] = {0};
        bool plps_locked = false;
        for (int retry = 0; retry < 4; retry++) {
            if (hdhomerun_device_get_tuner_plpinfo(hd, &plpinfo_str) > 0) {
                char *plpinfo_copy = strdup(plpinfo_str);
                if (plpinfo_copy) {
                    plp_url_str[0] = '\0';
                    char *line = strtok(plpinfo_copy, "\n");
                    while(line != NULL) {
                        if (strstr(line, "lock=1")) {
                            int plp_id;
                            if (sscanf(line, "%d:", &plp_id) == 1) {
                                char temp[8];
                                sprintf(temp, "p%d", plp_id);
                                strcat(plp_url_str, temp);
                            }
                        }
                        line = strtok(NULL, "\n");
                    }
                    free(plpinfo_copy);
                }
            }
            if (strlen(plp_url_str) > 0) {
                plps_locked = true;
                break;
            }
            if (retry < 3) {
                if (debug) printf("Could not find locked PLPs, retrying in 1s...\n");
                sleep(1);
            }
        }

        if (!plps_locked) {
            printf("Error: Could not find any locked PLPs for ATSC 3.0 capture after retries.\n");
            close(sock);
            return false;
        }
        
        snprintf(request, sizeof(request), "GET /tuner%d/ch%u%s?format=%s&duration=%d HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", config->tuner_index, channel, plp_url_str, format, duration, ip_addr);
    } else {
        // Standard ATSC 1.0 URL
        snprintf(request, sizeof(request), "GET /tuner%d/ch%u?duration=%d HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", config->tuner_index, channel, duration, ip_addr);
    }
    
    if (debug) {
        char *path_start = strchr(request, ' ');
        char *path_end = strstr(request, " HTTP/1.0");
        if (path_start && path_end) {
            path_start++;
            printf("Attempting to capture from URL: http://%s:5004%.*s\n", ip_addr, (int)(path_end - path_start), path_start);
        }
    }

    /* FIX: MSG_NOSIGNAL — if the device resets the connection, the default
     * SIGPIPE action would terminate the whole scanner. */
    if (send(sock, request, strlen(request), MSG_NOSIGNAL) < 0) {
        perror("send");
        printf("Error: Failed to send request.\n");
        close(sock);
        return false;
    }

    FILE *outfile = fopen(filename, "wb");
    if (!outfile) {
        printf("Failed to open file %s for writing\n", filename);
        close(sock);
        return false;
    }

    bool success = true;
    bool headers_processed = false;
    char buffer[65536];
    /* FIX: accumulate the HTTP header across recv() calls. Previously
     * strstr() ran on a non-NUL-terminated binary buffer (over-read), and
     * a header split across two reads was never found, so the whole
     * capture was discarded. */
    char hdr_buf[8192];
    size_t hdr_len = 0;
    time_t start_time = time(NULL);
    unsigned long long total_bytes_written = 0;

    while ((time(NULL) - start_time) < duration && keep_running) {
        int bytes_read = recv(sock, buffer, sizeof(buffer), 0);

        if (bytes_read > 0) {
            char *data_to_write = buffer;
            size_t len_to_write = (size_t)bytes_read;

            if (!headers_processed) {
                size_t space = sizeof(hdr_buf) - hdr_len;
                size_t take = ((size_t)bytes_read < space) ? (size_t)bytes_read : space;
                memcpy(hdr_buf + hdr_len, buffer, take);
                size_t prev_len = hdr_len;
                hdr_len += take;

                char *sep = memmem(hdr_buf, hdr_len, "\r\n\r\n", 4);
                if (sep) {
                    size_t hdr_end = (size_t)(sep - hdr_buf) + 4;   /* in hdr_buf */

                    /* Check the status line: anything but 200 (e.g. 503
                     * tuner busy) is a failed capture. */
                    if (hdr_len < 12 || memcmp(hdr_buf, "HTTP/1.", 7) != 0 ||
                        memcmp(hdr_buf + 9, "200", 3) != 0) {
                        int line_len = 0;
                        while (line_len < (int)hdr_end && hdr_buf[line_len] != '\r') line_len++;
                        printf("Capture request rejected by device: %.*s\n", line_len, hdr_buf);
                        success = false;
                        break;
                    }

                    /* Body starts at hdr_end in hdr_buf, which corresponds
                     * to offset (hdr_end - prev_len) in this recv buffer. */
                    size_t body_off = hdr_end - prev_len;
                    data_to_write = buffer + body_off;
                    len_to_write = (size_t)bytes_read - body_off;
                    headers_processed = true;
                } else {
                    if (hdr_len >= sizeof(hdr_buf)) {
                        printf("Capture failed: HTTP header from device too large or malformed\n");
                        success = false;
                        break;
                    }
                    len_to_write = 0;
                }
            }

            if (len_to_write > 0) {
                if (fwrite(data_to_write, 1, len_to_write, outfile) != len_to_write) {
                    printf("File write error during capture\n");
                    success = false;
                    break;
                }
                total_bytes_written += len_to_write;
            }
        } else if (bytes_read == 0) {
            if(debug) printf("Connection closed by device during capture.\n");
            break;
        } else { // bytes_read < 0
             if (errno != EWOULDBLOCK && errno != EAGAIN) {
                 perror("recv");
                 success = false;
                 break;
             }
             usleep(10000); // 10ms
        }
    }

    fclose(outfile);
    close(sock);

    if (success && headers_processed && total_bytes_written > 1024) {
        printf("Capture of channel %u finished successfully.\n", channel);
    } else {
        printf("Capture of channel %u failed (wrote %llu bytes).\n", channel, total_bytes_written);
        remove(filename);
        success = false;
    }
    return success;
}


/*
 * NEW: parse_capture_requests
 * Parses a JSON string from the server to look for capture requests.
 * Expected format: {"captures": [{"channel": 25, "duration": 15}, ...]}
 */
void parse_capture_requests(const char *json_str, bool debug) {
    if (!json_str || strlen(json_str) == 0) return;

    json_object *root = json_tokener_parse(json_str);
    if (!root) {
        if (debug) printf("Failed to parse capture request JSON\n");
        return;
    }

    json_object *captures_array;
    if (!json_object_object_get_ex(root, "captures", &captures_array)) {
        if (debug) printf("No 'captures' array in server response\n");
        json_object_put(root);
        return;
    }

    /* FIX: json_object_array_length() asserts (aborts) on a non-array. */
    if (!json_object_is_type(captures_array, json_type_array)) {
        if (debug) printf("'captures' in server response is not an array\n");
        json_object_put(root);
        return;
    }

    int array_len = json_object_array_length(captures_array);
    if (debug) printf("Server requested %d captures\n", array_len);

    for (int i = 0; i < array_len; i++) {
        if (pending_capture_count >= MAX_CAPTURE_REQUESTS) {
            printf("Capture request queue is full, ignoring further requests\n");
            break;
        }

        json_object *capture_obj = json_object_array_get_idx(captures_array, i);
        json_object *channel_obj, *duration_obj;

        if (json_object_object_get_ex(capture_obj, "channel", &channel_obj) &&
            json_object_object_get_ex(capture_obj, "duration", &duration_obj)) {

            int req_channel = json_object_get_int(channel_obj);
            int req_duration = json_object_get_int(duration_obj);

            /* FIX: validate server-supplied values. An oversized duration
             * would park the scanner in a capture for that long (times up
             * to 5 retries). */
            if (req_channel < 2 || req_channel > 69) {
                printf("Ignoring capture request for invalid channel %d\n", req_channel);
                continue;
            }
            if (req_duration < MIN_CAPTURE_DURATION || req_duration > MAX_CAPTURE_DURATION) {
                int clamped = req_duration < MIN_CAPTURE_DURATION ? MIN_CAPTURE_DURATION
                                                                  : MAX_CAPTURE_DURATION;
                printf("Capture duration %d s for channel %d out of range; using %d s\n",
                       req_duration, req_channel, clamped);
                req_duration = clamped;
            }

            /* FIX: skip duplicates if the server re-sends a request that
             * is still queued, so repeats can't fill the queue. */
            bool duplicate = false;
            for (int q = 0; q < pending_capture_count; q++) {
                if (pending_captures[q].channel == (unsigned int)req_channel) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                if (debug) printf("Capture for channel %d already queued, skipping\n", req_channel);
                continue;
            }

            pending_captures[pending_capture_count].channel = (unsigned int)req_channel;
            pending_captures[pending_capture_count].duration = req_duration;
            pending_captures[pending_capture_count].attempts = 0;

            if (debug) {
                printf("Queued capture: Channel %u for %d seconds\n",
                       pending_captures[pending_capture_count].channel,
                       pending_captures[pending_capture_count].duration);
            }
            pending_capture_count++;
        }
    }
    json_object_put(root);
}


/*
 * print_usage - Print command line usage
 */
void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -c, --config FILE    Configuration file (default: %s)\n", DEFAULT_CONFIG_FILE);
    printf("  -d, --debug          Enable debug output\n");
    printf("  -h, --help           Show this help\n");
    printf("  -t, --tuner ID       Tuner ID (overrides config)\n");
    printf("  -i, --index NUM      Tuner index (overrides config)\n");
    printf("  -u, --url URL        Submit URL (overrides config)\n");
    printf("  -o, --override LIST  Per-channel callsign override(s).\n");
    printf("                       Format: CH,CALL[,CH,CALL,...]\n");
    printf("                       Example: -o 26,W26EV,18,W18EG\n");
    printf("\nTesting Options:\n");
    printf("      --test-capture CHAN  Record a capture from a channel and upload it, then exit.\n");
    printf("      --test-duration SECS Duration in seconds for test capture (default: 30).\n");
    printf("      --test-upload FILE   Upload a specific local file via FTP, then exit.\n");
    printf("\nConfiguration file format:\n");
    printf("  tuner_id=FFFFFFFF\n");
    printf("  tuner_index=0\n");
    printf("  min_channel=2\n");
    printf("  max_channel=69\n");
    printf("  spot_url=http://www.rabbitears.info/tvdx/raw_spot\n");
    printf("  seek_timeout=3.0\n");
    printf("  psip_wait_time=2\n");
    printf("  atsc3_psip_wait_time=10\n");
    printf("  retry_delay=10\n");
    printf("  debug=false\n");
    printf("  use_channel_map=false\n");
    printf("  override_callsign=\n");
    printf("  override_location=\n");
    printf("  override_channel=26,W26EV  (repeatable; per-channel callsign)\n");
}

/*
 * main - Main program entry point
 */
int main(int argc, char *argv[]) {
    /* FIX: line-buffer stdout so log output under systemd / nohup / a
     * redirect appears promptly instead of in large delayed bursts
     * (which can look like a stall). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct scanner_config config;
    char config_file[MAX_FILENAME] = DEFAULT_CONFIG_FILE;
    bool config_debug_override = false;
    
    // Command line option parsing
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"debug", no_argument, 0, 'd'},
        {"help", no_argument, 0, 'h'},
        {"tuner", required_argument, 0, 't'},
        {"index", required_argument, 0, 'i'},
        {"url", required_argument, 0, 'u'},
        {"override", required_argument, 0, 'o'},
        {"test-capture", required_argument, 0, 1001},
        {"test-duration", required_argument, 0, 1002},
        {"test-upload", required_argument, 0, 1003},
        {0, 0, 0, 0}
    };
    
    // NEW: Variables for test modes
    unsigned int test_capture_channel = 0;
    int test_duration = 30;
    char *test_upload_filename = NULL;

    int opt;
    while ((opt = getopt_long(argc, argv, "c:dht:i:u:o:", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                strncpy(config_file, optarg, sizeof(config_file) - 1);
                break;
            case 'd':
                config_debug_override = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 't':
                // Will be handled after config load
                break;
            case 'i':
                // Will be handled after config load
                break;
            case 'u':
                // Will be handled after config load
                break;
            case 'o':
                // Will be handled after config load
                break;
            case 1001: // --test-capture
                test_capture_channel = (unsigned int)atoi(optarg);
                break;
            case 1002: // --test-duration
                test_duration = atoi(optarg);
                if (test_duration <= 0) test_duration = 30;
                break;
            case 1003: // --test-upload
                test_upload_filename = optarg;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    // Load configuration
    load_config(config_file, &config);
    
    // Apply command line overrides
    if (config_debug_override) {
        config.debug = true;
    }
    
    // Re-parse for overrides that need to be applied after config load
    optind = 1; // Reset getopt
    while ((opt = getopt_long(argc, argv, "c:dht:i:u:o:", long_options, NULL)) != -1) {
        switch (opt) {
            case 't':
                strncpy(config.tuner_id, optarg, sizeof(config.tuner_id) - 1);
                break;
            case 'i':
                config.tuner_index = atoi(optarg);
                break;
            case 'u':
                strncpy(config.spot_url, optarg, sizeof(config.spot_url) - 1);
                break;
            case 'o': {
                // Parse Perl-compatible -o CH,CALL,CH,CALL,... and append
                // to channel_overrides. Adds to (does not replace) any
                // override_channel= entries already loaded from config.
                int max_overrides = (int)(sizeof(config.channel_overrides) /
                                          sizeof(config.channel_overrides[0]));
                char *buf = strdup(optarg);
                if (buf) {
                    char *saveptr = NULL;
                    char *tok = strtok_r(buf, ",", &saveptr);
                    while (tok != NULL) {
                        // Trim leading space on channel token
                        while (isspace(*tok)) tok++;
                        char *call = strtok_r(NULL, ",", &saveptr);
                        if (!call) {
                            fprintf(stderr,
                                "-o option: missing callsign for channel '%s'\n", tok);
                            break;
                        }
                        while (isspace(*call)) call++;
                        int ch_num = atoi(tok);
                        if (ch_num < 2 || ch_num > 69) {
                            fprintf(stderr,
                                "-o option: bad channel number '%s' (must be 2-69)\n", tok);
                        } else if (*call == '\0') {
                            fprintf(stderr,
                                "-o option: empty callsign for channel %d\n", ch_num);
                        } else if (config.num_channel_overrides >= max_overrides) {
                            fprintf(stderr,
                                "-o option: too many overrides (max %d)\n", max_overrides);
                            break;
                        } else {
                            int idx = config.num_channel_overrides;
                            config.channel_overrides[idx].channel = (unsigned int)ch_num;
                            strncpy(config.channel_overrides[idx].callsign, call,
                                    sizeof(config.channel_overrides[idx].callsign) - 1);
                            config.channel_overrides[idx].callsign[
                                sizeof(config.channel_overrides[idx].callsign) - 1] = '\0';
                            config.num_channel_overrides++;
                        }
                        tok = strtok_r(NULL, ",", &saveptr);
                    }
                    free(buf);
                }
                break;
            }
        }
    }
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    /* FIX: belt-and-braces against SIGPIPE on any socket write */
    signal(SIGPIPE, SIG_IGN);

    // --- NEW: HANDLE TEST MODES ---
    // If a test mode is selected, the program will perform the test and then exit.
    if (test_upload_filename) {
        printf("--- Running in Test Upload Mode ---\n");
        printf("Uploading file: %s\n", test_upload_filename);
        if (upload_file_ftp(test_upload_filename, true)) { // Force debug for clarity
             printf("--- Test upload successful ---\n");
        } else {
             printf("--- Test upload FAILED ---\n");
        }
        curl_global_cleanup();
        return 0; // Exit after test
    }

    if (test_capture_channel > 0) {
        printf("--- Running in Test Capture Mode ---\n");
        printf("Channel: %u, Duration: %d seconds\n", test_capture_channel, test_duration);

        struct hdhomerun_device_t *test_hd = hdhomerun_device_create_from_str(config.tuner_id, NULL);
        if (!test_hd) {
            printf("Failed to create HDHomeRun device for test capture with ID %s\n", config.tuner_id);
            if (strcmp(config.tuner_id, "FFFFFFFF") == 0) {
                 printf("Try specifying an exact tuner ID with the -t option.\n");
                 printf("Use 'hdhomerun_config discover' to find your device ID.\n");
            }
            curl_global_cleanup();
            return 1;
        }

        hdhomerun_device_set_tuner(test_hd, config.tuner_index);
        
        char capture_filename[MAX_FILENAME];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        snprintf(capture_filename, sizeof(capture_filename), "test_rf%u_%s_%04d%02d%02d_%02d%02d%02d.ts",
                 test_capture_channel, config.tuner_id, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);

        bool capture_ok = record_channel_capture(test_hd, &config, test_capture_channel, test_duration, capture_filename, true);
        if (capture_ok) {
            printf("--- Test capture complete, starting upload ---\n");
            if(upload_file_ftp(capture_filename, true)) {
                printf("--- Test upload successful ---\n");
            } else {
                printf("--- Test upload FAILED ---\n");
            }
        } else {
            printf("--- Test capture FAILED ---\n");
        }
        
        hdhomerun_device_destroy(test_hd);
        curl_global_cleanup();
        return 0; // Exit after test
    }


    if (config.debug) {
        printf("HDHomeRun Scanner starting with configuration:\n");
        printf("  Tuner ID: %s\n", config.tuner_id);
        printf("  Tuner Index: %d\n", config.tuner_index);
        printf("  Channel Range: %u-%u\n", config.min_channel, config.max_channel);
        printf("  Submit URL: %s\n", config.spot_url);
        printf("  Seek Timeout: %.1fs\n", config.seek_timeout);
        printf("  Retry Delay: %ds\n", config.retry_delay);
        printf("  Use Channel Map: %s\n", config.use_channel_map ? "yes" : "no");
    }
    
    // Main scanning loop
    while (keep_running) {
        // --- NEW: HANDLE PENDING CAPTURE REQUESTS ---
        if (pending_capture_count > 0) {
            struct capture_request *req = &pending_captures[0];
            req->attempts++; // Increment attempt counter at the start
            /* FIX: remember whether this request was already removed so the
             * give-up check below doesn't remove a second entry (which could
             * drive pending_capture_count to -1 and corrupt memory). */
            bool request_removed = false;
            unsigned int req_channel = req->channel;
            printf("Processing capture request for channel %u (attempt %d/5)\n", req->channel, req->attempts);

            struct hdhomerun_device_t *capture_hd = hdhomerun_device_create_from_str(config.tuner_id, NULL);
            if (!capture_hd) {
                printf("Failed to create HDHomeRun device for capture\n");
            } else {
                hdhomerun_device_set_tuner(capture_hd, config.tuner_index);
                
                char capture_filename[MAX_FILENAME];
                time_t now = time(NULL);
                struct tm *t = localtime(&now);
                snprintf(capture_filename, sizeof(capture_filename), "rf%u_%s_%04d%02d%02d_%02d%02d%02d.ts",
                         req->channel, config.tuner_id, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                         t->tm_hour, t->tm_min, t->tm_sec);

                bool capture_ok = record_channel_capture(capture_hd, &config, req->channel, req->duration, capture_filename, config.debug);

                if (capture_ok) {
                    bool upload_ok = upload_file_ftp(capture_filename, config.debug);
                    if (upload_ok) {
                         printf("Capture and upload for channel %u complete.\n", req->channel);
                         // Success, remove request from queue by shifting elements
                         pending_capture_count--;
                         for (int i = 0; i < pending_capture_count; i++) {
                             pending_captures[i] = pending_captures[i + 1];
                         }
                         request_removed = true;
                    } else {
                        printf("FTP upload failed for channel %u, will retry capture on next loop.\n", req->channel);
                    }
                } else {
                    printf("Capture failed for channel %u.\n", req->channel);
                }
                hdhomerun_device_destroy(capture_hd);
            }

            // If it was a success, it was already removed. If it failed, check if we've maxed out attempts.
            if (!request_removed && req->attempts >= 5) {
                printf("Giving up on capture for channel %u after 5 failed attempts.\n", req_channel);
                // Remove request from queue
                pending_capture_count--;
                for (int i = 0; i < pending_capture_count; i++) {
                    pending_captures[i] = pending_captures[i + 1];
                }
            }
        } // --- END OF CAPTURE LOGIC ---

        struct hdhomerun_device_t *hd = NULL;
        struct channel_result results[MAX_CHANNELS];
        int result_count = 0;
        
        // Initialize results array properly before each scan
        for (int i = 0; i < MAX_CHANNELS; i++) {
            memset(&results[i], 0, sizeof(results[i]));
            results[i].virtual_channels = NULL;
            results[i].plp_info = NULL;
            results[i].tsid = -999;
            results[i].bsid = -999;
        }
        
        // Create device - simplified approach for compatibility
        hd = hdhomerun_device_create_from_str(config.tuner_id, NULL);
        if (!hd) {
            printf("Failed to create HDHomeRun device with ID %s\n", config.tuner_id);
            printf("Try specifying an exact tuner ID instead of FFFFFFFF\n");
            printf("Use 'hdhomerun_config discover' to find your device ID\n");
            goto retry_loop;
        }
        
        hdhomerun_device_set_tuner(hd, config.tuner_index);

        /* FIX: resolve the wildcard to the actual device ID for user_id.
         * hdhomerun_control_get_device_id() connects (discovering the device
         * if needed) and returns the real ID, or 0 on failure. */
        config.resolved_tuner_id[0] = '\0';
        if (strcasecmp(config.tuner_id, "FFFFFFFF") == 0) {
            uint32_t actual_id = hdhomerun_control_get_device_id(
                                     hdhomerun_device_get_control_sock(hd));
            if (actual_id != 0 && actual_id != 0xFFFFFFFF) {
                snprintf(config.resolved_tuner_id, sizeof(config.resolved_tuner_id),
                         "%08X", (unsigned int)actual_id);
                if (config.debug) {
                    printf("Discovered device ID %s\n", config.resolved_tuner_id);
                }
            } else {
                printf("Warning: could not resolve device ID; user_id will use %s\n",
                       config.tuner_id);
            }
        } else {
            /* Explicit ID: normalize to uppercase to match the Perl's uc() */
            size_t k;
            for (k = 0; k + 1 < sizeof(config.resolved_tuner_id) && config.tuner_id[k]; k++) {
                config.resolved_tuner_id[k] = (char)toupper((unsigned char)config.tuner_id[k]);
            }
            config.resolved_tuner_id[k] = '\0';
        }

        if (config.debug) {
            printf("Connected to tuner %s-%d\n",
                   config.resolved_tuner_id[0] ? config.resolved_tuner_id : config.tuner_id,
                   config.tuner_index);
        }
        
        if (!hd) {
            printf("Failed to create HDHomeRun device\n");
            goto retry_loop;
        }
        
        // Get channel list if using channel map
        unsigned int channels[MAX_CHANNELS];
        int channel_count = 0;
        
        if (config.use_channel_map) {
            char *map_str;
            if (hdhomerun_device_get_tuner_channelmap(hd, &map_str) > 0) {
                char *map_copy = strdup(map_str);
                if (map_copy) {
                    char *token = strtok(map_copy, " ");
                    token = strtok(NULL, " "); // Skip first token (map name)
                    
                    while (token != NULL && channel_count < MAX_CHANNELS) {
                        unsigned int ch = (unsigned int)strtoul(token, NULL, 10);
                        if (ch >= config.min_channel && ch <= config.max_channel) {
                            channels[channel_count++] = ch;
                        }
                        token = strtok(NULL, " ");
                    }
                    free(map_copy);
                }
            }
            
            if (config.debug && channel_count > 0) {
                printf("Using channel map with %d channels\n", channel_count);
            }
        }
        
        // If no channel map or it failed, use sequential range.
        // Scan from max to min channel (top to bottom) as requested.
        if (channel_count == 0) {
            if (config.max_channel >= config.min_channel) {
                for (unsigned int ch = config.max_channel; ; ch--) {
                    channels[channel_count++] = ch;
                    if (ch == config.min_channel) break;
                }
            }
        }
        
        // Scan channels
        printf("Starting scan of %d channels...\n", channel_count);
        
        for (int i = 0; i < channel_count && keep_running; i++) {
            unsigned int channel = channels[i];
            
            if (config.debug) {
                printf("Scanning channel %u (%d/%d)...\n", channel, i + 1, channel_count);
            } else {
                printf("Scanning channel %u...\r", channel);
                fflush(stdout);
            }
            
            // Tune using 'auto' to let the device determine modulation.
            char tune_str[64];
            sprintf(tune_str, "auto:%u", channel);
            if (hdhomerun_device_set_tuner_channel(hd, tune_str) < 0) {
                if (config.debug) printf("Failed to tune to channel %u\n", channel);
                continue;
            }
            
            /* Poll for lock acquisition.
             *
             * hdhomerun_device_wait_for_lock returns immediately when lock_str
             * is "none", which happens during auto-detect cycling between
             * modulations and on truly empty channels. To reliably catch
             * marginal ATSC 3.0 signals (which can take several seconds to
             * lock), we poll get_tuner_status ourselves at 250ms intervals
             * for up to seek_timeout seconds.
             *
             * A bare lock_str like "atsc3" means locked; "(atsc3)" with
             * parentheses means attempting; "none" means nothing detected.
             * We accept the first un-parenthesized non-"none" lock_str.
             */
            struct hdhomerun_tuner_status_t status;
            /* FIX: zero so lock_str is valid even if every status read fails */
            memset(&status, 0, sizeof(status));
            bool has_lock = false;
            int poll_interval_ms = 250;
            int max_polls = (int)(config.seek_timeout * 1000.0 / poll_interval_ms);
            if (max_polls < 1) max_polls = 1;
            
            for (int poll = 0; poll < max_polls; poll++) {
                char *raw_status_str;
                if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
                    /* Locked if lock_str is non-empty, not "none", and not
                     * wrapped in parens (parens indicate "attempting"). */
                    if (strlen(status.lock_str) > 0 &&
                        strcmp(status.lock_str, "none") != 0 &&
                        status.lock_str[0] != '(') {
                        has_lock = true;
                        break;
                    }
                }
                /* Sleep poll_interval_ms */
                usleep(poll_interval_ms * 1000);
            }
            
            if (config.debug && !has_lock) {
                printf("No lock acquired on channel %u after %.1fs (final lock_str: %s)\n",
                       channel, config.seek_timeout,
                       strlen(status.lock_str) > 0 ? status.lock_str : "(empty)");
            }
            
            // Initialize result struct for this channel
            struct channel_result *result = &results[result_count];
            memset(result, 0, sizeof(*result));
            result->rf_channel = channel;
            /* FIX: sane defaults so a failed status read in
             * collect_channel_info() emits "tsid":"" and "virtual":{}
             * rather than "tsid":0 and "virtual":null. */
            result->tsid = -999;
            result->bsid = -999;
            result->virtual_channels = json_object_new_object();
            strncpy(result->modulation, status.lock_str, sizeof(result->modulation) - 1);
            result->modulation[sizeof(result->modulation) - 1] = '\0';

            if (has_lock) {
                bool is_atsc3_lock = (strstr(status.lock_str, "atsc3") != NULL);
                int max_wait = is_atsc3_lock ? config.atsc3_psip_wait_time : config.psip_wait_time;
                int min_wait = is_atsc3_lock ? 1 : config.psip_wait_time;
                
                if (config.debug) {
                    printf("Lock achieved on channel %u (%s), waiting up to %ds for %s...\n",
                           channel, status.lock_str, max_wait,
                           is_atsc3_lock ? "SLT" : "PSIP");
                }
                
                // Poll streaminfo each second; break out as soon as program data appears.
                int waited = 0;
                bool got_programs = false;
                while (waited < max_wait) {
                    sleep(1);
                    waited++;
                    
                    if (waited < min_wait) continue;
                    
                    char *si = NULL;
                    if (hdhomerun_device_get_tuner_streaminfo(hd, &si) > 0 && si) {
                        bool has_programs = false;
                        char *si_copy = strdup(si);
                        if (si_copy) {
                            char *line = strtok(si_copy, "\n");
                            while (line) {
                                while (isspace((unsigned char)*line)) line++;
                                if (strncmp(line, "program ", 8) == 0 ||
                                    strncmp(line, "program=", 8) == 0 ||
                                    (isdigit((unsigned char)*line) && strchr(line, ':'))) {
                                    has_programs = true;
                                    break;
                                }
                                line = strtok(NULL, "\n");
                            }
                            free(si_copy);
                        }
                        if (has_programs) {
                            if (config.debug) printf("Program data appeared after %ds\n", waited);
                            got_programs = true;
                            break;
                        }
                    }
                }
                
                if (config.debug && !got_programs) {
                    printf("Timed out waiting for program data after %ds\n", waited);
                }
                
                // Collect detailed channel info (virtual channels, TSID, etc.).
                // This function gets the latest status internally.
                collect_channel_info(hd, result, &config, config.debug);
                
                // For ATSC 3.0, if we have no virtual channels or see "(no data)",
                // it might mean we only locked one PLP. Try tuning to all PLPs
                // to gather complete information.
                if (result->is_atsc3) {
                    bool has_no_data = false;
                    int virtual_count = 0;
                    if (result->virtual_channels) {
                        json_object_object_foreach(result->virtual_channels, key, val) {
                            (void)key; (void)val;
                            virtual_count++;
                            json_object *name_obj;
                            if (json_object_object_get_ex(val, "name", &name_obj)) {
                                const char *name = json_object_get_string(name_obj);
                                if (name && strstr(name, "(no data)")) {
                                    has_no_data = true;
                                    break;
                                }
                            }
                        }
                    }
                    
                    if (virtual_count == 0 || has_no_data) {
                        if (config.debug) {
                             printf("ATSC 3.0 channel %u has missing data, attempting all-PLP tune.\n", channel);
                        }
                        attempt_atsc3_all_plps(hd, result, &config, config.debug);
                    }
                }
            } else {
                // No lock, but still get the current status to log signal strength etc.
                if (config.debug) printf("No lock on channel %u\n", channel);
                char *raw_status_str;
                if (hdhomerun_device_get_tuner_status(hd, &raw_status_str, &status) > 0) {
                    strncpy(result->modulation, status.lock_str, sizeof(result->modulation) - 1);
                    result->modulation[sizeof(result->modulation) - 1] = '\0';
                    result->signal_strength = status.signal_strength;
                    result->signal_quality = status.signal_to_noise_quality;
                    result->symbol_error_quality = status.symbol_error_quality;
                } else {
                    strcpy(result->modulation, "none");
                }
                /* virtual_channels already initialized to {} above */
                result->tsid = -999;
                result->bsid = -999;
            }
            
            // Copy reporter callsign if configured
            if (strlen(config.override_callsign) > 0) {
                strncpy(result->reporter_callsign, config.override_callsign,
                       sizeof(result->reporter_callsign) - 1);
            }

            // Per-channel override (Perl -o equivalent). Applied last so it
            // takes precedence over the global override_callsign for matched
            // RF channels only.
            for (int oi = 0; oi < config.num_channel_overrides; oi++) {
                if (config.channel_overrides[oi].channel == channel) {
                    strncpy(result->reporter_callsign,
                            config.channel_overrides[oi].callsign,
                            sizeof(result->reporter_callsign) - 1);
                    result->reporter_callsign[sizeof(result->reporter_callsign) - 1] = '\0';
                    break;
                }
            }
                
            result_count++;
            
            if (config.debug) {
                printf("Channel %u: %s, Signal: %d%%, SNR: %d%%, SER: %d%%",
                       channel, result->modulation, result->signal_strength,
                       result->signal_quality, result->symbol_error_quality);
                
                if (result->tsid != -999) {
                    printf(", TSID: %ld", result->tsid);
                }
                /* FIX: show the callsign override being sent, if any */
                if (result->reporter_callsign[0] != '\0') {
                    printf(", reporter_callsign: %s", result->reporter_callsign);
                }
                printf("\n");
            }
        }
        
        printf("\nScan complete. Found %d channels with signals.\n", result_count);
        
        // Submit results if we found anything
        if (result_count > 0) {
            json_object *scan_json = create_scan_json(&config, results, result_count);
            
            if (config.debug) {
                const char *json_str = json_object_to_json_string_ext(scan_json, JSON_C_TO_STRING_PRETTY);
                printf("JSON to submit:\n%s\n", json_str);
            }
            
            // REVERTED to plain JSON for stability
            // if (strlen(json_object_to_json_string(scan_json)) > 500) {
                struct http_response server_response = submit_results(config.spot_url, scan_json, config.debug);
                if (server_response.data) {
                    if (config.debug) printf("Server response: %s\n", server_response.data);
                    // NEW: Parse response for capture requests
                    parse_capture_requests(server_response.data, config.debug);
                    free(server_response.data); // Free the data after parsing
                } else {
                    printf("Failed to submit scan results\n");
                }
            // } else {
            //    printf("Scan data too short, not submitting\n");
            // }
            
            // The scan_json object now owns all the subsidiary json objects.
            // Freeing it will free everything correctly.
            json_object_put(scan_json);
        } else {
            printf("No channels found in scan!\n");
        }
        
        if (hd) {
            hdhomerun_device_destroy(hd);
        }
        
retry_loop:
        if (keep_running) {
            printf("Waiting %d seconds before next scan...\n", config.retry_delay);
            for (int i = 0; i < config.retry_delay && keep_running; i++) {
                sleep(1);
            }
        }
    }
    
    // Cleanup
    curl_global_cleanup();
    printf("Scanner shutdown complete\n");
    
    return 0;
}
