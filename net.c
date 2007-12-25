/* net.c
 * Implementation of network-related stuff.
 *
 * File begun on 2007-07-20 by RGerhards (extracted from syslogd.c)
 * This file is under development and has not yet arrived at being fully
 * self-contained and a real object. So far, it is mostly an excerpt
 * of the "old" message code without any modifications. However, it
 * helps to have things at the right place one we go to the meat of it.
 *
 * Starting 2007-12-24, I have begun to shuffle more network-related code
 * from syslogd.c to over here. I am not sure if it will stay here in the
 * long term, but it is good to have it out of syslogd.c. Maybe this here is
 * an interim location ;)
 *
 * Copyright 2007 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */
#include "config.h"

#ifdef SYSLOG_INET

#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <netdb.h>
#include <fnmatch.h>

#include "syslogd.h"
#include "syslogd-types.h"
#include "net.h"
#include "parse.h"

/* support for defining allowed TCP and UDP senders. We use the same
 * structure to implement this (a linked list), but we define two different
 * list roots, one for UDP and one for TCP.
 * rgerhards, 2005-09-26
 */
/* All of the five below are read-only after startup */
struct AllowedSenders *pAllowedSenders_UDP = NULL; /* the roots of the allowed sender */
struct AllowedSenders *pAllowedSenders_TCP = NULL; /* lists. If NULL, all senders are ok! */
static struct AllowedSenders *pLastAllowedSenders_UDP = NULL; /* and now the pointers to the last */
static struct AllowedSenders *pLastAllowedSenders_TCP = NULL; /* element in the respective list */
#ifdef USE_GSSAPI
struct AllowedSenders *pAllowedSenders_GSS = NULL;
static struct AllowedSenders *pLastAllowedSenders_GSS = NULL;
#endif

int     ACLAddHostnameOnFail = 0; /* add hostname to acl when DNS resolving has failed */
int     ACLDontResolve = 0;       /* add hostname to acl instead of resolving it to IP(s) */

/* Code for handling allowed/disallowed senders
 */
static inline void MaskIP6 (struct in6_addr *addr, uint8_t bits) {
	register uint8_t i;
	
	assert (addr != NULL);
	assert (bits <= 128);
	
	i = bits/32;
	if (bits%32)
		addr->s6_addr32[i++] &= htonl(0xffffffff << (32 - (bits % 32)));
	for (; i < (sizeof addr->s6_addr32)/4; i++)
		addr->s6_addr32[i] = 0;
}

static inline void MaskIP4 (struct in_addr  *addr, uint8_t bits) {
	
	assert (addr != NULL);
	assert (bits <=32 );
	
	addr->s_addr &= htonl(0xffffffff << (32 - bits));
}

#define SIN(sa)  ((struct sockaddr_in  *)(sa))
#define SIN6(sa) ((struct sockaddr_in6 *)(sa))

/* This function adds an allowed sender entry to the ACL linked list.
 * In any case, a single entry is added. If an error occurs, the
 * function does its error reporting itself. All validity checks
 * must already have been done by the caller.
 * This is a helper to AddAllowedSender().
 * rgerhards, 2007-07-17
 */
static rsRetVal AddAllowedSenderEntry(struct AllowedSenders **ppRoot, struct AllowedSenders **ppLast,
		     		      struct NetAddr *iAllow, uint8_t iSignificantBits)
{
	struct AllowedSenders *pEntry = NULL;

	assert(ppRoot != NULL);
	assert(ppLast != NULL);
	assert(iAllow != NULL);

	if((pEntry = (struct AllowedSenders*) calloc(1, sizeof(struct AllowedSenders))) == NULL) {
		glblHadMemShortage = 1;
		return RS_RET_OUT_OF_MEMORY; /* no options left :( */
	}
	
	memcpy(&(pEntry->allowedSender), iAllow, sizeof (struct NetAddr));
	pEntry->pNext = NULL;
	pEntry->SignificantBits = iSignificantBits;
	
	/* enqueue */
	if(*ppRoot == NULL) {
		*ppRoot = pEntry;
	} else {
		(*ppLast)->pNext = pEntry;
	}
	*ppLast = pEntry;
	
	return RS_RET_OK;
}

/* function to clear the allowed sender structure in cases where
 * it must be freed (occurs most often when HUPed.
 * TODO: reconsider recursive implementation
 */
void clearAllowedSenders (struct AllowedSenders *pAllow) {
	if (pAllow != NULL) {
		if (pAllow->pNext != NULL)
			clearAllowedSenders (pAllow->pNext);
		else {
			if (F_ISSET(pAllow->allowedSender.flags, ADDR_NAME))
				free (pAllow->allowedSender.addr.HostWildcard);
			else
				free (pAllow->allowedSender.addr.NetAddr);
			
			free (pAllow);
		}
	}
}

/* function to add an allowed sender to the allowed sender list. The
 * root of the list is caller-provided, so it can be used for all
 * supported lists. The caller must provide a pointer to the root,
 * as it eventually needs to be updated. Also, a pointer to the
 * pointer to the last element must be provided (to speed up adding
 * list elements).
 * rgerhards, 2005-09-26
 * If a hostname is given there are possible multiple entries
 * added (all addresses from that host).
 */
static rsRetVal AddAllowedSender(struct AllowedSenders **ppRoot, struct AllowedSenders **ppLast,
		     		 struct NetAddr *iAllow, uint8_t iSignificantBits)
{
	DEFiRet;

	assert(ppRoot != NULL);
	assert(ppLast != NULL);
	assert(iAllow != NULL);

	if (!F_ISSET(iAllow->flags, ADDR_NAME)) {
		if(iSignificantBits == 0)
			/* we handle this seperatly just to provide a better
			 * error message.
			 */
			logerror("You can not specify 0 bits of the netmask, this would "
				 "match ALL systems. If you really intend to do that, "
				 "remove all $AllowedSender directives.");
		
		switch (iAllow->addr.NetAddr->sa_family) {
		case AF_INET:
			if((iSignificantBits < 1) || (iSignificantBits > 32)) {
				logerrorInt("Invalid bit number in IPv4 address - adjusted to 32",
					    (int)iSignificantBits);
				iSignificantBits = 32;
			}
			
			MaskIP4 (&(SIN(iAllow->addr.NetAddr)->sin_addr), iSignificantBits);
			break;
		case AF_INET6:
			if((iSignificantBits < 1) || (iSignificantBits > 128)) {
				logerrorInt("Invalid bit number in IPv6 address - adjusted to 128",
					    iSignificantBits);
				iSignificantBits = 128;
			}

			MaskIP6 (&(SIN6(iAllow->addr.NetAddr)->sin6_addr), iSignificantBits);
			break;
		default:
			/* rgerhards, 2007-07-16: We have an internal program error in this
			 * case. However, there is not much we can do against it right now. Of
			 * course, we could abort, but that would probably cause more harm
			 * than good. So we continue to run. We simply do not add this line - the
			 * worst thing that happens is that one host will not be allowed to
			 * log.
			 */
			logerrorInt("Internal error caused AllowedSender to be ignored, AF = %d",
				    iAllow->addr.NetAddr->sa_family);
			return RS_RET_ERR;
		}
		/* OK, entry constructed, now lets add it to the ACL list */
		iRet = AddAllowedSenderEntry(ppRoot, ppLast, iAllow, iSignificantBits);
	} else {
		/* we need to process a hostname ACL */
		if (DisableDNS) {
			logerror ("Ignoring hostname based ACLs because DNS is disabled.");
			return RS_RET_OK;
		}
		
		if (!strchr (iAllow->addr.HostWildcard, '*') &&
		    !strchr (iAllow->addr.HostWildcard, '?') &&
		    ACLDontResolve == 0) {
			/* single host - in this case, we pull its IP addresses from DNS
			* and add IP-based ACLs.
			*/
			struct addrinfo hints, *res, *restmp;
			struct NetAddr allowIP;
			
			memset (&hints, 0, sizeof (struct addrinfo));
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_DGRAM;
#			ifdef AI_ADDRCONFIG /* seems not to be present on all systems */
				hints.ai_flags  = AI_ADDRCONFIG;
#			endif

			if (getaddrinfo (iAllow->addr.HostWildcard, NULL, &hints, &res) != 0) {
			        logerrorSz("DNS error: Can't resolve \"%s\"", iAllow->addr.HostWildcard);
				
				if (ACLAddHostnameOnFail) {
				        logerrorSz("Adding hostname \"%s\" to ACL as a wildcard entry.", iAllow->addr.HostWildcard);
				        return AddAllowedSenderEntry(ppRoot, ppLast, iAllow, iSignificantBits);
				} else {
				        logerrorSz("Hostname \"%s\" WON\'T be added to ACL.", iAllow->addr.HostWildcard);
				        return RS_RET_NOENTRY;
				}
			}
			
			for (restmp = res ; res != NULL ; res = res->ai_next) {
				switch (res->ai_family) {
				case AF_INET: /* add IPv4 */
					iSignificantBits = 32;
					allowIP.flags = 0;
					if((allowIP.addr.NetAddr = malloc(res->ai_addrlen)) == NULL) {
						glblHadMemShortage = 1;
						return RS_RET_OUT_OF_MEMORY;
					}
					memcpy(allowIP.addr.NetAddr, res->ai_addr, res->ai_addrlen);
					
					if((iRet = AddAllowedSenderEntry(ppRoot, ppLast, &allowIP, iSignificantBits))
						!= RS_RET_OK)
						return(iRet);
					break;
				case AF_INET6: /* IPv6 - but need to check if it is a v6-mapped IPv4 */
					if(IN6_IS_ADDR_V4MAPPED (&SIN6(res->ai_addr)->sin6_addr)) {
						/* extract & add IPv4 */
						
						iSignificantBits = 32;
						allowIP.flags = 0;
						if((allowIP.addr.NetAddr = malloc(sizeof(struct sockaddr_in)))
						    == NULL) {
							glblHadMemShortage = 1;
							return RS_RET_OUT_OF_MEMORY;
						}
						SIN(allowIP.addr.NetAddr)->sin_family = AF_INET;
#ifdef HAVE_STRUCT_SOCKADDR_SA_LEN    
                                                SIN(allowIP.addr.NetAddr)->sin_len    = sizeof (struct sockaddr_in);
#endif
						SIN(allowIP.addr.NetAddr)->sin_port   = 0;
						memcpy(&(SIN(allowIP.addr.NetAddr)->sin_addr.s_addr),
							&(SIN6(res->ai_addr)->sin6_addr.s6_addr32[3]),
							sizeof (struct sockaddr_in));

						if((iRet = AddAllowedSenderEntry(ppRoot, ppLast, &allowIP,
								iSignificantBits))
							!= RS_RET_OK)
							return(iRet);
					} else {
						/* finally add IPv6 */
						
						iSignificantBits = 128;
						allowIP.flags = 0;
						if((allowIP.addr.NetAddr = malloc(res->ai_addrlen)) == NULL) {
							glblHadMemShortage = 1;
							return RS_RET_OUT_OF_MEMORY;
						}
						memcpy(allowIP.addr.NetAddr, res->ai_addr, res->ai_addrlen);
						
						if((iRet = AddAllowedSenderEntry(ppRoot, ppLast, &allowIP,
								iSignificantBits))
							!= RS_RET_OK)
							return(iRet);
					}
					break;
				}
			}
			freeaddrinfo (restmp);
		} else {
			/* wildcards in hostname - we need to add a text-based ACL.
			 * For this, we already have everything ready and just need
			 * to pass it along...
			 */
			iRet =  AddAllowedSenderEntry(ppRoot, ppLast, iAllow, iSignificantBits);
		}
	}

	return iRet;
}


/* Print an allowed sender list. The caller must tell us which one.
 * iListToPrint = 1 means UDP, 2 means TCP
 * rgerhards, 2005-09-27
 */
void PrintAllowedSenders(int iListToPrint)
{
	struct AllowedSenders *pSender;
	uchar szIP[64];
	
	assert((iListToPrint == 1) || (iListToPrint == 2)
#ifdef USE_GSSAPI
	       || (iListToPrint == 3)
#endif
	       );

	printf("\nAllowed %s Senders:\n",
	       (iListToPrint == 1) ? "UDP" :
#ifdef USE_GSSAPI
	       (iListToPrint == 3) ? "GSS" :
#endif
	       "TCP");

	pSender = (iListToPrint == 1) ? pAllowedSenders_UDP :
#ifdef USE_GSSAPI
		(iListToPrint == 3) ? pAllowedSenders_GSS :
#endif
		pAllowedSenders_TCP;
	if(pSender == NULL) {
		printf("\tNo restrictions set.\n");
	} else {
		while(pSender != NULL) {
			if (F_ISSET(pSender->allowedSender.flags, ADDR_NAME))
				printf ("\t%s\n", pSender->allowedSender.addr.HostWildcard);
			else {
				if(getnameinfo (pSender->allowedSender.addr.NetAddr,
						     SALEN(pSender->allowedSender.addr.NetAddr),
						     (char*)szIP, 64, NULL, 0, NI_NUMERICHOST) == 0) {
					printf ("\t%s/%u\n", szIP, pSender->SignificantBits);
				} else {
					/* getnameinfo() failed - but as this is only a
					 * debug function, we simply spit out an error and do
					 * not care much about it.
					 */
					dbgprintf("\tERROR in getnameinfo() - something may be wrong "
						"- ignored for now\n");
				}
			}
			pSender = pSender->pNext;
		}
	}
}


/* parse an allowed sender config line and add the allowed senders
 * (if the line is correct).
 * rgerhards, 2005-09-27
 */
rsRetVal addAllowedSenderLine(char* pName, uchar** ppRestOfConfLine)
{
	struct AllowedSenders **ppRoot;
	struct AllowedSenders **ppLast;
	rsParsObj *pPars;
	rsRetVal iRet;
	struct NetAddr *uIP = NULL;
	int iBits;

	assert(pName != NULL);
	assert(ppRestOfConfLine != NULL);
	assert(*ppRestOfConfLine != NULL);

	if(!strcasecmp(pName, "udp")) {
		ppRoot = &pAllowedSenders_UDP;
		ppLast = &pLastAllowedSenders_UDP;
	} else if(!strcasecmp(pName, "tcp")) {
		ppRoot = &pAllowedSenders_TCP;
		ppLast = &pLastAllowedSenders_TCP;
#ifdef USE_GSSAPI
	} else if(!strcasecmp(pName, "gss")) {
		ppRoot = &pAllowedSenders_GSS;
		ppLast = &pLastAllowedSenders_GSS;
#endif
	} else {
		logerrorSz("Invalid protocol '%s' in allowed sender "
		           "list, line ignored", pName);
		return RS_RET_ERR;
	}

	/* OK, we now know the protocol and have valid list pointers.
	 * So let's process the entries. We are using the parse class
	 * for this.
	 */
	/* create parser object starting with line string without leading colon */
	if((iRet = rsParsConstructFromSz(&pPars, (uchar*) *ppRestOfConfLine) != RS_RET_OK)) {
		logerrorInt("Error %d constructing parser object - ignoring allowed sender list", iRet);
		return(iRet);
	}

	while(!parsIsAtEndOfParseString(pPars)) {
		if(parsPeekAtCharAtParsPtr(pPars) == '#')
			break; /* a comment-sign stops processing of line */
		/* now parse a single IP address */
		if((iRet = parsAddrWithBits(pPars, &uIP, &iBits)) != RS_RET_OK) {
			logerrorInt("Error %d parsing address in allowed sender"
				    "list - ignoring.", iRet);
			rsParsDestruct(pPars);
			return(iRet);
		}
		if((iRet = AddAllowedSender(ppRoot, ppLast, uIP, iBits))
			!= RS_RET_OK) {
		        if (iRet == RS_RET_NOENTRY) {
			        logerrorInt("Error %d adding allowed sender entry "
					    "- ignoring.", iRet);
		        } else {
			        logerrorInt("Error %d adding allowed sender entry "
					    "- terminating, nothing more will be added.", iRet);
				rsParsDestruct(pPars);
				return(iRet);
		        }
		}
		free (uIP); /* copy stored in AllowedSenders list */ 
	}

	/* cleanup */
	*ppRestOfConfLine += parsGetCurrentPosition(pPars);
	return rsParsDestruct(pPars);
}



/* compares a host to an allowed sender list entry. Handles all subleties
 * including IPv4/v6 as well as domain name wildcards.
 * This is a helper to isAllowedSender. As it is only called once, it is
 * declared inline.
 * Returns 0 if they do not match, something else otherwise.
 * contributed 1007-07-16 by mildew@gmail.com
 */
static inline int MaskCmp(struct NetAddr *pAllow, uint8_t bits, struct sockaddr *pFrom, const char *pszFromHost)
{
	assert(pAllow != NULL);
	assert(pFrom != NULL);

	if(F_ISSET(pAllow->flags, ADDR_NAME)) {
		dbgprintf("MaskCmp: host=\"%s\"; pattern=\"%s\"\n", pszFromHost, pAllow->addr.HostWildcard);
		
		return(fnmatch(pAllow->addr.HostWildcard, pszFromHost, FNM_NOESCAPE|FNM_CASEFOLD) == 0);
	} else {/* We need to compare an IP address */
		switch (pFrom->sa_family) {
		case AF_INET:
			if (AF_INET == pAllow->addr.NetAddr->sa_family)
				return(( SIN(pFrom)->sin_addr.s_addr & htonl(0xffffffff << (32 - bits)) )
				       == SIN(pAllow->addr.NetAddr)->sin_addr.s_addr);
			else
				return 0;
			break;
		case AF_INET6:
			switch (pAllow->addr.NetAddr->sa_family) {
			case AF_INET6: {
				struct in6_addr ip, net;
				register uint8_t i;
				
				memcpy (&ip,  &(SIN6(pFrom))->sin6_addr, sizeof (struct in6_addr));
				memcpy (&net, &(SIN6(pAllow->addr.NetAddr))->sin6_addr, sizeof (struct in6_addr));
				
				i = bits/32;
				if (bits % 32)
					ip.s6_addr32[i++] &= htonl(0xffffffff << (32 - (bits % 32)));
				for (; i < (sizeof ip.s6_addr32)/4; i++)
					ip.s6_addr32[i] = 0;
				
				return (memcmp (ip.s6_addr, net.s6_addr, sizeof ip.s6_addr) == 0 &&
					(SIN6(pAllow->addr.NetAddr)->sin6_scope_id != 0 ?
					 SIN6(pFrom)->sin6_scope_id == SIN6(pAllow->addr.NetAddr)->sin6_scope_id : 1));
			}
			case AF_INET: {
				struct in6_addr *ip6 = &(SIN6(pFrom))->sin6_addr;
				struct in_addr  *net = &(SIN(pAllow->addr.NetAddr))->sin_addr;
				
				if ((ip6->s6_addr32[3] & (u_int32_t) htonl((0xffffffff << (32 - bits)))) == net->s_addr &&
#if BYTE_ORDER == LITTLE_ENDIAN
				    (ip6->s6_addr32[2] == (u_int32_t)0xffff0000) &&
#else
				    (ip6->s6_addr32[2] == (u_int32_t)0x0000ffff) &&
#endif
				    (ip6->s6_addr32[1] == 0) && (ip6->s6_addr32[0] == 0))
					return 1;
				else
					return 0;
			}
			default:
				/* Unsupported AF */
				return 0;
			}
		default:
			/* Unsupported AF */
			return 0;
		}
	}
}


/* check if a sender is allowed. The root of the the allowed sender.
 * list must be proveded by the caller. As such, this function can be
 * used to check both UDP and TCP allowed sender lists.
 * returns 1, if the sender is allowed, 0 otherwise.
 * rgerhards, 2005-09-26
 */
int isAllowedSender(struct AllowedSenders *pAllowRoot, struct sockaddr *pFrom, const char *pszFromHost)
{
	struct AllowedSenders *pAllow;
	
	assert(pFrom != NULL);

	if(pAllowRoot == NULL)
		return 1; /* checking disabled, everything is valid! */
	
	/* now we loop through the list of allowed senders. As soon as
	 * we find a match, we return back (indicating allowed). We loop
	 * until we are out of allowed senders. If so, we fall through the
	 * loop and the function's terminal return statement will indicate
	 * that the sender is disallowed.
	 */
	for(pAllow = pAllowRoot ; pAllow != NULL ; pAllow = pAllow->pNext) {
		if (MaskCmp (&(pAllow->allowedSender), pAllow->SignificantBits, pFrom, pszFromHost))
			return 1;
	}
	return 0;
}


/* The following #ifdef sequence is a small compatibility 
 * layer. It tries to work around the different availality
 * levels of SO_BSDCOMPAT on linuxes...
 * I borrowed this code from
 *    http://www.erlang.org/ml-archive/erlang-questions/200307/msg00037.html
 * It still needs to be a bit better adapted to rsyslog.
 * rgerhards 2005-09-19
 */
#ifndef BSD
#include <sys/utsname.h>
int should_use_so_bsdcompat(void)
{
    static int init_done;
    static int so_bsdcompat_is_obsolete;

    if (!init_done) {
	struct utsname utsname;
	unsigned int version, patchlevel;

	init_done = 1;
	if (uname(&utsname) < 0) {
		char errStr[1024];
		dbgprintf("uname: %s\r\n", strerror_r(errno, errStr, sizeof(errStr)));
		return 1;
	}
	/* Format is <version>.<patchlevel>.<sublevel><extraversion>
	   where the first three are unsigned integers and the last
	   is an arbitrary string. We only care about the first two. */
	if (sscanf(utsname.release, "%u.%u", &version, &patchlevel) != 2) {
	    dbgprintf("uname: unexpected release '%s'\r\n",
		    utsname.release);
	    return 1;
	}
	/* SO_BSCOMPAT is deprecated and triggers warnings in 2.5
	   kernels. It is a no-op in 2.4 but not in 2.2 kernels. */
	if (version > 2 || (version == 2 && patchlevel >= 5))
	    so_bsdcompat_is_obsolete = 1;
    }
    return !so_bsdcompat_is_obsolete;
}
#else	/* #ifndef BSD */
#define should_use_so_bsdcompat() 1
#endif	/* #ifndef BSD */
#ifndef SO_BSDCOMPAT
/* this shall prevent compiler errors due to undfined name */
#define SO_BSDCOMPAT 0
#endif


/* get the hostname of the message source. This was originally in cvthname()
 * but has been moved out of it because of clarity and fuctional separation.
 * It must be provided by the socket we received the message on as well as
 * a NI_MAXHOST size large character buffer for the FQDN.
 *
 * Please see http://www.hmug.org/man/3/getnameinfo.php (under Caveats)
 * for some explanation of the code found below. We do by default not
 * discard message where we detected malicouos DNS PTR records. However,
 * there is a user-configurabel option that will tell us if
 * we should abort. For this, the return value tells the caller if the
 * message should be processed (1) or discarded (0).
 */
/* TODO: after the bughunt, make this function static - rgerhards, 2007-09-18 */
rsRetVal gethname(struct sockaddr_storage *f, uchar *pszHostFQDN)
{
	DEFiRet;
	int error;
	sigset_t omask, nmask;
	char ip[NI_MAXHOST];
	struct addrinfo hints, *res;
	
	assert(f != NULL);
	assert(pszHostFQDN != NULL);

        error = getnameinfo((struct sockaddr *)f, SALEN((struct sockaddr *)f),
			    ip, sizeof ip, NULL, 0, NI_NUMERICHOST);

        if (error) {
                dbgprintf("Malformed from address %s\n", gai_strerror(error));
		strcpy((char*) pszHostFQDN, "???");
		ABORT_FINALIZE(RS_RET_INVALID_SOURCE);
	}

	if (!DisableDNS) {
		sigemptyset(&nmask);
		sigaddset(&nmask, SIGHUP);
		pthread_sigmask(SIG_BLOCK, &nmask, &omask);

		error = getnameinfo((struct sockaddr *)f, SALEN((struct sockaddr *) f),
				    (char*)pszHostFQDN, NI_MAXHOST, NULL, 0, NI_NAMEREQD);
		
		if (error == 0) {
			memset (&hints, 0, sizeof (struct addrinfo));
			hints.ai_flags = AI_NUMERICHOST;
			hints.ai_socktype = SOCK_DGRAM;

			/* we now do a lookup once again. This one should fail,
			 * because we should not have obtained a non-numeric address. If
			 * we got a numeric one, someone messed with DNS!
			 */
			if (getaddrinfo ((char*)pszHostFQDN, NULL, &hints, &res) == 0) {
				uchar szErrMsg[1024];
				freeaddrinfo (res);
				/* OK, we know we have evil. The question now is what to do about
				 * it. One the one hand, the message might probably be intended
				 * to harm us. On the other hand, losing the message may also harm us.
				 * Thus, the behaviour is controlled by the $DropMsgsWithMaliciousDnsPTRRecords
				 * option. If it tells us we should discard, we do so, else we proceed,
				 * but log an error message together with it.
				 * time being, we simply drop the name we obtained and use the IP - that one
				 * is OK in any way. We do also log the error message. rgerhards, 2007-07-16
		 		 */
		 		if(bDropMalPTRMsgs == 1) {
					snprintf((char*)szErrMsg, sizeof(szErrMsg) / sizeof(uchar),
						 "Malicious PTR record, message dropped "
						 "IP = \"%s\" HOST = \"%s\"",
						 ip, pszHostFQDN);
					logerror((char*)szErrMsg);
					pthread_sigmask(SIG_SETMASK, &omask, NULL);
					ABORT_FINALIZE(RS_RET_MALICIOUS_ENTITY);
				}

				/* Please note: we deal with a malicous entry. Thus, we have crafted
				 * the snprintf() below so that all text is in front of the entry - maybe
				 * it contains characters that make the message unreadable
				 * (OK, I admit this is more or less impossible, but I am paranoid...)
				 * rgerhards, 2007-07-16
				 */
				snprintf((char*)szErrMsg, sizeof(szErrMsg) / sizeof(uchar),
					 "Malicious PTR record (message accepted, but used IP "
					 "instead of PTR name: IP = \"%s\" HOST = \"%s\"",
					 ip, pszHostFQDN);
				logerror((char*)szErrMsg);

				error = 1; /* that will trigger using IP address below. */
			}
		}		
		pthread_sigmask(SIG_SETMASK, &omask, NULL);
	}

        if (error || DisableDNS) {
                dbgprintf("Host name for your address (%s) unknown\n", ip);
		strcpy((char*) pszHostFQDN, ip);
		ABORT_FINALIZE(RS_RET_ADDRESS_UNKNOWN);
        }

finalize_it:
	return iRet;
}


/* Return a printable representation of a host address.
 * Now (2007-07-16) also returns the full host name (if it could be obtained)
 * in the second param [thanks to mildew@gmail.com for the patch].
 * The caller must provide buffer space for pszHost and pszHostFQDN. These
 * buffers must be of size NI_MAXHOST. This is not checked here, because
 * there is no way to check it. We use this way of doing things because it
 * frees us from using dynamic memory allocation where it really does not
 * pay.
 */
rsRetVal cvthname(struct sockaddr_storage *f, uchar *pszHost, uchar *pszHostFQDN)
{
	DEFiRet;
	register uchar *p;
	int count;
	
	assert(f != NULL);
	assert(pszHost != NULL);
	assert(pszHostFQDN != NULL);

	iRet = gethname(f, pszHostFQDN);

	if(iRet == RS_RET_INVALID_SOURCE || iRet == RS_RET_ADDRESS_UNKNOWN) {
		strcpy((char*) pszHost, (char*) pszHostFQDN); /* we use whatever was provided as replacement */
		ABORT_FINALIZE(RS_RET_OK); /* this is handled, we are happy with it */
	} else if(iRet != RS_RET_OK) {
		FINALIZE; /* we return whatever error state we have - can not handle it */
	}

	/* if we reach this point, we obtained a non-numeric hostname and can now process it */

	/* Convert to lower case, just like LocalDomain above
	 */
	for (p = pszHostFQDN ; *p ; p++)
		if (isupper((int) *p))
			*p = tolower(*p);
	
	/* OK, the fqdn is now known. Now it is time to extract only the hostname
	 * part if we were instructed to do so.
	 */
	/* TODO: quick and dirty right now: we need to optimize that. We simply
	 * copy over the buffer and then use the old code. In the long term, that should
	 * be placed in its own function and probably outside of the net module (at least
	 * if should no longer reley on syslogd.c's global config-setting variables).
	 * Note that the old code always removes the local domain. We may want to
	 * make this in option in the long term. (rgerhards, 2007-09-11)
	 */
	strcpy((char*)pszHost, (char*)pszHostFQDN);
	if ((p = (uchar*) strchr((char*)pszHost, '.'))) { /* find start of domain name "machine.example.com" */
		if(strcmp((char*) (p + 1), LocalDomain) == 0) {
			*p = '\0'; /* simply terminate the string */
		} else {
			/* now check if we belong to any of the domain names that were specified
			 * in the -s command line option. If so, remove and we are done.
			 */
			if (StripDomains) {
				count=0;
				while (StripDomains[count]) {
					if (strcmp((char*)(p + 1), StripDomains[count]) == 0) {
						*p = '\0';
						FINALIZE; /* we are done */
					}
					count++;
				}
			}
			/* if we reach this point, we have not found any domain we should strip. Now
			 * we try and see if the host itself is listed in the -l command line option
			 * and so should be stripped also. If so, we do it and return. Please note that
			 * -l list FQDNs, not just the hostname part. If it did just list the hostname, the
			 * door would be wide-open for all kinds of mixing up of hosts. Because of this,
			 * you'll see comparison against the full string (pszHost) below. The termination
			 * still occurs at *p, which points at the first dot after the hostname.
			 */
			if (LocalHosts) {
				count=0;
				while (LocalHosts[count]) {
					if (!strcmp((char*)pszHost, LocalHosts[count])) {
						*p = '\0';
						break; /* we are done */
					}
					count++;
				}
			}
		}
	}

finalize_it:
	return iRet;
}

#endif /* #ifdef SYSLOG_INET */
/*
 * vi:set ai:
 */
