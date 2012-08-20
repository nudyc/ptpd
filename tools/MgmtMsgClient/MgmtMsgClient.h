/*-
 * Copyright (c) 2011-2012 George V. Neville-Neil,
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen,
 *                         Inaqui Delgado,
 *                         Rick Ratzel,
 *                         National Instruments,
 *                         Tomasz Kleinschmidt
 * Copyright (c) 2009-2010 George V. Neville-Neil, 
 *                         Steven Kreuzer, 
 *                         Martin Burnicki, 
 *                         Jan Breuer,
 *                         Gael Mace, 
 *                         Alexandre Van Kempen
 *
 * Copyright (c) 2005-2008 Kendall Correll, Aidan Williams
 *
 * All Rights Reserved
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** 
 * @file        MgmtMsgClient.h
 * @author      Tomasz Kleinschmidt
 *
 * @brief       Debug messages printing and memory allocation functions.
 */

#ifndef MGMTMSGCLIENT_H
#define	MGMTMSGCLIENT_H

extern int verbose_flag;

#define DBG(...) \
        do { if (verbose_flag) { \
                fprintf(stderr, "DBG: "); \
                fprintf(stderr, __VA_ARGS__); } \
        } while (0)

#define ERROR(...) \
        {do { \
                fprintf(stderr, "ERROR: "); \
                fprintf(stderr, __VA_ARGS__); \
        } while (0); \
        exit(1);}

#define WARNING(...) \
        do { \
                fprintf(stderr, "WARNING: "); \
                fprintf(stderr, __VA_ARGS__); \
        } while (0)

#define XCALLOC(ptr, type, nelements, size) \
        if(!((ptr)=(type)calloc(nelements, size))) { \
                perror("failed to allocate memory"); \
		exit(1); \
	}

#define XMALLOC(ptr, type, size) \
        if(!((ptr)=(type)malloc(size))) { \
                perror("failed to allocate memory"); \
		exit(1); \
	}

#endif	/* MGMTMSGCLIENT_H */
