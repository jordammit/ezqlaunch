/*
	Copyright (C) 2004 Cory Nelson - rewritten to use plain char* (no TCHAR)
*/

#ifndef __CONF_H__
#define __CONF_H__

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct __confnode {
	char *key;
	char *value;
	struct __confnode *next;
} CONFNODE;

typedef struct __conf {
	CONFNODE *first, *last;
} CONF;

CONF *ConfCreate(void);
void ConfDestroy(CONF *conf);

const char *ConfGetOpt(const CONF *conf, const char *key);
void ConfSetOpt(CONF *conf, const char *key, const char *value);

BOOL ConfLoad(CONF *conf, const char *file);
BOOL ConfSave(const CONF *conf, const char *file);

#endif
