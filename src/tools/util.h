#ifndef UTIL_H
#define UTIL_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#ifndef _WIN32
#include <unistd.h>
#include <strings.h>
#endif
#include <sys/stat.h>
#include <opensc/opensc.h>

#ifdef  __cplusplus
extern "C" {
#endif

extern const struct option options[];
extern const char *option_help[];
extern const char *app_name;

void print_binary(FILE *f, const u8 *buf, int count);
void hex_dump(FILE *f, const u8 *in, int len, const char *sep);
void hex_dump_asc(FILE *f, const u8 *in, size_t count, int addr);
void print_usage_and_die(void);
const char * acl_to_str(const struct sc_acl_entry *e);
void warn(const char *fmt, ...);
void error(const char *fmt, ...);
void fatal(const char *fmt, ...);

#ifdef  __cplusplus
}
#endif

#endif
