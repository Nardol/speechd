/*
 * module_process.c - Processing loop of output modules.
 *
 * Copyright (C) 2020-2021 Samuel Thibault <samuel.thibault@ens-lyon.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Samuel Thibault AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>

#include "module_main.h"

pthread_mutex_t module_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

/* This sends some text to the server, taking the mutex to avoid intermixing
 * between multi-line answers and asynchronous sends.  */
void module_send(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	pthread_mutex_lock(&module_stdout_mutex);
	vprintf(format, ap);
	pthread_mutex_unlock(&module_stdout_mutex);
	va_end(ap);
	fflush(stdout);
}

/*
 * This only parses the SSIP protocol from the server, and calls the
 * corresponding functions provided by the module or by module_utils.c
 */

#define print(fmt, ...) module_send(fmt "\n", ## __VA_ARGS__)

#define BAD_SYNTAX "302 ERROR BAD SYNTAX"
#define BAD_PARAM "303 ERROR INVALID PARAMETER OR VALUE"
#define BAD_MULTILINE "305 DATA MORE THAN ONE LINE"

#define bad_syntax() print(BAD_SYNTAX)
#define bad_param() print(BAD_PARAM)
#define bad_multiline() print(BAD_MULTILINE)

#define bad_internal() print("401 ERROR INTERNAL")
#define bad_memory() print("402 ERROR OUT OF MEMORY")

/* some text
 * at will
 * .
 */
static void cmd_speak(int fd, SPDMessageType msgtype)
{
	size_t text_allocated = 128, new_allocated;
	char  *text = malloc(text_allocated), *new_text;
	size_t text_len = 0;
	size_t len;
	int ret;
	int nlines = 0;

	print("202 OK RECEIVING MESSAGE");

	while (1) {
		char *line = module_readline(fd, 1);
		int offset = 0;
		if (!line) {
			/* EOF */
			free(text);
			return;
		}

		if (!strcmp(line, ".\n")) {
			/* Replace \n at the end with a \0 */
			if (text_len) {
				text_len--;
				text[text_len] = 0;
			}
			free(line);
			break;
		}

		if (line[0] == '.') {
			offset++;
			line++;
		}

		nlines++;
		len = strlen(line);

		new_allocated = text_allocated;
		while (text_len + len > new_allocated)
			new_allocated *= 2;

		if (new_allocated > text_allocated) {
			new_text = realloc(text, new_allocated);
			if (!new_text) {
				free(line);
				free(text);
				bad_internal();
				return;
			}
			text = new_text;
			text_allocated = new_allocated;
		}
		memcpy(text + text_len, line, len);
		text_len += len;
		free(line - offset);
	}

	if (!text_len) {
		free(text);
		print("301 ERROR CANT SPEAK");
		return;
	}

	if (msgtype != SPD_MSGTYPE_TEXT && nlines > 1) {
		free(text);
		print("305 DATA MORE THAN ONE LINE");
		return;
	}

	if (msgtype == SPD_MSGTYPE_CHAR) {
		if (!strcmp(text, "space")) {
			free(text);
			text = strdup(" ");
		}
	}

#pragma weak module_speak_sync
#pragma weak module_speak
	if (module_speak_sync) {
		print("200 OK SPEAKING");
		ret = module_speak_sync(text, text_len, msgtype);
	} else {
		ret = module_speak(text, text_len, msgtype);
		if (ret > 0)
			print("200 OK SPEAKING");
		else
			print("301 ERROR CANT SPEAK");
	}
	free(text);
}

static void cmd_speak_text(int fd)
{
	return cmd_speak(fd, SPD_MSGTYPE_TEXT);
}

static void cmd_speak_sound_icon(int fd)
{
	return cmd_speak(fd, SPD_MSGTYPE_SOUND_ICON);
}

static void cmd_speak_char(int fd)
{
	return cmd_speak(fd, SPD_MSGTYPE_CHAR);
}

static void cmd_speak_key(int fd)
{
	return cmd_speak(fd, SPD_MSGTYPE_KEY);
}

static void cmd_stop(void)
{
	module_stop();
}

static void cmd_pause(void)
{
	module_pause();
}

static void cmd_list_voices(void)
{
	SPDVoice **voices, **voice;
	int one = 0;;

	voices = module_list_voices();
	if (!voices) {
		print("304 CANT LIST VOICES");
		return;
	}

	pthread_mutex_lock(&module_stdout_mutex);
	for (voice = voices; *voice; voice++) {
		const char *name = (*voice)->name;
		const char *language = (*voice)->language;
		const char *variant = (*voice)->variant;

		if (!name)
			/* Ok, skip this */
			continue;

		one = 1;
		if (!language)
			language = "none";
		if (!variant)
			variant = "none";

		printf("200-%s\t%s\t%s\n", name, language, variant);
	}
	if (one)
		printf("200 OK VOICE LIST SENT\n");
	else
		printf("304 CANT LIST VOICES\n");
	pthread_mutex_unlock(&module_stdout_mutex);
	fflush(stdout);
}

/* FOO1=bar1
 * FOO2=bar2
 * .
 */
static int cmd_params(int fd, const char *type, int (*set)(const char *var, const char *val))
{
	char *var, *val, *save;
	const char *err = NULL;

	print("207 OK RECEIVING %sSETTINGS", type);

	while (1) {
		char *line = module_readline(fd, 1);
		if (!line)
			/* EOF */
			return -1;

		if (!strcmp(line, ".\n")) {
			free(line);

			if (!err)
				return 0;
			print("%s", err);
			return -1;
		}

		save = NULL;
		var = strtok_r(line, "=", &save);
		if (!var) {
			err = BAD_SYNTAX;
			free(line);
			continue;
		}

		val = strtok_r(NULL, "\n", &save);
		if (!val) {
			err = BAD_SYNTAX;
			free(line);
			continue;
		}

		if (set(var, val) != 0)
			err = BAD_PARAM;

		free(line);
	}
}

static void cmd_set(int fd)
{
	if (cmd_params(fd, "", module_set) != 0)
		return;

	print("203 OK SETTINGS RECEIVED");
}

static void cmd_audio(int fd)
{
	char *status = NULL;
	int ret;

	if (cmd_params(fd, "AUDIO ", module_audio_set) != 0)
		return;

	ret = module_audio_init(&status);

	if (!ret)
		print("203 OK AUDIO INITIALIZED");
	else
		print("300-%s\n300 MODULE ERROR", status);

	free(status);
}

static void cmd_loglevel(int fd)
{
	if (cmd_params(fd, "LOGLEVEL ", module_loglevel_set) != 0)
		return;
	print("203 OK LOGLEVEL SET");
}

/* DEBUG ON /some/file
 * or
 * DEBUG OFF
 */
static void cmd_debug(char *line)
{
	char *debug, *on, *file = NULL, *save = NULL;
	int enable = 0;

	debug = strtok_r(line, " \n", &save);
	if (strcmp(debug, "DEBUG")) {
		bad_internal();
		return;
	}

	on = strtok_r(NULL, " \n", &save);
	if (!on) {
		bad_syntax();
		return;
	}

	if (!strcmp(on, "ON")) {
		enable = 1;
		file = strtok_r(NULL, " \n", &save);
		if (!file) {
			bad_syntax();
			return;
		}
	} else if (strcmp(on, "OFF")) {
		bad_syntax();
		return;
	}

	if (module_debug(enable, file) != 0)
		print("303 CANT OPEN CUSTOM DEBUG FILE");
	else
		print("200 OK DEBUGGING %s", on);
}

static void cmd_quit(void)
{
	module_close();

	print("210 OK QUIT");
}

int module_process(int fd, int block)
{
	while (1) {
		char *line = module_readline(fd, block);
		if (line == NULL)
			return -1;

		if (!strcmp(line, "SPEAK\n"))
			cmd_speak_text(fd);

		else if (!strcmp(line, "SOUND_ICON\n"))
			cmd_speak_sound_icon(fd);

		else if (!strcmp(line, "CHAR\n"))
			cmd_speak_char(fd);

		else if (!strcmp(line, "KEY\n"))
			cmd_speak_key(fd);

		else if (!strcmp(line, "STOP\n"))
			cmd_stop();

		else if (!strcmp(line, "PAUSE\n"))
			cmd_pause();

		else if (!strcmp(line, "LIST VOICES\n"))
			cmd_list_voices();

		else if (!strcmp(line, "SET\n"))
			cmd_set(fd);

		else if (!strcmp(line, "AUDIO\n"))
			cmd_audio(fd);

		else if (!strcmp(line, "LOGLEVEL\n"))
			cmd_loglevel(fd);

		else if (!strncmp(line, "DEBUG", 5))
			cmd_debug(line);

		else if (!strcmp(line, "QUIT\n")) {
			cmd_quit();
			fflush(stdout);
			return 0;
		}

		else
			print("300 ERR UNKNOWN COMMAND");

		fflush(stdout);
	}
}

/* Report index */
void module_report_index_mark(char *mark)
{
	if (!mark)
		return;

	print("700-%s\n700 INDEX MARK\n", mark);
}

/* Report speak start */
void module_report_event_begin(void)
{
	print("701 BEGIN");
}

/* Report speak end */
void module_report_event_end(void)
{
	print("702 END");
}

/* Report speak stop */
void module_report_event_stop(void)
{
	print("703 STOP");
}

/* Report speak pause */
void module_report_event_pause(void)
{
	print("704 PAUSE");
}