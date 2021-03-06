/*
 * Copyright (c) 2001-2016 Mellanox Technologies, Ltd. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include "vlogger.h"

#include <sys/types.h>
#include <sys/syscall.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>

#include "utils/bullseye.h"
#include "vma/util/utils.h"
#include "vma/util/sys_vars.h"

#define VLOG_DEFAULT_MODULE_NAME "VMA"
#define VMA_LOG_CB_ENV_VAR "VMA_LOG_CB_FUNC_PTR"

char         g_vlogger_module_name[VLOG_MODULE_MAX_LEN] = VLOG_DEFAULT_MODULE_NAME;
int          g_vlogger_fd = -1;
FILE*        g_vlogger_file = NULL;
vlog_levels_t g_vlogger_level = VLOG_DEFAULT;
vlog_levels_t*     g_p_vlogger_level = NULL;
uint8_t      g_vlogger_details = 0;
uint8_t*     g_p_vlogger_details = NULL;
uint32_t     g_vlogger_usec_on_startup = 0;
bool         g_vlogger_log_in_colors = MCE_DEFAULT_LOG_COLORS;
vma_log_cb_t g_vlogger_cb = NULL;

namespace log_level
{
	typedef struct {
		vlog_levels_t level;
		const char *  output_name;
		const char *  output_color;
		const char ** input_names;
	} level_names;

	static const char *log_names_none[]    = {"none", NULL};
	static const char *log_names_panic[]   = {"panic", "0", NULL};
	static const char *log_names_error[]   = {"error", "1", NULL};
	static const char *log_names_warn[]    = {"warn",  "warning", "2", NULL};
	static const char *log_names_info[]    = {"info",  "information", "3", NULL};
	static const char *log_names_details[] = {"details", NULL};
	static const char *log_names_debug[]   = {"debug", "4", NULL};
	static const char *log_names_fine[]    = {"fine",  "func", "5", NULL};
	static const char *log_names_finer[]   = {"finer", "func+", "funcall", "func_all", "func-all", "6", NULL};
	static const char *log_names_all[]     = {"all", NULL};

	// must be by order because "to_str" relies on that!
	static const level_names levels[] = {
			{VLOG_NONE,    "NONE",    "\e[0;31m" /*Red*/,     (const char ** )log_names_none},
			{VLOG_PANIC,   "PANIC",   "\e[0;31m" /*Red*/,     (const char ** )log_names_panic},
			{VLOG_ERROR,   "ERROR",   "\e[0;31m" /*Red*/,     (const char ** )log_names_error},
			{VLOG_WARNING, "WARNING", "\e[2;35m" /*Magenta*/, (const char ** )log_names_warn},
			{VLOG_INFO,    "INFO",    "\e[0m"    /*Default*/, (const char ** )log_names_info},
			{VLOG_DETAILS, "DETAILS", "\e[0m"    /*Default*/, (const char ** )log_names_details},
			{VLOG_DEBUG,   "DEBUG",   "\e[0m"    /*Default*/, (const char ** )log_names_debug},
			{VLOG_FINE,    "FINE",    "\e[2m"    /*Grey*/,    (const char ** )log_names_fine},
			{VLOG_FINER,   "FINER",   "\e[2m"    /*Grey*/,    (const char ** )log_names_finer},
			{VLOG_ALL,     "ALL",     "\e[2m"    /*Grey*/,    (const char ** )log_names_all},
	};

	// convert str to vlog_levels_t; upon error - returns the given 'def_value'
	vlog_levels_t from_str(const char* str, vlog_levels_t def_value)
	{
		size_t num_levels = sizeof(levels) / sizeof(levels[0]);
		for (size_t i = 0; i < num_levels; ++i) {
			const char ** input_name = levels[i].input_names;
			while (*input_name) {
				if (strcasecmp(str, *input_name) == 0)
					return levels[i].level;
				input_name++;
			}
		}

		return def_value; // not found. use given def_value
	}

	const char * to_str(vlog_levels_t level)
	{
		static int base = VLOG_NONE;
		return levels[level - base].output_name;
	}

	const char * get_color(vlog_levels_t level)
	{
		static int base = VLOG_NONE;
		return levels[level - base].output_color;
	}
}

pid_t gettid(void)
{
	return syscall(__NR_gettid);
}

// Credit for the C++ de-mangler go to: http://tombarta.wordpress.com/2008/08/01/c-stack-traces-with-gcc/
#include <cxxabi.h>
void printf_backtrace(void)
{
	char **backtrace_strings;
	void* backtrace_addrs[10];
	int backtrace_depth = backtrace(backtrace_addrs, 10);
	printf("[tid: %d] ------ printf_backtrace ------ \n", gettid());
	backtrace_strings = backtrace_symbols(backtrace_addrs, backtrace_depth);
	for (int i = 1; i < backtrace_depth; i++) {
#if 0
		printf("[%d] %p: %s\n", i, backtrace_addrs[i], backtrace_strings[i]);
#else
		size_t sz = 1024; // just a guess, template names will go much wider
		char *function = static_cast<char*>(malloc(sz));
		char *begin = 0, *end = 0;
		// find the parentheses and address offset surrounding the mangled name
		for (char *j = backtrace_strings[i]; *j; ++j) {
			if (*j == '(') {
				begin = j;
			}
			else if (*j == '+') {
				end = j;
			}
		}
		if (begin && end) {
			*begin++ = '\0';
			*end = '\0';
			// found our mangled name, now in [begin, end)

			int status;
			char *ret = abi::__cxa_demangle(begin, function, &sz, &status);
			if (ret) {
				// return value may be a realloc() of the input
				function = ret;
			}
			else {
				// demangling failed, just pretend it's a C function with no args
				strncpy(function, begin, sz);
				strncat(function, "()", sz);
				function[sz-1] = '\0';
			}
			//	        fprintf(out, "    %s:%s\n", stack.backtrace_strings[i], function);
			printf("[%d] %p: %s:%s\n", i, backtrace_addrs[i], backtrace_strings[i], function);
		}
		else
		{
			// didn't find the mangled name, just print the whole line
			printf("[%d] %p: %s\n", i, backtrace_addrs[i], backtrace_strings[i]);
		}
		free(function);
#endif
	}
	free(backtrace_strings);
}

////////////////////////////////////////////////////////////////////////////////
// NOTE: this function matches 'bool vma_log_set_cb_func(vma_log_cb_t log_cb)' that
// we gave customers; hence, you must not change our side without considering their side
static vma_log_cb_t vma_log_get_cb_func()
{
	vma_log_cb_t log_cb = NULL;
	const char* const CB_STR = getenv(VMA_LOG_CB_ENV_VAR);
	if (!CB_STR || !*CB_STR) return NULL;

	if (1 != sscanf(CB_STR, "%p", &log_cb)) return NULL;
	return log_cb;
}

void vlog_start(const char* log_module_name, vlog_levels_t log_level, const char* log_filename, int log_details, bool log_in_colors)
{
	g_vlogger_file = stderr;

	g_vlogger_cb = vma_log_get_cb_func();

	strncpy(g_vlogger_module_name, log_module_name, sizeof(g_vlogger_module_name) - 1);
	g_vlogger_module_name[sizeof(g_vlogger_module_name) - 1] = '\0';

	vlog_get_usec_since_start();

	char local_log_filename[255];
	if (log_filename != NULL && strcmp(log_filename,"")) {
		sprintf(local_log_filename, "%s", log_filename);
		g_vlogger_fd = open(local_log_filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
		if (g_vlogger_fd < 0) {
			vlog_printf(VLOG_PANIC, "Failed to open logfile: %s\n",local_log_filename);
			exit(1);
		}
		g_vlogger_file = fdopen(g_vlogger_fd, "w");

		BULLSEYE_EXCLUDE_BLOCK_START
		if (g_vlogger_file == NULL) {
			g_vlogger_file = stderr;
			vlog_printf(VLOG_PANIC, "Failed to open logfile: %s\n",local_log_filename);
			exit(1);
		}
		BULLSEYE_EXCLUDE_BLOCK_END
	}

	g_vlogger_level = log_level;
	g_p_vlogger_level = &g_vlogger_level;
	g_vlogger_details = log_details;
	g_p_vlogger_details = &g_vlogger_details;


	int file_fd = fileno(g_vlogger_file);
	if (file_fd >= 0 && isatty(file_fd) && log_in_colors)
		g_vlogger_log_in_colors = log_in_colors;
}

void vlog_stop(void)
{
	// Closing logger

	// Allow only really extreme (PANIC) logs to go out
	g_vlogger_level = VLOG_PANIC;
	
	//set default module name
	strcpy(g_vlogger_module_name, VLOG_DEFAULT_MODULE_NAME);

	// Close output stream
	if(g_vlogger_file && g_vlogger_file != stderr)
		fclose(g_vlogger_file);

	//fix for using LD_PRELOAD with LBM. Unset the pointer given by the parent process, so a child could get his own pointer without issues.
	unsetenv(VMA_LOG_CB_ENV_VAR);
}

const tscval_t LogDuration::TSC_RATE_PER_USEC = get_tsc_rate_per_second() / 1000 / 1000;

LogDuration::LogDuration(const char * label, vlog_levels_t log_level) : m_label(label), m_log_level(log_level), m_printCounter(0)
{
	gettimeoftsc(&m_startTime);
	gettimeoftsc(&m_lastPrint);
}

void LogDuration::print()
{
	tscval_t currTime;
	gettimeoftsc(&currTime);

	tscval_t duration = (currTime - m_lastPrint) / TSC_RATE_PER_USEC;
	vlog_printf(m_log_level, "\t [%2u] >> LogDuration=%llu usec label=%s\n", m_printCounter++, (unsigned long long)duration, m_label);
	m_lastPrint = currTime;
}

LogDuration::~LogDuration()
{
	tscval_t endTime;
	gettimeoftsc(&endTime);

	tscval_t duration = (endTime - m_startTime) / TSC_RATE_PER_USEC;
	vlog_printf(m_log_level, " >> LogDuration=%llu usec label=%s\n", (unsigned long long)duration, m_label);
}
