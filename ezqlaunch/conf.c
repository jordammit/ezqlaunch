/*
	Copyright (C) 2004 Cory Nelson - rewritten to use plain char* (no TCHAR)
*/
#define snprintf _snprintf
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "conf.h"

CONF *ConfCreate(void) {
	return (CONF*)calloc(1, sizeof(CONF));
}

void ConfDestroy(CONF *conf) {
	CONFNODE *c, *next;
	for(c = conf->first; c != NULL; c = next) {
		if(c->key)   free(c->key);
		if(c->value) free(c->value);
		next = c->next;
		free(c);
	}
	free(conf);
}

const char *ConfGetOpt(const CONF *conf, const char *key) {
	const CONFNODE *c;
	for(c = conf->first; c != NULL; c = c->next)
		if(strcmp(c->key, key) == 0) return c->value;
	return NULL;
}

/* Duplicate a string, trimming leading and trailing whitespace. */
static char *strdup_trim(const char *str) {
	const char *start, *end;
	char *newstr;
	size_t newlen;

	start = str;
	while(*start && isspace((unsigned char)*start)) start++;

	end = start + strlen(start);
	if(end > start) end--;
	while(end > start && isspace((unsigned char)*end)) end--;

	newlen = (end >= start && !isspace((unsigned char)*end))
	         ? (size_t)(end - start + 1) : 0;
	newstr = (char*)malloc(newlen + 1);
	if(newstr) {
		memcpy(newstr, start, newlen);
		newstr[newlen] = '\0';
	}
	return newstr;
}

void ConfSetOpt(CONF *conf, const char *key, const char *value) {
	CONFNODE *c;
	for(c = conf->first; c != NULL; c = c->next) {
		if(strcmp(c->key, key) == 0) {
			if(c->value) free(c->value);
			c->value = _strdup(value);
			return;
		}
	}

	c = (CONFNODE*)malloc(sizeof(CONFNODE));
	c->key   = strdup_trim(key);
	c->value = strdup_trim(value);
	c->next  = NULL;

	if(conf->last) conf->last = conf->last->next = c;
	else           conf->last = conf->first = c;
}

BOOL ConfLoad(CONF *conf, const char *file) {
	char buf[512];
	FILE *fp = fopen(file, "r");
	if(!fp) return FALSE;

	while(fgets(buf, sizeof(buf), fp)) {
		char key[512], value[512];
		char *eq = strchr(buf, '=');
		if (!eq) continue;
		/* Split at the first '=' */
		{
			size_t klen = (size_t)(eq - buf);
			if (klen == 0 || klen >= sizeof(key)) continue;
			memcpy(key, buf, klen); key[klen] = '\0';
			/* Value runs from eq+1 to end, strip trailing \r\n */
			{
				char *vstart = eq + 1;
				char *end = vstart + strlen(vstart);
				while (end > vstart && (end[-1] == '\r' || end[-1] == '\n')) end--;
				{
					size_t vlen = (size_t)(end - vstart);
					if (vlen >= sizeof(value)) vlen = sizeof(value)-1;
					memcpy(value, vstart, vlen); value[vlen] = '\0';
				}
			}
		}
		ConfSetOpt(conf, key, value);
	}

	fclose(fp);
	return TRUE;
}

BOOL ConfSave(const CONF *conf, const char *file) {
	const CONFNODE *c;
	FILE *fp = fopen(file, "w");
	if(!fp) return FALSE;

	for(c = conf->first; c != NULL; c = c->next)
		fprintf(fp, "%s=%s\n", c->key, c->value);

	fclose(fp);
	return TRUE;
}
