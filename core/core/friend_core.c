/*©mit**************************************************************************
*                                                                              *
* This file is part of FRIEND UNIFYING PLATFORM.                               *
* Copyright (c) Friend Software Labs AS. All rights reserved.                  *
*                                                                              *
* Licensed under the Source EULA. Please refer to the copy of the MIT License, *
* found in the file license_mit.txt.                                           *
*                                                                              *
*****************************************************************************©*/
/** @file
*
*  Friend Core
*
*  Friend Core is the root of the system
*  in C and a message dispatch mechanism.
*
*  @author PS (Pawel Stefanski)
*  @date pushed 19/10/2015
* 
* \defgroup FriendCore Friend Core
* @{
*/

/* epoll events use data.ptr to store pointer to a Socket struct.
* Previously the data.fd union field was used. Now it is unified,
* except for the pipe that is used to trigger shutdown.
*/

#define _POSIX_SOURCE

//#define USE_SELECT

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <core/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#ifdef USE_SELECT

#else
#include <sys/epoll.h>
#endif
#include <arpa/inet.h>
#include "util/string.h"
#include "core/friend_core.h"
#include "network/socket.h"
#include "network/protocol_http.h"
#include <util/log/log.h>
#include <system/services/service_manager.h>
#include <util/buffered_string.h>
#include <util/buffered_string_disk.h>
#include <util/http_string.h>
#include <openssl/rand.h>

#include <hardware/network.h>

#include <system/systembase.h>
#include <core/friendcore_manager.h>
#include <openssl/crypto.h>

//#undef DEBUG
//#define DEBUG( ...)
//#undef DEBUG1
//#define DEBUG1( ...)

#define USE_PTHREAD
//#define USE_WORKERS
//#define USE_PTHREAD_ACCEPT

extern void *FCM;			// FriendCoreManager

void FriendCoreProcess( void *fcv );

int accept4(int sockfd, struct sockaddr *addr,            socklen_t *addrlen, int flags );

void FriendCoreProcessSockBlock( void *fcv );

void FriendCoreProcessSockNonBlock( void *fcv );

int nothreads = 0;					/// threads coutner @todo to rewrite
#define MAX_CALLHANDLER_THREADS 256			///< maximum number of simulatenous handlers
//#define USE_BLOCKED_SOCKETS_TO_READ_HTTP

/**
* Creates a new instance of Friend Core.
*
* @param sb pointer to SystemBase
* @param ssl flag to indicate the use of ssl protocol
* @param port communication port number
* @param maxp maximum number of polls at the same time FL>PS ?
* @param bufsiz buffer size
* @param hostname FC host name
* @return pointer to the new instance of FriendCore
* @return NULL in case of error
*/

FriendCoreInstance *FriendCoreNew( void *sb, int id, FBOOL ssl, int port, int maxp, int bufsiz, char *hostname )
{
	LOG( FLOG_INFO, "[FriendCoreNew] Starting friend core\n" );
	
	// FOR DEBUG PURPOSES! -ht
	_reads = 0;
	_writes = 0;
	_sockets = 0;
	
	FriendCoreInstance *fc = FCalloc( sizeof(FriendCoreInstance) , 1 );

	if( fc != NULL )
	{
		char buffer[ 256 ];
		fc->fci_Port = port;
		fc->fci_MaxPoll = maxp;
		fc->fci_BufferSize = bufsiz;
		fc->fci_SSLEnabled = ssl;
		fc->fci_SB = sb;
		snprintf( buffer, 256, "%032d", id );
		memcpy( fc->fci_CoreID, buffer, 32 );
		//snprintf( fc->fci_CoreID, 32, "%032d", id );
		strncpy( fc->fci_IP, hostname, 256 );
		
		pthread_mutex_init( &(fc->fci_AcceptMutex), NULL );
		pthread_cond_init( &(fc->fci_AcceptCond), NULL);
	}
	else
	{
		LOG( FLOG_PANIC, "[FriendCoreNew] Cannot allocate memory for FriendCore instance\n");
		return NULL;
	}
	
	LOG( FLOG_INFO,"[FriendCoreNew] WorkerManager started\n");
	
	return fc;
}

/**
* Stops and destroys an instance of Friend Core.
*
* @param fc pointer to Friend Core instance to destroy
*/

void FriendCoreShutdown( FriendCoreInstance* fc )
{	
	DEBUG("[FriendCoreShutdown] On exit, we have %d idle threads.\n", nothreads );

	while( fc->fci_Closed != TRUE )
	{
		LOG( FLOG_INFO, "[FriendCoreShutdown] Waiting for close\n" );
		sleep( 1 );
	}
	
	pthread_cond_destroy( &(fc->fci_AcceptCond) );
	pthread_mutex_destroy( &(fc->fci_AcceptMutex) );

	FFree( fc );
	
	LOG( FLOG_INFO,"FriendCore shutdown!\n");
}

//
// Handy string divider macro
//

static char *dividerStr = "\r\n\r\n";

//
//
//

#define NO_WORKERS

//
// Current Friend Core instance
//

struct FriendCoreInstance *fci;

/**
* Sends a quit message
*
* @param signal signal id
*/
void SignalHandler( int signal __attribute__((unused)) )
{
	DEBUG("[SignalHandler] Quit signal sent\n");
#ifdef USE_SELECT
	
#endif

	FriendCoreManagerShutdown( FCM );
}

// Removes from doc as it appears in list of modules for Friend Core group (should be in .h! :)
#ifndef DOXYGEN
/**
* Thread to handle incoming connections!!!
*/
struct fcThreadInstance 
{ 
	FriendCoreInstance		*fc;
	pthread_t				thread;
	struct epoll_event		*event;
	Socket					*sock;
	
	// Incoming from accept
	struct AcceptPair		*acceptPair;
};

#endif

//
// internal
//

static inline void moveToHttp( int fd )
{
	char redirectURL[ 1024 ];
	Http *response;
	
	char buf[ 1024 ];
	int re = 0;
	
	while( TRUE )
	{
		int r = recv( fd, buf, 1024, 0 );
		if( r  <= 0 )
		{
			break;
		}
		re += r;
		//DEBUG("Received from socket '%s' size %d\n", buf, re );
		sleep( 1 );
	}

	strcpy( redirectURL, "https://" );
	
	// get Host
	char *f = strstr( buf, "Host:" );
	char *tmp = NULL;
	if( f != NULL )
	{
		f += 6; // + "Host: "
		tmp = f;
		while( *f != '\r' )
		{
			f++;
		}
		*f = 0;
		strcat( redirectURL, tmp );
	}

	strcat( redirectURL, "/webclient/index.html" );
	
	Log( FLOG_DEBUG, "[moveToHttp] Redirect : '%s'\n", redirectURL );
	struct TagItem tags[] = {
		{ HTTP_HEADER_LOCATION, (FULONG)StringDuplicateN( redirectURL, strlen( redirectURL ) ) },
		{ HTTP_HEADER_CONNECTION, (FULONG)StringDuplicate( "close" ) },
		{ TAG_DONE, TAG_DONE }
	};
	response = HttpNewSimple( HTTP_307_TEMPORARY_REDIRECT, tags );

	/*
	struct TagItem tags[] = {
		{ HTTP_HEADER_CONNECTION, (FULONG)StringDuplicate( "Upgrade" ) },
		{ HTTP_HEADER_UPGRADE, (FULONG)StringDuplicate("TLS/1.1" ) },
		{ HTTP_HEADER_CONNECTION, (FULONG)StringDuplicate( "close" ) },
		{ TAG_DONE, TAG_DONE }
	};
	response = HttpNewSimple( HTTP_426_UPGRADE_REQUIRED, tags );
	*/
	
	if( response != NULL )
	{
		HttpAddTextContent( response, StringDuplicate("<html>please change to https!</html>") );
		
		if( HttpBuild( response ) != NULL )
		{
			int s;
			s = send( fd, response->http_Response, response->http_ResponseLength, 0 );
			//close( fd );
		}
		HttpFree( response );
	}
}

static inline void moveToHttps( Socket *sock )
{
	Http *response;
	
	struct TagItem tags[] = {
		{ HTTP_HEADER_CONNECTION, (FULONG)StringDuplicate( "close" ) },
		{ TAG_DONE, TAG_DONE }
	};
	response = HttpNewSimple( HTTP_400_BAD_REQUEST, tags );
	
	if( response != NULL )
	{
		HttpAddTextContent( response, StringDuplicate("<html>please change to https!</html>") );
		
		if( HttpBuild( response ) != NULL )
		{
			int s;
			//s = SSL_write( sock->s_Ssl, response->response, response->responseLength );
			s = send( sock->fd, response->http_Response, response->http_ResponseLength, 0 );
			//DEBUG("Response send!!!\n\n\n %s\n\n\n%d\n\n\n", response->response, response->responseLength );
			//close( fd );
		}
		HttpFree( response );
	}
}

/**
* Accept FriendCore http connections
*
* @param d pointer to fcThreadInstance
*/

#define SINGLE_SHOT
//#define ACCEPT_IN_EPOLL
#define ACCEPT_IN_THREAD

#ifdef ACCEPT_IN_THREAD

void *FriendCoreAcceptPhase2( void *data )
{
	FriendCoreInstance *fc = (FriendCoreInstance *)data;
	
	// Accept
	struct sockaddr_in6 client;
	socklen_t clientLen = sizeof( client );
	int fd = 0;
	
	Socket *incoming = NULL;
	SSL_CTX						*s_Ctx = NULL;
	SSL							*s_Ssl = NULL;
	DEBUG("[FriendCoreAcceptPhase2] before accept4\n");
	
	fc->fci_AcceptThreadDestroyed = FALSE;
	
	while( fc->fci_AcceptQuit != TRUE )
	{
		DEBUG("Going to accept!\n");

		if( FRIEND_MUTEX_LOCK( &(fc->fci_AcceptMutex) ) == 0 )
		{
			pthread_cond_wait( &fc->fci_AcceptCond, &(fc->fci_AcceptMutex) );
			FRIEND_MUTEX_UNLOCK( &(fc->fci_AcceptMutex) );
		}
		
		if( ( fd = accept( fc->fci_Sockets->fd, ( struct sockaddr* )&(client), &clientLen ) ) > 0 )
		//while( ( fd = accept4( fc->fci_Sockets->fd, ( struct sockaddr* )&client, &clientLen, 0 ) ) > 0 )
		//while( ( fd = accept4( fc->fci_Sockets->fd, ( struct sockaddr* )&client, &clientLen, SOCK_NONBLOCK ) ) > 0 )
		{
			if( fd <= 0 )
			{
				// Get some info about failure..
				switch( errno )
				{
					case EAGAIN: break;
					case EBADF:DEBUG( "[FriendCoreAcceptPhase2] The socket argument is not a valid file descriptor.\n" );
						goto accerror;
					case ECONNABORTED:DEBUG( "[FriendCoreAcceptPhase2] A connection has been aborted.\n" );
						goto accerror;
					case EINTR:DEBUG( "[FriendCoreAcceptPhase2] The accept() function was interrupted by a signal that was caught before a valid connection arrived.\n" );
						goto accerror;
					case EINVAL:DEBUG( "[FriendCoreAcceptPhase2] The socket is not accepting connections.\n" );
						goto accerror;
					case ENFILE:DEBUG( "[FriendCoreAcceptPhase2] The maximum number of file descriptors in the system are already open.\n" );
						goto accerror;
					case ENOTSOCK:DEBUG( "[FriendCoreAcceptPhase2] The socket argument does not refer to a socket.\n" );
						goto accerror;
					case EOPNOTSUPP:DEBUG( "[FriendCoreAcceptPhase2] The socket type of the specified socket does not support accepting connections.\n" );
						goto accerror;
					default: DEBUG("[FriendCoreAcceptPhase2] Accept return bad fd\n");
						goto accerror;
				}
				goto accerror;
			}
		
			int prerr = getpeername( fd, (struct sockaddr *) &client, &clientLen );
			if( prerr == -1 )
			{
				goto accerror;
			}
		
			// Get incoming
			int lbreak = 0;
		
			if( fc->fci_Sockets->s_SSLEnabled == TRUE )
			{
				int srl;
				
				s_Ssl = SSL_new( fc->fci_Sockets->s_Ctx );
				
				if( s_Ssl == NULL )
				{
					FERROR("[FriendCoreAcceptPhase2] Cannot accept SSL connection\n");
				
					goto accerror;
				}
				else
				{
					srl = SSL_set_fd( s_Ssl, fd );
					SSL_set_accept_state( s_Ssl );
					if( srl != 1 )
					{
						int error = SSL_get_error( s_Ssl, srl );
						FERROR( "[FriendCoreAcceptPhase2] Could not set fd, error: %d fd: %d\n", error, fd );
						goto accerror;
					}
					
					int err = 0;
					// we must be sure that SSL Accept is working
					while( 1 )
					{
						DEBUG("[FriendCoreAcceptPhase2] before accept\n");
						if( ( err = SSL_accept( s_Ssl ) ) == 1 )
						{
							lbreak = 1;
							break;
						}
						DEBUG("[FriendCoreAcceptPhase2] after accept, err: %d\n", err );
						if( err <= 0 || err == 2 )
						{
							int error = SSL_get_error( s_Ssl, err );
							switch( error )
							{
								case SSL_ERROR_NONE:
									// NO error..
									FERROR( "[FriendCoreAcceptPhase2] No error\n" );
									lbreak = 1;
								break;
								case SSL_ERROR_ZERO_RETURN:
									FERROR("[FriendCoreAcceptPhase2] SSL_ACCEPT error: Socket closed.\n" );
									goto accerror;
								case SSL_ERROR_WANT_READ:
									lbreak = 2;
								break;
								case SSL_ERROR_WANT_WRITE:
									lbreak = 2;
								break;
								case SSL_ERROR_WANT_ACCEPT:
									FERROR( "[FriendCoreAcceptPhase2] Want accept\n" );
									goto accerror;
								case SSL_ERROR_WANT_X509_LOOKUP:
									FERROR( "[FriendCoreAcceptPhase2] Want 509 lookup\n" );
									goto accerror;
								case SSL_ERROR_SYSCALL:
								{
									int enume = ERR_get_error();
									FERROR( "[FriendCoreAcceptPhase2] Error syscall. Goodbye! %s. Enume: %d\n", ERR_error_string( enume, NULL ), enume );
									if( enume == 0 )
									{
										//continue;
									}
									//lbreak = 2;
									//break;
									goto accerror;
								}
								case SSL_ERROR_SSL:
								{
									int enume = ERR_get_error();
									FERROR( "[FriendCoreAcceptPhase2] SSL_ERROR_SSL: %s. enume: %d\n", ERR_error_string( enume, NULL ), enume );
									lbreak = 2;
							
									// HTTP to HTTPS redirection code
									if( enume == 336027804 ) // http redirect
									{
										moveToHttp( fd );
									}
									else
									{
										goto accerror;
									}
									break;
								}
								default:
								{
									int enume = ERR_get_error();
									FERROR( "[FriendCoreAcceptPhase2] default: %s. enume: %d error: %d\n", ERR_error_string( enume, NULL ), enume, error );
								}
								break;
							}
						}
						if( lbreak >= 1 )
						{
							break;
						}
						//usleep( 0 );
				
						if( fc->fci_Shutdown == TRUE )
						{
							FINFO("[FriendCoreAcceptPhase2] Accept socket process will be stopped, becaouse Shutdown is in progress\n");
							goto accerror;
						}
					}	// while( 1 )
				}	// if SSL
			}

			DEBUG("[FriendCoreAcceptPhase2] before getting incoming: fd %d\n", fd );
		
			if( fc->fci_Shutdown == TRUE )
			{
				if( fd > 0 )
				{
					goto accerror;
				}
				break;
			}
			else
			{
				if( fd > 0 )
				{
					incoming = ( Socket *)FCalloc( 1, sizeof( Socket ) );
					if( incoming != NULL )
					{
						DEBUG("[FriendCoreAcceptPhase2] memory for socket allocated: %d\n", fd );
						incoming->s_Data = fc;
						incoming->fd = fd;
						incoming->port = ntohs( client.sin6_port );
						incoming->ip = client.sin6_addr;
						incoming->s_SSLEnabled = fc->fci_Sockets->s_SSLEnabled;
						incoming->s_SB = fc->fci_Sockets->s_SB;
						incoming->s_Interface = fc->fci_Sockets->s_Interface;
			
						if( fc->fci_Sockets->s_SSLEnabled == TRUE )
						{
							incoming->s_Ssl = s_Ssl;
							incoming->s_Ctx = s_Ctx;
							s_Ssl = NULL;
							s_Ctx = NULL;
						}
					}
					else
					{
						FERROR("[FriendCoreAcceptPhase2] Cannot allocate memory for socket!\n");
					
						goto accerror;
					}
					
					struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
					if( pre != NULL )
					{
						pre->fc = fc; pre->sock = incoming;
					
#ifdef USE_PTHREAD
						//size_t stacksize = 16777216; //16 * 1024 * 1024;
						size_t stacksize = 8777216;	// half of previous stack
						pthread_attr_t attr;
						pthread_attr_init( &attr );
						pthread_attr_setstacksize( &attr, stacksize );
						
						// Make sure we keep the number of threads under the limit
						
						//change NULL to &attr
//#ifdef USE_BLOCKED_SOCKETS_TO_READ_HTTP
						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockBlock, ( void *)pre ) != 0 )
//#else
//						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockNonBlock, ( void *)pre ) != 0 )
//#endif
						{
							FFree( pre );
						}
#endif
					}
				/*
					/// Add to epoll
					struct epoll_event event;
					event.data.ptr = incoming;
					event.events = EPOLLIN| EPOLLET;
				
					int error = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fd, &event );
			
					if( error == -1 )
					{
						Log( FLOG_ERROR, "[FriendCoreAcceptPhase2] epoll_ctl failure, cannot add fd: %d to epoll, errno %d\n", fd, errno );
						//goto accerror;
						if( fd > 0 )
						{
							if( incoming != NULL )
							{
								if( fc->fci_Sockets->s_SSLEnabled == TRUE )
								{
								
								}
								FFree( incoming );
							}
						
							if( s_Ssl != NULL )
							{
								SSL_free( s_Ssl );
							}
						
							shutdown( fd, SHUT_RDWR );
							close( fd );
						}
					}
					*/
				}
			}
			//DEBUG("[FriendCoreAcceptPhase2] in accept loop\n");
		}	// while accept

		FBOOL ok = TRUE;
		//return NULL;
accerror:
		if( ok == FALSE && fd > 0 )
		{
			if( s_Ssl != NULL )
			{
				SSL_free( s_Ssl );
			}
		
			shutdown( fd, SHUT_RDWR );
			close( fd );
		}
		
		ok = FALSE;
	}	// while not quit
	
	// time to say goodbye to thread
	fc->fci_AcceptThreadDestroyed = TRUE;
	
	DEBUG("[FriendCoreAcceptPhase2] ERROR\n");
	return NULL;
}

#else

#ifdef ACCEPT_IN_EPOLL

inline static void *FriendCoreAcceptPhase2( FriendCoreInstance *fc )
{
	// Accept
	struct sockaddr_in6 client;
	socklen_t clientLen = sizeof( client );
	int fd = 0;
	
	Socket *incoming = NULL;
	SSL_CTX						*s_Ctx = NULL;
	SSL							*s_Ssl = NULL;
	DEBUG("[FriendCoreAcceptPhase2] before accept4\n");
	
	//if( ( fd = accept4( fc->fci_Sockets->fd, ( struct sockaddr* )&client, &clientLen, SOCK_NONBLOCK ) ) > 0 )
	while( ( fd = accept4( fc->fci_Sockets->fd, ( struct sockaddr* )&client, &clientLen, SOCK_NONBLOCK ) ) > 0 )
	{
		if( fd <= 0 )
		{
			// Get some info about failure..
			switch( errno )
			{
				case EAGAIN: break;
				case EBADF:DEBUG( "[FriendCoreAcceptPhase2] The socket argument is not a valid file descriptor.\n" );
					goto accerror;
				case ECONNABORTED:DEBUG( "[FriendCoreAcceptPhase2] A connection has been aborted.\n" );
					goto accerror;
				case EINTR:DEBUG( "[FriendCoreAcceptPhase2] The accept() function was interrupted by a signal that was caught before a valid connection arrived.\n" );
					goto accerror;
				case EINVAL:DEBUG( "[FriendCoreAcceptPhase2] The socket is not accepting connections.\n" );
					goto accerror;
				case ENFILE:DEBUG( "[FriendCoreAcceptPhase2] The maximum number of file descriptors in the system are already open.\n" );
					goto accerror;
				case ENOTSOCK:DEBUG( "[FriendCoreAcceptPhase2] The socket argument does not refer to a socket.\n" );
					goto accerror;
				case EOPNOTSUPP:DEBUG( "[FriendCoreAcceptPhase2] The socket type of the specified socket does not support accepting connections.\n" );
					goto accerror;
				default: DEBUG("[FriendCoreAcceptPhase2] Accept return bad fd\n");
					goto accerror;
			}
			goto accerror;
		}
		
		int prerr = getpeername( fd, (struct sockaddr *) &client, &clientLen );
		if( prerr == -1 )
		{
			goto accerror;
		}
		
		// Get incoming
		int lbreak = 0;
		
		if( fc->fci_Sockets->s_SSLEnabled == TRUE )
		{
			int srl;
			
			s_Ssl = SSL_new( fc->fci_Sockets->s_Ctx );

			if( s_Ssl == NULL )
			{
				FERROR("[FriendCoreAcceptPhase2] Cannot accept SSL connection\n");
				
				goto accerror;
			}
			else
			{
				/*
				BIO *bio = SSL_get_rbio( s_Ssl );
				if( bio != NULL )
				{
					DEBUG("[FriendCoreAcceptPhase2] Read buffer will be changed!\n");
					BIO_set_read_buffer_size( bio, 81920 );
				}
				*/

				srl = SSL_set_fd( s_Ssl, fd );
				SSL_set_accept_state( s_Ssl );
				if( srl != 1 )
				{
					int error = SSL_get_error( s_Ssl, srl );
					FERROR( "[FriendCoreAcceptPhase2] Could not set fd, error: %d fd: %d\n", error, fd );

					goto accerror;
				}

				int err = 0;
				// we must be sure that SSL Accept is working
				while( 1 )
				{
					DEBUG("[FriendCoreAcceptPhase2] before accept\n");
					if( ( err = SSL_accept( s_Ssl ) ) == 1 )
					{
						lbreak = 1;
						break;
					}
					DEBUG("[FriendCoreAcceptPhase2] after accept, err: %d\n", err );
					if( err <= 0 || err == 2 )
					{
						int error = SSL_get_error( s_Ssl, err );
						switch( error )
						{
							case SSL_ERROR_NONE:
								// NO error..
								FERROR( "[FriendCoreAcceptPhase2] No error\n" );
								lbreak = 1;
							break;
							case SSL_ERROR_ZERO_RETURN:
								FERROR("[FriendCoreAcceptPhase2] SSL_ACCEPT error: Socket closed.\n" );
								goto accerror;
							case SSL_ERROR_WANT_READ:
								lbreak = 2;
							break;
							case SSL_ERROR_WANT_WRITE:
								lbreak = 2;
							break;
							case SSL_ERROR_WANT_ACCEPT:
								FERROR( "[FriendCoreAcceptPhase2] Want accept\n" );
								goto accerror;
							case SSL_ERROR_WANT_X509_LOOKUP:
								FERROR( "[FriendCoreAcceptPhase2] Want 509 lookup\n" );
								goto accerror;
							case SSL_ERROR_SYSCALL:
							{
								int enume = ERR_get_error();
								FERROR( "[FriendCoreAcceptPhase2] Error syscall. Goodbye! %s. Enume: %d\n", ERR_error_string( enume, NULL ), enume );
								if( enume == 0 )
								{
									goto accerror;
								}
								//lbreak = 2;
								break;
							}
							case SSL_ERROR_SSL:
							{
								int enume = ERR_get_error();
								FERROR( "[FriendCoreAcceptPhase2] SSL_ERROR_SSL: %s. enume: %d\n", ERR_error_string( enume, NULL ), enume );
								lbreak = 2;
							
								// HTTP to HTTPS redirection code
								if( enume == 336027804 ) // http redirect
								{
									moveToHttp( fd );
								}
								else
								{
									goto accerror;
								}
								break;
							}
							default:
							{
								int enume = ERR_get_error();
								FERROR( "[FriendCoreAcceptPhase2] default: %s. enume: %d error: %d\n", ERR_error_string( enume, NULL ), enume, error );
							}
							break;
						}
					}
					if( lbreak >= 1 )
					{
						break;
					}
					//usleep( 0 );
				
					if( fc->fci_Shutdown == TRUE )
					{
						FINFO("[FriendCoreAcceptPhase2] Accept socket process will be stopped, becaouse Shutdown is in progress\n");
						goto accerror;
					}
				}	// while( 1 )
			}	// if SSL
		}

		DEBUG("[FriendCoreAcceptPhase2] before getting incoming: fd %d\n", fd );
		
		if( fc->fci_Shutdown == TRUE )
		{
			if( fd > 0 )
			{
				goto accerror;
			}
			break;
		}
		else
		{
			if( fd > 0 )
			{
				incoming = ( Socket *)FCalloc( 1, sizeof( Socket ) );
				if( incoming != NULL )
				{
					DEBUG("[FriendCoreAcceptPhase2] memory for socket allocated: %d\n", fd );
					incoming->s_Data = fc;
					incoming->fd = fd;
					incoming->port = ntohs( client.sin6_port );
					incoming->ip = client.sin6_addr;
					incoming->s_SSLEnabled = fc->fci_Sockets->s_SSLEnabled;
					incoming->s_SB = fc->fci_Sockets->s_SB;
					incoming->s_Interface = fc->fci_Sockets->s_Interface;
			
					if( fc->fci_Sockets->s_SSLEnabled == TRUE )
					{
						incoming->s_Ssl = s_Ssl;
						incoming->s_Ctx = s_Ctx;
						
						s_Ssl = NULL;
						s_Ctx = NULL;
						fd = 0;
					}
				}
				else
				{
					FERROR("[FriendCoreAcceptPhase2] Cannot allocate memory for socket!\n");
					
					goto accerror;
				}
				
				struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
					if( pre != NULL )
					{
						pre->fc = fc; pre->sock = incoming;
					
#ifdef USE_PTHREAD
						//size_t stacksize = 16777216; //16 * 1024 * 1024;
						size_t stacksize = 8777216;	// half of previous stack
						pthread_attr_t attr;
						pthread_attr_init( &attr );
						pthread_attr_setstacksize( &attr, stacksize );
						
						// Make sure we keep the number of threads under the limit
						
						//change NULL to &attr
//#ifdef USE_BLOCKED_SOCKETS_TO_READ_HTTP
						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockBlock, ( void *)pre ) != 0 )
//#else
//						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockNonBlock, ( void *)pre ) != 0 )
//#endif
						{
							FFree( pre );
						}
						else
						{
							
						}
#endif
					}
				/*
				/// Add to epoll
				struct epoll_event event;
				event.data.ptr = incoming;
				event.events = EPOLLIN| EPOLLET;
				
				int error = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fd, &event );
			
				if( error == -1 )
				{
					Log( FLOG_ERROR, "[FriendCoreAcceptPhase2] epoll_ctl failure, cannot add fd: %d to epoll, errno %d\n", fd, errno );
					//goto accerror;
					if( fd > 0 )
					{
						if( incoming != NULL )
						{
							if( fc->fci_Sockets->s_SSLEnabled == TRUE )
							{
								
							}
							FFree( incoming );
						}
						
						if( s_Ssl != NULL )
						{
							SSL_free( s_Ssl );
						}
						
						shutdown( fd, SHUT_RDWR );
						close( fd );
					}
				}
				*/
			}
		}
		//DEBUG("[FriendCoreAcceptPhase2] in accept loop\n");
	}	// while accept

	return NULL;
accerror:
	if( fd > 0 )
	{
		if( s_Ssl != NULL )
		{
			SSL_free( s_Ssl );
		}
		
		shutdown( fd, SHUT_RDWR );
		close( fd );
	}
	DEBUG("[FriendCoreAcceptPhase2] ERROR\n");
	return NULL;
}

#else

void *FriendCoreAcceptPhase2( void *d )
{
	//DEBUG("[FriendCoreAcceptPhase2] detached\n");
	//pthread_detach( pthread_self() );

	struct fcThreadInstance *pre = (struct fcThreadInstance *)d;
	FriendCoreInstance *fc = (FriendCoreInstance *)pre->fc;

	// Accept
	struct sockaddr_in6 client;
	socklen_t clientLen = sizeof( client );
	int fd = 0;
	
	Socket *incoming = NULL;
	SSL_CTX						*s_Ctx = NULL;
	SSL							*s_Ssl = NULL;
	DEBUG("[FriendCoreAcceptPhase2] before accept4\n");
	
	while( ( fd = accept4( fc->fci_Sockets->fd, ( struct sockaddr* )&client, &clientLen, SOCK_NONBLOCK ) ) > 0 )
	{
		if( fd == -1 )
		{
			// Get some info about failure..
			switch( errno )
			{
				case EAGAIN: break;
				case EBADF:DEBUG( "[FriendCoreAcceptPhase2] The socket argument is not a valid file descriptor.\n" );
					goto accerror;
				case ECONNABORTED:DEBUG( "[FriendCoreAcceptPhase2] A connection has been aborted.\n" );
					goto accerror;
				case EINTR:DEBUG( "[FriendCoreAcceptPhase2] The accept() function was interrupted by a signal that was caught before a valid connection arrived.\n" );
					goto accerror;
				case EINVAL:DEBUG( "[FriendCoreAcceptPhase2] The socket is not accepting connections.\n" );
					goto accerror;
				case ENFILE:DEBUG( "[FriendCoreAcceptPhase2] The maximum number of file descriptors in the system are already open.\n" );
					goto accerror;
				case ENOTSOCK:DEBUG( "[FriendCoreAcceptPhase2] The socket argument does not refer to a socket.\n" );
					goto accerror;
				case EOPNOTSUPP:DEBUG( "[FriendCoreAcceptPhase2] The socket type of the specified socket does not support accepting connections.\n" );
					goto accerror;
				default: DEBUG("[FriendCoreAcceptPhase2] Accept return bad fd\n");
					goto accerror;
			}
			goto accerror;
		}
		
		int prerr = getpeername( fd, (struct sockaddr *) &client, &clientLen );
		if( prerr == -1 )
		{
			shutdown( fd, SHUT_RDWR );
			close( fd );
			goto accerror;
		}
		
		// Get incoming
		int lbreak = 0;
		
		if( fc->fci_Sockets->s_SSLEnabled == TRUE )
		{
			int srl;
			
			s_Ssl = SSL_new( fc->fci_Sockets->s_Ctx );

			if( s_Ssl == NULL )
			{
				FERROR("[FriendCoreAcceptPhase2] Cannot accept SSL connection\n");
				shutdown( fd, SHUT_RDWR );
				close( fd );
				goto accerror;
			}
			
			BIO *bio = SSL_get_rbio( s_Ssl );
			if( bio != NULL )
			{
				DEBUG("[FriendCoreAcceptPhase2] Read buffer will be changed!\n");
				BIO_set_read_buffer_size( bio, 81920 );
			}

			srl = SSL_set_fd( s_Ssl, fd );
			SSL_set_accept_state( s_Ssl );
			if( srl != 1 )
			{
				int error = SSL_get_error( s_Ssl, srl );
				FERROR( "[FriendCoreAcceptPhase2] Could not set fd, error: %d fd: %d\n", error, fd );
				shutdown( fd, SHUT_RDWR );
				close( fd );
				SSL_free( s_Ssl );
				goto accerror;
			}

			int err = 0;
			// we must be sure that SSL Accept is working
			while( 1 )
			{
				DEBUG("[FriendCoreAcceptPhase2] before accept\n");
				if( ( err = SSL_accept( s_Ssl ) ) == 1 )
				{
					break;
				}

				if( err <= 0 || err == 2 )
				{
					int error = SSL_get_error( s_Ssl, err );
					switch( error )
					{
						case SSL_ERROR_NONE:
							// NO error..
							FERROR( "[FriendCoreAcceptPhase2] No error\n" );
							lbreak = 1;
						break;
						case SSL_ERROR_ZERO_RETURN:
							FERROR("[FriendCoreAcceptPhase2] SSL_ACCEPT error: Socket closed.\n" );
							goto accerror;
						case SSL_ERROR_WANT_READ:
							lbreak = 2;
						break;
						case SSL_ERROR_WANT_WRITE:
							lbreak = 2;
						break;
						case SSL_ERROR_WANT_ACCEPT:
							FERROR( "[FriendCoreAcceptPhase2] Want accept\n" );
							goto accerror;
						case SSL_ERROR_WANT_X509_LOOKUP:
							FERROR( "[FriendCoreAcceptPhase2] Want 509 lookup\n" );
							goto accerror;
						case SSL_ERROR_SYSCALL:
							FERROR( "[FriendCoreAcceptPhase2] Error syscall. Goodbye! %s.\n", ERR_error_string( ERR_get_error(), NULL ) );
							//goto accerror;
							//lbreak = 2;
							break;
						case SSL_ERROR_SSL:
						{
							int enume = ERR_get_error();
							FERROR( "[FriendCoreAcceptPhase2] SSL_ERROR_SSL: %s.\n", ERR_error_string( enume, NULL ) );
							lbreak = 2;
							
							// HTTP to HTTPS redirection code
							if( enume == 336027804 ) // http redirect
							{
								moveToHttp( fd );
							}
							else
							{
								goto accerror;
							}
							break;
						}
					}
				}
				if( lbreak >= 1 )
				{
					break;
				}
				usleep( 0 );
				
				if( fc->fci_Shutdown == TRUE )
				{
					FINFO("[FriendCoreAcceptPhase2] Accept socket process will be stopped, becaouse Shutdown is in progress\n");
					break;
				}
			}
		}

		DEBUG("[FriendCoreAcceptPhase2] before getting incoming: fd %d\n", fd );
		
		if( fc->fci_Shutdown == TRUE )
		{
			if( fd > 0 )
			{
				close( fd );
			}
			break;
		}
		else
		{
			if( fd > 0 )
			{
				incoming = ( Socket *)FCalloc( 1, sizeof( Socket ) );
				if( incoming != NULL )
				{
					incoming->s_Data = fc;
					incoming->fd = fd;
					incoming->port = ntohs( client.sin6_port );
					incoming->ip = client.sin6_addr;
					incoming->s_SSLEnabled = fc->fci_Sockets->s_SSLEnabled;
					incoming->s_SB = fc->fci_Sockets->s_SB;
					incoming->s_Interface = fc->fci_Sockets->s_Interface;
			
					if( fc->fci_Sockets->s_SSLEnabled == TRUE )
					{
						incoming->s_Ssl = s_Ssl;
						incoming->s_Ctx = s_Ctx;
					}
				}
				else
				{
					FERROR("[FriendCoreAcceptPhase2] Cannot allocate memory for socket!\n");
					shutdown( fd, SHUT_RDWR );
					close( fd );
					goto accerror;
				}
				
				/// Add to epoll
				// TODO: Check return of epoll ctl
				struct epoll_event event;
				event.data.ptr = incoming;
				event.events = EPOLLIN| EPOLLET;
				
				int error = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fd, &event );
			
				if( error )
				{
					FERROR("[FriendCoreAcceptPhase2] epoll_ctl failure **************************************\n\n");
					//goto accerror;
				}
			}
		}
		//DEBUG("[FriendCoreAcceptPhase2] in accept loop\n");
	}	// while accept
	FFree( pre );
		
	return NULL;
accerror:
	DEBUG("[FriendCoreAcceptPhase2] ERROR\n");
	FFree( pre );

	//pthread_exit( 0 );

	return NULL;
}

#endif

#endif // ACCEPT IN THREAD

//
//
//

void FriendCoreProcessSockBlock( void *fcv )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif 

	if( fcv == NULL )
	{
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}

	struct fcThreadInstance *th = ( struct fcThreadInstance *)fcv;

	if( th->sock == NULL )
	{
		FFree( th );
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}

	// Let's go!

	FQUAD bufferSize = HTTP_READ_BUFFER_DATA_SIZE;
	FQUAD bufferSizeAlloc = HTTP_READ_BUFFER_DATA_SIZE_ALLOC;

	BufStringDisk *resultString = BufStringDiskNewSize( TUNABLE_LARGE_HTTP_REQUEST_SIZE );

	char *locBuffer = FMalloc( bufferSizeAlloc );

	FQUAD expectedLength = 0;
	FBOOL headerFound = FALSE;
	int headerLen = 0;
	
	SocketSetBlocking( th->sock, TRUE );
	
	DEBUG("[FriendCoreProcessSockBlock] start\n");
	
	if( locBuffer != NULL )
	{
		int retryContentNotFull = 0;
		
		while( TRUE )
		{
			// Increase timeouts in retries
			if( retryContentNotFull == 1 )
			{
				th->sock->s_SocketBlockTimeout = 100;
			}
			else if( retryContentNotFull > 1 )
			{
				th->sock->s_SocketBlockTimeout = 250;
			}
			
			// Read from socket
			int res = th->sock->s_Interface->SocketReadBlocked( th->sock, locBuffer, bufferSize, bufferSize );
			if( res > 0 )
			{
				retryContentNotFull = 0;	// we must reset error counter
				DEBUG("[FriendCoreProcessSockBlock] received bytes: %d buffer size: %lu\n", res, resultString->bsd_Size );
				
				// add received string to buffer.
				// If 
				int err = BufStringDiskAddSize( resultString, locBuffer, res );

				if( headerFound == FALSE )
				{
					// find end of header
					char *headEnd = strstr( resultString->bsd_Buffer, "\r\n\r\n" );
					if(  headEnd != NULL )
					{
						// get length of header
						headerLen = ((headEnd+4) - resultString->bsd_Buffer);
						
						char *conLen = strstr( resultString->bsd_Buffer, "Content-Length:" );
						DEBUG("[FriendCoreProcessSockBlock] Pointer to conLen %p headerLen %d\n", conLen, headerLen );
						if( conLen != NULL )
						{
							DEBUG("[FriendCoreProcessSockBlock] Conlen is not empty!\n");
							conLen += 16;
							char *conLenEnd = strstr( conLen, "\r\n" );
							if( conLenEnd != NULL )
							{
								char *end;
								expectedLength = strtoll( conLen,  &end, 0 ) + headerLen;
								DEBUG("[FriendCoreProcessSockBlock] Expected len %ld\n", expectedLength );
							}
						}
						DEBUG("[FriendCoreProcessSockBlock] Header found!\n");
						headerFound = TRUE;
					}
				}
			}
			else
			{
				if( expectedLength > 0 )
				{
					if( retryContentNotFull++ > 500 )
					{
						break;
					}
					else	// we check size and try again
					{
						if( resultString->bsd_Size >= expectedLength )
						{
							DEBUG("[FriendCoreProcessSockBlock] We have everything!\n");
							break;
						}
						else
						{
							usleep( 2000 );
							DEBUG("[FriendCoreProcessSockBlock] Continue, resultString->bsd_Size %ld expectedLength %ld\n", resultString->bsd_Size, expectedLength );
							// buffer is not equal to what should come
							continue;
						}
					}
				}
				else
				{
					DEBUG("[FriendCoreProcessSockBlock] No more data in sockets!\n");
					break;
				}
			}
		}

		DEBUG( "[FriendCoreProcessSockBlock] Exited headers loop. Now freeing up.\n" );

		if( resultString->bsd_Size > 0 )
		{
			Http *resp = ProtocolHttp( th->sock, resultString->bsd_Buffer, resultString->bsd_Size );

			if( resp != NULL )
			{
				if( resp->http_WriteType == FREE_ONLY )
				{
					HttpFree( resp );
				}
				else
				{
					HttpWriteAndFree( resp, th->sock );
				}
			}
		}

		// Free up buffers
		if( locBuffer )
		{
			FFree( locBuffer );
		}
	}

	// Shortcut!
	close_fcp:
	
	DEBUG( "[FriendCoreProcessSockBlock] Closing socket %d.\n", th->sock->fd );
	th->sock->s_Interface->SocketDelete( th->sock );
	th->sock = NULL;

	// Free the pair
	if( th != NULL )
	{
		FFree( th );
		th = NULL;
	}

	BufStringDiskDelete( resultString );

#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}





void FriendCoreProcessSockNonBlock( void *fcv )
{
#ifdef USE_PTHREAD
	pthread_detach( pthread_self() );
#endif 

	if( fcv == NULL )
	{
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}

	struct fcThreadInstance *th = ( struct fcThreadInstance *)fcv;

	if( th->sock == NULL )
	{
		FFree( th );
#ifdef USE_PTHREAD
		pthread_exit( 0 );
#endif
		return;
	}

	// Let's go!

	FQUAD bufferSize = HTTP_READ_BUFFER_DATA_SIZE;
	FQUAD bufferSizeAlloc = HTTP_READ_BUFFER_DATA_SIZE_ALLOC;

	BufStringDisk *resultString = BufStringDiskNewSize( TUNABLE_LARGE_HTTP_REQUEST_SIZE );

	char *locBuffer = FMalloc( bufferSizeAlloc );

	FQUAD expectedLength = 0;
	FBOOL headerFound = FALSE;
	int headerLen = 0;
	
	DEBUG("[FriendCoreProcessSockNonBlock] start");
	
	//int a = 65535;
	//if (setsockopt( th->sock->fd, SOL_SOCKET, SO_RCVBUF, &a, sizeof(int)) == -1) {
	//	fprintf(stderr, "[FriendCoreProcessSockNonBlock] Error setting socket opts: %s\n", strerror(errno));
	//}
	
	if( locBuffer != NULL )
	{
		int retryContentNotFull = 0;
		
		while( TRUE )
		{
			int res = th->sock->s_Interface->SocketRead( th->sock, locBuffer, bufferSize, bufferSize );
			if( res > 0 )
			{
				retryContentNotFull = 0;
				DEBUG("[FriendCoreProcessSockNonBlock] received bytes: %d\n", res );
				
				int err = BufStringDiskAddSize( resultString, locBuffer, res );
				
				if( headerFound == FALSE )
				{
					// find end of header
					char *headEnd = strstr( resultString->bsd_Buffer, "\r\n\r\n" );
					if(  headEnd != NULL )
					{
						// get length of header
						headerLen = ((headEnd+4) - resultString->bsd_Buffer);
						
						char *conLen = strstr( resultString->bsd_Buffer, "Content-Length:" );
						DEBUG("[FriendCoreProcessSockNonBlock] Pointer to conLen %p headerLen %d\n", conLen, headerLen );
						if( conLen != NULL )
						{
							DEBUG("[FriendCoreProcessSockNonBlock] Conlen is not empty!\n");
							conLen += 16;
							char *conLenEnd = strstr( conLen, "\r\n" );
							if( conLenEnd != NULL )
							{
								char *end;
								expectedLength = strtoll( conLen,  &end, 0 ) + headerLen;
								DEBUG("[FriendCoreProcessSockNonBlock] Expected len %ld\n", expectedLength );
							}
						}
						DEBUG("[FriendCoreProcessSockNonBlock] Header found!\n");
						headerFound = TRUE;
					}
				}
			}
			else
			{
				if( expectedLength > 0 )
				{
					DEBUG("[FriendCoreProcessSockNonBlock] Retry: %d\n", retryContentNotFull );
					if( retryContentNotFull++ > 500 )
					{
						break;
					}
					else	// we check size and try again
					{
						if( resultString->bsd_Size >= expectedLength )
						{
							DEBUG("[FriendCoreProcessSockNonBlock] We have everything!\n");
						}
						else
						{
							// buffer is not equal to what should come
							continue;
						}
					}
				}
				DEBUG("[FriendCoreProcessSockNonBlock] No more data in sockets!\n");
				break;
			}
		}

		DEBUG( "[FriendCoreProcessSockNonBlock] Exited headers loop. Now freeing up.\n" );

		if( resultString->bsd_Size > 0 )
		{
			Http *resp = ProtocolHttp( th->sock, resultString->bsd_Buffer, resultString->bsd_Size );

			if( resp != NULL )
			{
				if( resp->http_WriteType == FREE_ONLY )
				{
					HttpFree( resp );
				}
				else
				{
					HttpWriteAndFree( resp, th->sock );
				}
			}
		}

		// Free up buffers
		if( locBuffer )
		{
			FFree( locBuffer );
		}
	}

	// Shortcut!
	close_fcp:
	
	DEBUG( "[FriendCoreProcessSockNonBlock] Closing socket %d.\n", th->sock->fd );
	th->sock->s_Interface->SocketDelete( th->sock );
	th->sock = NULL;

	// Free the pair
	if( th != NULL )
	{
		FFree( th );
		th = NULL;
	}

	BufStringDiskDelete( resultString );

#ifdef USE_PTHREAD
	pthread_exit( 0 );
#endif
	return;
}



pthread_t thread;

#ifdef USE_SELECT
/**
* Polls all Friend Core messages via Select().
*
* This is the main loop of the Friend Core system
*
* @param fc pointer to Friend Core instance to poll
*/
static inline void FriendCoreSelect( FriendCoreInstance* fc )
{
	// add communication ReadCommPipe		
	//signal(SIGINT, test);
	// Handle signals and block while going on!
	struct sigaction setup_action;
	sigset_t block_mask, curmask; 
	sigemptyset( &block_mask );
	// Block other terminal-generated signals while handler runs.
	sigaddset( &block_mask, SIGINT );
	setup_action.sa_handler = SignalHandler;
#ifndef CYGWIN_BUILD
	setup_action.sa_restorer = NULL;
#endif
	setup_action.sa_mask = block_mask;
	setup_action.sa_flags = 0;
	sigaction( SIGINT, &setup_action, NULL );
	sigprocmask( SIG_SETMASK, NULL, &curmask );
	fci = fc;
	
	int pipefds[2] = {};
	pipe( pipefds );	
	fc->fci_ReadCorePipe = pipefds[ 0 ]; fc->fci_WriteCorePipe = pipefds[ 1 ];
	
	// Read buffer
	char buffer[ fc->fci_BufferSize ];
	int maxd = fc->fci_Sockets->fd;
	if( fc->fci_ReadCorePipe > maxd )
	{
		maxd = fc->fci_ReadCorePipe;
	}
	
	fd_set readfds;
	
	SocketSetBlocking( fc->fci_Sockets, TRUE );
	
	// All incoming network events go through here
	while( !fc->fci_Shutdown )
	{
		FD_ZERO( &readfds );
		
		FD_SET( fc->fci_Sockets->fd, &readfds );
		FD_SET( fc->fci_ReadCorePipe, &readfds );
		
		DEBUG("[FriendCoreSelect] Before select, maxd %d  pipe %d socket %d\n", maxd, fc->fci_ReadCorePipe, fc->fci_Sockets->fd );
		
		int activity = select( maxd+1, &readfds, NULL, NULL, NULL );
		
		DEBUG("[FriendCoreSelect] After select\n");
		
		if( ( activity < 0 ) && ( errno != EINTR ) )
		{
			DEBUG("[FriendCoreSelect] Select error\n");
		}
		
		if( FD_ISSET( fc->fci_ReadCorePipe, &readfds ) )
		{
			DEBUG("[FriendCoreSelect] Received from PIPE\n");
			// read all bytes from read end of pipe
			char ch;
			int result = 1;
			
			DEBUG("[FriendCoreSelect] FC Read from pipe!\n");
			
			while( result > 0 )
			{
				result = read( fc->fci_ReadCorePipe, &ch, 1 );
				DEBUG("[FriendCoreSelect] FC Read from pipe %c\n", ch );
				if( ch == 'q' )
				{
					fc->fci_Shutdown = TRUE;
					DEBUG("[FriendCoreSelect] FC Closing. Current reads: %d, writes: %d!\n", _reads, _writes );
					break;
				}
			}
			
			if( fc->fci_Shutdown == TRUE )
			{
				LOG( FLOG_INFO, "[FriendCoreSelect] Core shutdown in porgress\n");
				break;
			}
		}
		
		if( FD_ISSET( fc->fci_Sockets->fd, &readfds ) )
		{
			DEBUG("[FriendCoreSelect] Sockets received\n");
			
			FD_CLR( fc->fci_Sockets->fd, &readfds );
			
			/*
			while( nothreads >= 32 )
			{
				usleep( 500 );
			}
			*/
			
			//FriendCoreAcceptPhase2( fc );
			//pthread_create( &thread, NULL, &FriendCoreAcceptPhase2, ( void *)fc );
			
			struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
			if( pre != NULL )
			{
				pre->fc = fc;
				if( pthread_create( &pre->thread, NULL, &FriendCoreAcceptPhase2, ( void *)pre ) != 0 )
				{
					FFree( pre );
				}
			}
			
			/*
			int pid = fork();
					
					if( pid < 0 )
					{
						FERROR( "FORK fail\n" );
					}
					else if( pid == 0 )
					{
						FriendCoreAcceptPhase2( fc );
					}
					else
					{
						INFO( "Fork done\n" );
					}
			*/
			
			/*
			fc->fci_Sockets->s_AcceptFlags = SSL_VERIFY_PEER;
			// there is no need to reject non validated (by cert) connections
			Socket *sock = SocketAccept( fc->fci_Sockets );
			
			if( sock != NULL )
			{
				SocketSetBlocking( sock, TRUE );
			
				// go on and accept on the socket
				struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
				if( pre != NULL )
				{
					pre->fc = fc;
					pre->sock = sock;
					if( pthread_create( &pre->thread, NULL, &FriendCoreProcess, ( void *)pre ) != 0 )
					{
						FFree( pre );
					}
				}
			}
			*/
		}
		
		
	}	// shutdown
}
#else
/**
* Polls all Friend Core messages.
*
* This is the main loop of the Friend Core system
*
* @param fc pointer to Friend Core instance to poll
*/
static inline void FriendCoreEpoll( FriendCoreInstance* fc )
{
	int eventCount = 0;
	int i;
	struct epoll_event *currentEvent;
	struct epoll_event *events = FCalloc( fc->fci_MaxPoll, sizeof( struct epoll_event ) );


	// add communication ReadCommPipe		
	int pipefds[2] = {}; struct epoll_event piev = { 0 };	
	if( pipe( pipefds ) != 0 )
	{
		Log( FLOG_ERROR, "[FriendCoreEpoll] pipe call failed\n");
		exit(5);
	}
	fc->fci_ReadCorePipe = pipefds[ 0 ]; fc->fci_WriteCorePipe = pipefds[ 1 ];
	// add the read end to the epoll
	piev.events = EPOLLIN; piev.data.fd = fc->fci_ReadCorePipe;
	int err = epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fc->fci_ReadCorePipe, &piev );
	if( err != 0 )
	{
		LOG( FLOG_PANIC,"[FriendCoreEpoll] Cannot add main event %d\n", err );
	}

	// Handle signals and block while going on!
	struct sigaction setup_action;
	sigset_t block_mask, curmask; 
	sigemptyset( &block_mask );
	// Block other terminal-generated signals while handler runs.
	sigaddset( &block_mask, SIGINT );
	setup_action.sa_handler = SignalHandler;
	setup_action.sa_restorer = NULL;
	setup_action.sa_mask = block_mask;
	setup_action.sa_flags = 0;
	sigaction( SIGINT, &setup_action, NULL );
	sigprocmask( SIG_SETMASK, NULL, &curmask );
	fci = fc;

	events->events = EPOLLIN;
	
#ifdef ACCEPT_IN_THREAD
	pthread_t thread;
	
	//epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DISABLE, fc->fci_Sockets->fd, NULL );

	if( pthread_create( &thread, NULL, &FriendCoreAcceptPhase2, ( void *)fc ) != 0 )
	{
		DEBUG("[FriendCoreEpoll] Pthread Accept create fail\n");
	}
#endif

	// All incoming network events go through here
	while( !fc->fci_Shutdown )
	{

#ifdef SINGLE_SHOT
		epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_MOD, fc->fci_Sockets->fd, &(fc->fci_EpollEvent) );
#endif
		
		// Wait for something to happen on any of the sockets we're listening on
		DEBUG("[FriendCoreEpoll] Before epollwait\n");
		eventCount = epoll_pwait( fc->fci_Epollfd, events, fc->fci_MaxPoll, -1, &curmask );
		DEBUG("[FriendCoreEpoll] Epollwait, eventcount: %d\n", eventCount );

		for( i = 0; i < eventCount; i++ )
		{
			currentEvent = &events[i];
			Socket *sock = ( Socket *)currentEvent->data.ptr;
			if( fc->fci_Sockets != NULL )
			{
				//FERROR("epoll event %d sock %p fd %d - listen %d\n", currentEvent->events, sock, sock->fd, fc->fci_Sockets->fd );
				
				FERROR("[FriendCoreEpoll] epoll event %d - listen %d\n", currentEvent->events, fc->fci_Sockets->fd );
			}
			
			// Ok, we have a problem with our connection
			if( 
				( ( currentEvent->events & EPOLLERR ) ||
				( currentEvent->events & EPOLLRDHUP ) ||
				( currentEvent->events & EPOLLHUP ) ) || 
				!( currentEvent->events & EPOLLIN ) 
			)
			{
				if( ((Socket*)currentEvent->data.ptr)->fd == fc->fci_Sockets->fd )
				{
					// Oups! We have gone away!
					DEBUG( "[FriendCoreEpoll] Socket went away!\n" );
					break;
				}
								
				// Remove it
				LOG( FLOG_ERROR, "[FriendCoreEpoll] Socket had errors.\n" );
				if( sock != NULL )
				{
					DEBUG("[FriendCoreEpoll] FD %d\n", sock->fd );
					epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, sock->fd, NULL );
					sock->s_Interface->SocketDelete( sock );
					sock = NULL;
				}
			}
			else if( currentEvent->events & EPOLLWAKEUP )
			{
				DEBUG( "[FriendCoreEpoll] Wake up!\n" );
			}
			// First handle pipe messages
			else if( currentEvent->data.fd == fc->fci_ReadCorePipe ) //FIXME! pipe must be wrapped in a Socket*
			{
				// read all bytes from read end of pipe
				char ch;
				int result = 1;
					
				//DEBUG("[[FriendCoreEpoll] FC Reads from pipe!\n");
				
				while( result > 0 )
				{
					result = read( fc->fci_ReadCorePipe, &ch, 1 );
					if( ch == 'q' )
					{
						fc->fci_Shutdown = TRUE;
						DEBUG("[FriendCoreEpoll] FC Closing all socket connections. Current reads: %d, writes: %d!\n", _reads, _writes );
						break;
					}
				}
				
				if( fc->fci_Shutdown == TRUE )
				{
					LOG( FLOG_INFO, "[FriendCoreEpoll] Core shutdown in porgress\n");
					//break;
				}
			}
			// Accept incoming connections - if the socket fd is out listening socket fd
			else if( ((Socket*)currentEvent->data.ptr)->fd == fc->fci_Sockets->fd && !fc->fci_Shutdown )
			{
				DEBUG("[FriendCoreEpoll] =====================before calling FriendCoreAcceptPhase2\n");
				
#ifdef ACCEPT_IN_THREAD
				if( FRIEND_MUTEX_LOCK( &(fc->fci_AcceptMutex) ) == 0 )
				{
					pthread_cond_broadcast( &(fc->fci_AcceptCond) );
					FRIEND_MUTEX_UNLOCK( &(fc->fci_AcceptMutex) );
				}
				
#else
		
#ifdef ACCEPT_IN_EPOLL
				FriendCoreAcceptPhase2( fc );
#else
				
				struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
				if( pre != NULL )
				{
					pre->fc = fc;
					DEBUG("[FriendCoreEpoll] Thread create pointer: %p friendcore: %p\n", pre, fc );
					
					if( pthread_create( &pre->thread, NULL, &FriendCoreAcceptPhase2, ( void *)pre ) != 0 )
					{
						DEBUG("[FriendCoreEpoll] Pthread create fail\n");
						FFree( pre );
					}
				}
#endif
#endif // ACCEPT_IN_THREAD
				DEBUG("[FriendCoreEpoll] Accept done\n");
			}
			// Get event that are incoming!
			else if( currentEvent->events & EPOLLIN )
			{
				// Stop listening here..
				epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, sock->fd, NULL );
				INFO("[FriendCoreEpoll] Socket fd: %d removed from epoll\n", sock->fd );
				
				// Process
				if( !fc->fci_Shutdown )
				{
					DEBUG("[FriendCoreEpoll] EPOLLIN\n");
					
					struct fcThreadInstance *pre = FCalloc( 1, sizeof( struct fcThreadInstance ) );
					if( pre != NULL )
					{
						pre->fc = fc; pre->sock = sock;
					
#ifdef USE_PTHREAD
						//size_t stacksize = 16777216; //16 * 1024 * 1024;
						size_t stacksize = 8777216;	// half of previous stack
						pthread_attr_t attr;
						pthread_attr_init( &attr );
						pthread_attr_setstacksize( &attr, stacksize );
						
						// Make sure we keep the number of threads under the limit
						
						//change NULL to &attr
#ifdef USE_BLOCKED_SOCKETS_TO_READ_HTTP
						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockBlock, ( void *)pre ) != 0 )
#else
						if( pthread_create( &pre->thread, &attr, (void *(*) (void *))&FriendCoreProcessSockNonBlock, ( void *)pre ) != 0 )
#endif
						{
							FFree( pre );
						}
#else
#ifdef USE_WORKERS
						DEBUG("[FriendCoreEpoll] Worker will be launched\n");
						SystemBase *locsb = (SystemBase *)fc->fci_SB;
						if( WorkerManagerRun( locsb->sl_WorkerManager,  FriendCoreProcess, pre, NULL, "FriendCoreProcess" ) != 0 )
						{
							SocketDelete( sock );
							sock = NULL;
						}
						DEBUG("[FriendCoreEpoll] Worker launched\n");
						//WorkerManagerRun( fc->fci_WorkerManager,  FriendCoreProcess, pre );
#else
						int pid = fork();
						if( pid == 0 )
						{
							FriendCoreProcess( pre );
						}
#endif
#endif
					}
					//DEBUG("EPOLLIN end\n");
				}
			}
		}
	}
	
	//DEBUG("End main loop\n");
	
	usleep( 1 );

	int counter = 15;
	// check number of working threads
	while( TRUE )
	{
		if( nothreads <= 0 )
		{
			DEBUG("[FriendCoreEpoll] Number of threads %d\n", nothreads );
			break;
		}
		sleep( 1 );
		if( counter-- < 0 )
		{
			break;
		}
		DEBUG("[FriendCoreEpoll] Number of threads %d, waiting .....\n", nothreads );
	}
	
#ifdef USE_SELECT

#else
	struct epoll_event event;
	
	if( epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_DEL, fc->fci_Sockets->fd, &event ) == -1 )
	{
		LOG( FLOG_ERROR, "[FriendCoreEpoll] epoll_ctl can not remove connection!\n" );
	}
#endif

#ifdef ACCEPT_IN_THREAD
	fc->fci_AcceptQuit = TRUE;
	
	if( FRIEND_MUTEX_LOCK( &(fc->fci_AcceptMutex) ) == 0 )
	{
		pthread_cond_broadcast( &(fc->fci_AcceptCond) );
		FRIEND_MUTEX_UNLOCK( &(fc->fci_AcceptMutex) );
	}
	
	while( fc->fci_AcceptThreadDestroyed != TRUE )
	{
		DEBUG("[FriendCoreShutdown] Time to kill accept thread\n");
		sleep( 1 );
	}
#endif
	
	// Server is shutting down
	DEBUG("[FriendCoreEpoll] Shutting down.\n");
	if( fc->fci_Sockets )
	{
		fc->fci_Sockets->s_Interface->SocketDelete( fc->fci_Sockets );
		fc->fci_Sockets = NULL;
	}
	
	// Free epoll events
	FFree( events );
	
	LOG( FLOG_INFO, "[FriendCoreEpoll] Done freeing events" );
	events = NULL;
	
	// Free pipes!
	close( fc->fci_ReadCorePipe );
	close( fc->fci_WriteCorePipe );
	
	fc->fci_Closed = TRUE;
}
#endif

/**
* Launches an instance of Friend Core.
*
* Only returns after interrupt signal
*
* @param fc pointer to Friend Core structure
* @return 0 no errors
* @return -1 in case of errors
*/
int FriendCoreRun( FriendCoreInstance* fc )
{
	// TODO:
	//     Launch ports and services from config file.
	//     Something like this, maybe:
	//     service{
	//         name     web
	//         host     somedomain.com
	//         protocol http
	//         port     6502
	//         handler  stdweb
	//     }
	//     service{
	//         name     web-load-balancer
	//         protocol http
	//         port     6503
	//         library  loadbalancer
	//     }
	//     service{
	//         name     friend-mesh
	//         protocol friendlish
	//         port     6503
	//     }

	LOG( FLOG_INFO,"=========================================\n");
	LOG( FLOG_INFO,"==========Starting FriendCore.===========\n");
	LOG( FLOG_INFO,"=========================================\n");
	
	SystemBase *lsb = (SystemBase *)fc->fci_SB;
	
	// Open new socket for lisenting

	fc->fci_Sockets = SocketNew( lsb, fc->fci_SSLEnabled, fc->fci_Port, SOCKET_TYPE_SERVER );
	
	if( fc->fci_Sockets == NULL )
	{
		Log( FLOG_ERROR, "[FriendCoreEpoll] Cannot create socket on port: %d!\n", fc->fci_Port );
		fc->fci_Closed = TRUE;
		return -1;
	}
	
	// Non blocking listening!
	//if( SocketSetBlocking( fc->fci_Sockets, TRUE ) == -1 )
	if( SocketSetBlocking( fc->fci_Sockets, FALSE ) == -1 )
	{
		Log( FLOG_ERROR, "[FriendCoreEpoll] Cannot set socket to blocking state!\n");
		fc->fci_Sockets->s_Interface->SocketDelete( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}
	
	SSL_CTX_get_read_ahead( fc->fci_Sockets->s_Ctx );
	SSL_CTX_set_session_cache_mode( fc->fci_Sockets->s_Ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
			
	
	if( SocketListen( fc->fci_Sockets ) != 0 )
	{
		Log( FLOG_ERROR, "[FriendCoreEpoll] Cannot setup socket!\nCheck if port: %d\n", fc->fci_Port );
		fc->fci_Sockets->s_Interface->SocketDelete( fc->fci_Sockets );
		fc->fci_Closed= TRUE;
		return -1;
	}
	
	pipe2( fc->fci_SendPipe, 0 );
	pipe2( fc->fci_RecvPipe, 0 );

#ifdef USE_SELECT
	
	FriendCoreSelect( fc );
	
#else
	// Create epoll
	fc->fci_Epollfd = epoll_create1( EPOLL_CLOEXEC );
	if( fc->fci_Epollfd == -1 )
	{
		Log( FLOG_ERROR, "[FriendCore] epoll_create\n" );
		fc->fci_Sockets->s_Interface->SocketDelete( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}

	// Register for events
	
	memset( &(fc->fci_EpollEvent), 0, sizeof( fc->fci_EpollEvent ) );
	fc->fci_EpollEvent.data.ptr = fc->fci_Sockets;
#ifdef SINGLE_SHOT
	fc->fci_EpollEvent.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR | EPOLLONESHOT ;// | EPOLLEXCLUSIVE ; //all flags are necessary, otherwise epoll may not deliver disconnect events and socket descriptors will leak
#else
	fc->fci_EpollEvent.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLHUP | EPOLLERR;// | EPOLLEXCLUSIVE ; //all flags are necessary, otherwise epoll may not deliver disconnect events and socket descriptors will leak
#endif
	
	if( epoll_ctl( fc->fci_Epollfd, EPOLL_CTL_ADD, fc->fci_Sockets->fd, &(fc->fci_EpollEvent) ) == -1 )
	{
		Log( FLOG_ERROR, "[FriendCore] epoll_ctl fail\n" );
		fc->fci_Sockets->s_Interface->SocketDelete( fc->fci_Sockets );
		fc->fci_Closed = TRUE;
		return -1;
	}

	DEBUG("[FriendCoreRun] Listening.\n");
	FriendCoreEpoll( fc );
#endif
	
	fc->fci_Sockets = NULL;
	fc->fci_Closed = TRUE;

	// Close libraries
	if( fc->fci_Libraries )
	{
		unsigned int iterator = 0;
		HashmapElement* e = NULL;
		while( ( e = HashmapIterate( fc->fci_Libraries, &iterator ) ) != NULL )
		{
			DEBUG( "[FriendCoreRun] Closing library at address %lld\n", ( long long int )e->hme_Data );
			LibraryClose( (Library*)e->hme_Data );
			e->hme_Data = NULL;
			FFree( e->hme_Key );
			e->hme_Key = NULL;
		}
		HashmapFree( fc->fci_Libraries );
		fc->fci_Libraries = NULL;
	}
	
#ifdef USE_SELECT

#else
	// Close this
	if( fc->fci_Epollfd )
	{
		LOG( FLOG_INFO, "[FriendCoreRun] Closing Epoll file descriptor\n");
		close( fc->fci_Epollfd );
	}
#endif
	
	close( fc->fci_SendPipe[0] );
	close( fc->fci_SendPipe[1] );
	close( fc->fci_RecvPipe[0] );
	close( fc->fci_RecvPipe[1] );
	
	LOG( FLOG_INFO, "[FriendCoreRun] Goodbye.\n");
	return 0;
}

/**
* Opens a library into a Friend Code thread.
*
* @param fc pointer to Friend Core instance
* @param libname name of the library to open
* @param version version of the libray to open
* @return NULL library could not be open
*/
Library* FriendCoreGetLibrary( FriendCoreInstance* fc, char* libname, FULONG version )
{
	if( !fc )
	{
		return NULL;
	}

	if( !fc->fci_Libraries )
	{
		fc->fci_Libraries = HashmapNew();
	}

	HashmapElement* e = HashmapGet( fc->fci_Libraries, libname );
	Library* lib = NULL;
	if( e == NULL )
	{
		SystemBase *lsb = (SystemBase *)fc->fci_SB;
		lib = LibraryOpen( lsb, libname, version );
		if( !lib )
		{
			return NULL;
		}
		HashmapPut( fc->fci_Libraries, StringDuplicate( libname ), lib );
	}
	else
	{
		lib = (Library*)e->hme_Data;
		if( lib->l_Version < version )
		{
			lib = NULL;
		}
	}
	return lib;
}

/**@}*/
// End of FriendCore Doxygen group
