/*
 * vim:ts=8:expandtab
 *
 * i3status – Generates a status line for dzen2 or xmobar
 *
 * Copyright © 2008-2011 Michael Stapelberg and contributors
 * Copyright © 2009 Thorsten Toepper <atsutane at freethoughts dot de>
 * Copyright © 2010 Axel Wagner <mail at merovius dot de>
 * Copyright © 2010 Fernando Tarlá Cardoso Lemos <fernandotcl at gmail dot com>
 *
 * See file LICENSE for license information.
 *
 */
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <getopt.h>
#include <signal.h>
#include <confuse.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>

#include "i3status.h"

#define exit_if_null(pointer, ...) { if (pointer == NULL) die(__VA_ARGS__); }

/* socket file descriptor for general purposes */
int general_socket;

cfg_t *cfg, *cfg_general;

/*
 * Exit upon SIGPIPE because when we have nowhere to write to, gathering
 * system information is pointless.
 *
 */
void sigpipe(int signum) {
        fprintf(stderr, "Received SIGPIPE, exiting\n");
        exit(1);
}

/*
 * Checks if the given path exists by calling stat().
 *
 */
static bool path_exists(const char *path) {
        struct stat buf;
        return (stat(path, &buf) == 0);
}

static void *scalloc(size_t size) {
        void *result = calloc(size, 1);
        exit_if_null(result, "Error: out of memory (calloc(%zd))\n", size);
        return result;
}

static char *sstrdup(const char *str) {
        char *result = strdup(str);
        exit_if_null(result, "Error: out of memory (strdup())\n");
        return result;
}


/*
 * Validates a color in "#RRGGBB" format
 *
 */
static int valid_color(const char *value)
{
        if (strlen(value) != 7) return 0;
        if (value[0] != '#') return 0;
        for (int i = 1; i < 7; ++i) {
                if (value[i] >= '0' && value[i] <= '9') continue;
                if (value[i] >= 'a' && value[i] <= 'f') continue;
                if (value[i] >= 'A' && value[i] <= 'F') continue;
                return 0;
        }
        return 1;
}

/*
 * This function resolves ~ in pathnames.
 * It may resolve wildcards in the first part of the path, but if no match
 * or multiple matches are found, it just returns a copy of path as given.
 *
 */
static char *resolve_tilde(const char *path) {
        static glob_t globbuf;
        char *head, *tail, *result = NULL;

        tail = strchr(path, '/');
        head = strndup(path, tail ? (size_t)(tail - path) : strlen(path));

        int res = glob(head, GLOB_TILDE, NULL, &globbuf);
        free(head);
        /* no match, or many wildcard matches are bad */
        if (res == GLOB_NOMATCH || globbuf.gl_pathc != 1)
                result = sstrdup(path);
        else if (res != 0) {
                die("glob() failed");
        } else {
                head = globbuf.gl_pathv[0];
                result = scalloc(strlen(head) + (tail ? strlen(tail) : 0) + 1);
                strncpy(result, head, strlen(head));
                strncat(result, tail, strlen(tail));
        }
        globfree(&globbuf);

        return result;
}

static char *get_config_path() {
        char *xdg_config_home, *xdg_config_dirs, *config_path;

        /* 1: check the traditional path under the home directory */
        config_path = resolve_tilde("~/.i3status.conf");
        if (path_exists(config_path))
                return config_path;

        /* 2: check for $XDG_CONFIG_HOME/i3status/config */
        if ((xdg_config_home = getenv("XDG_CONFIG_HOME")) == NULL)
                xdg_config_home = "~/.config";

        xdg_config_home = resolve_tilde(xdg_config_home);
        if (asprintf(&config_path, "%s/i3status/config", xdg_config_home) == -1)
                die("asprintf() failed");
        free(xdg_config_home);

        if (path_exists(config_path))
                return config_path;
        free(config_path);

        /* 3: check the traditional path under /etc */
        config_path = SYSCONFDIR "/i3status.conf";
        if (path_exists(config_path))
            return sstrdup(config_path);

        /* 4: check for $XDG_CONFIG_DIRS/i3status/config */
        if ((xdg_config_dirs = getenv("XDG_CONFIG_DIRS")) == NULL)
                xdg_config_dirs = "/etc/xdg";

        char *buf = strdup(xdg_config_dirs);
        char *tok = strtok(buf, ":");
        while (tok != NULL) {
                tok = resolve_tilde(tok);
                if (asprintf(&config_path, "%s/i3status/config", tok) == -1)
                        die("asprintf() failed");
                free(tok);
                if (path_exists(config_path)) {
                        free(buf);
                        return config_path;
                }
                free(config_path);
                tok = strtok(NULL, ":");
        }
        free(buf);

        die("Unable to find the configuration file (looked at "
                "~/.i3status/config, $XDG_CONFIG_HOME/i3status/config, "
                "/etc/i3status/config and $XDG_CONFIG_DIRS/i3status/config)");
        return NULL;
}

int main(int argc, char *argv[]) {
        unsigned int j;

        cfg_opt_t general_opts[] = {
                CFG_STR("output_format", "auto", CFGF_NONE),
                CFG_BOOL("colors", 1, CFGF_NONE),
                CFG_STR("color_good", "#00FF00", CFGF_NONE),
                CFG_STR("color_degraded", "#FFFF00", CFGF_NONE),
                CFG_STR("color_bad", "#FF0000", CFGF_NONE),
                CFG_STR("color_separator", "#333333", CFGF_NONE),
                CFG_INT("interval", 1, CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t run_watch_opts[] = {
                CFG_STR("pidfile", NULL, CFGF_NONE),
                CFG_STR("format", "%title: %status", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t wireless_opts[] = {
                CFG_STR("format_up", "W: (%quality at %essid, %bitrate) %ip", CFGF_NONE),
                CFG_STR("format_down", "W: down", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t ethernet_opts[] = {
                CFG_STR("format_up", "E: %ip (%speed)", CFGF_NONE),
                CFG_STR("format_down", "E: down", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t ipv6_opts[] = {
                CFG_STR("format_up", "%ip", CFGF_NONE),
                CFG_STR("format_down", "no IPv6", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t battery_opts[] = {
                CFG_STR("format", "%status %percentage %remaining", CFGF_NONE),
                CFG_BOOL("last_full_capacity", false, CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t time_opts[] = {
                CFG_STR("format", "%d.%m.%Y %H:%M:%S", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t ddate_opts[] = {
                CFG_STR("format", "%{%a, %b %d%}, %Y%N - %H", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t load_opts[] = {
                CFG_STR("format", "%1min %5min %15min", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t usage_opts[] = {
                CFG_STR("format", "%usage", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t temp_opts[] = {
                CFG_STR("format", "%degrees C", CFGF_NONE),
                CFG_STR("path", NULL, CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t disk_opts[] = {
                CFG_STR("format", "%free", CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t volume_opts[] = {
                CFG_STR("format", "♪: %volume", CFGF_NONE),
                CFG_STR("device", "default", CFGF_NONE),
                CFG_STR("mixer", "Master", CFGF_NONE),
                CFG_INT("mixer_idx", 0, CFGF_NONE),
                CFG_END()
        };

        cfg_opt_t opts[] = {
                CFG_STR_LIST("order", "{}", CFGF_NONE),
                CFG_SEC("general", general_opts, CFGF_NONE),
                CFG_SEC("run_watch", run_watch_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("wireless", wireless_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("ethernet", ethernet_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("battery", battery_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("cpu_temperature", temp_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("disk", disk_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("volume", volume_opts, CFGF_TITLE | CFGF_MULTI),
                CFG_SEC("ipv6", ipv6_opts, CFGF_NONE),
                CFG_SEC("time", time_opts, CFGF_NONE),
                CFG_SEC("ddate", ddate_opts, CFGF_NONE),
                CFG_SEC("load", load_opts, CFGF_NONE),
                CFG_SEC("cpu_usage", usage_opts, CFGF_NONE),
                CFG_END()
        };

        char *configfile = NULL;
        int o, option_index = 0;
        struct option long_options[] = {
                {"config", required_argument, 0, 'c'},
                {"help", no_argument, 0, 'h'},
                {"version", no_argument, 0, 'v'},
                {0, 0, 0, 0}
        };

        struct sigaction action;
        memset(&action, 0, sizeof(struct sigaction));
        action.sa_handler = sigpipe;
        sigaction(SIGPIPE, &action, NULL);

        if (setlocale(LC_ALL, "") == NULL)
                die("Could not set locale. Please make sure all your LC_* / LANG settings are correct.");

        while ((o = getopt_long(argc, argv, "c:hv", long_options, &option_index)) != -1)
                if ((char)o == 'c')
                        configfile = optarg;
                else if ((char)o == 'h') {
                        printf("i3status " VERSION " © 2008-2011 Michael Stapelberg and contributors\n"
                                "Syntax: %s [-c <configfile>] [-h] [-v]\n", argv[0]);
                        return 0;
                } else if ((char)o == 'v') {
                        printf("i3status " VERSION " © 2008-2011 Michael Stapelberg and contributors\n");
                        return 0;
                }


        if (configfile == NULL)
                configfile = get_config_path();

        cfg = cfg_init(opts, CFGF_NOCASE);
        if (cfg_parse(cfg, configfile) == CFG_PARSE_ERROR)
                return EXIT_FAILURE;

        if (cfg_size(cfg, "order") == 0)
                die("Your 'order' array is empty. Please fix your config.\n");

        cfg_general = cfg_getsec(cfg, "general");
        if (cfg_general == NULL)
                die("Could not get section \"general\"\n");

        char *output_str = cfg_getstr(cfg_general, "output_format");
        if (strcasecmp(output_str, "auto") == 0) {
                fprintf(stderr, "i3status: trying to auto-detect output_format setting\n");
                output_str = auto_detect_format();
                if (!output_str) {
                        output_str = "none";
                        fprintf(stderr, "i3status: falling back to \"none\"\n");
                } else {
                        fprintf(stderr, "i3status: auto-detected \"%s\"\n", output_str);
                }
        }

        if (strcasecmp(output_str, "dzen2") == 0)
                output_format = O_DZEN2;
        else if (strcasecmp(output_str, "xmobar") == 0)
                output_format = O_XMOBAR;
        else if (strcasecmp(output_str, "none") == 0)
                output_format = O_NONE;
        else die("Unknown output format: \"%s\"\n", output_str);

        if (!valid_color(cfg_getstr(cfg_general, "color_good"))
                        || !valid_color(cfg_getstr(cfg_general, "color_degraded"))
                        || !valid_color(cfg_getstr(cfg_general, "color_bad"))
                        || !valid_color(cfg_getstr(cfg_general, "color_separator")))
               die("Bad color format");

        if ((general_socket = socket(AF_INET, SOCK_DGRAM, 0)) == -1)
                die("Could not create socket\n");

        int interval = cfg_getint(cfg_general, "interval");

        struct tm tm;
        while (1) {
                struct timeval tv;
                gettimeofday(&tv, NULL);
                time_t current_time = tv.tv_sec;
                struct tm *current_tm = NULL;
                if (current_time != (time_t) -1) {
                        localtime_r(&current_time, &tm);
                        current_tm = &tm;
                }
                for (j = 0; j < cfg_size(cfg, "order"); j++) {
                        if (j > 0)
                                print_seperator();

                        const char *current = cfg_getnstr(cfg, "order", j);

                        CASE_SEC("ipv6")
                                print_ipv6_info(cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));

                        CASE_SEC_TITLE("wireless")
                                print_wireless_info(title, cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));

                        CASE_SEC_TITLE("ethernet")
                                print_eth_info(title, cfg_getstr(sec, "format_up"), cfg_getstr(sec, "format_down"));

                        CASE_SEC_TITLE("battery")
                                print_battery_info(atoi(title), cfg_getstr(sec, "format"), cfg_getbool(sec, "last_full_capacity"));

                        CASE_SEC_TITLE("run_watch")
                                print_run_watch(title, cfg_getstr(sec, "pidfile"), cfg_getstr(sec, "format"));

                        CASE_SEC_TITLE("disk")
                                print_disk_info(title, cfg_getstr(sec, "format"));

                        CASE_SEC("load")
                                print_load(cfg_getstr(sec, "format"));

                        CASE_SEC("time")
                                print_time(cfg_getstr(sec, "format"), current_tm);

                        CASE_SEC("ddate")
                                print_ddate(cfg_getstr(sec, "format"), current_tm);

                        CASE_SEC_TITLE("volume")
                                print_volume(cfg_getstr(sec, "format"),
                                             cfg_getstr(sec, "device"),
                                             cfg_getstr(sec, "mixer"),
                                             cfg_getint(sec, "mixer_idx"));

                        CASE_SEC_TITLE("cpu_temperature")
                                print_cpu_temperature_info(atoi(title), cfg_getstr(sec, "path"), cfg_getstr(sec, "format"));

                        CASE_SEC("cpu_usage")
                                print_cpu_usage(cfg_getstr(sec, "format"));
                }
                printf("\n");
                fflush(stdout);

                /* To provide updates on every full second (as good as possible)
                 * we don’t use sleep(interval) but we sleep until the next
                 * second (with microsecond precision) plus (interval-1)
                 * seconds. */
                struct timeval current_timeval;
                gettimeofday(&current_timeval, NULL);
                struct timespec ts = {interval - 1, (10e5 - current_timeval.tv_usec) * 1000};
                nanosleep(&ts, NULL);
        }
}
