/*********************************************************** -- HEAD -{{{1- */
/* Simple Client for Network API Lab: Part I in Internet Technology 2011.
 *
 * Implementation of the simple client. The client connects to the address
 * specified on the command line, reads user input from stdin, and sends any
 * input from the user to the server. It then prints the reply it receives.
 * The client continues until the stdin stream is closed. On most linux
 * terminals you can do this by pressing ctrl-D.
 *
 * Build the client using e.g.
 * 		$ g++ -Wall -Wextra -o client-simple client-simple.cpp
 *
 * If MEASURE_ROUND_TRIP_TIME is enabled (=1), the program may need to be 
 * linked against additional libraries. On linux this is librt (-lrt):
 * 		$ g++ -Wall -Wextra -o client-simple client-simple.cpp -lrt
 *
 * Start with
 * 		$ ./client-simple remote.example.com 5703
 * to connect the client to the server at remote.example.com on port 5703.
 */
/******************************************************************* -}}}1- */

#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <algorithm>

//--//////////////////////////////////////////////////////////////////////////
//--    configurables       ///{{{1///////////////////////////////////////////

// Set VERBOSE to 0 to suppress non-essential output. 
#define VERBOSE 1

// Verify that the message received from the server is equal to the one
// the client sends.
#define VERIFY_MESSAGE 1

// Measure round trip time. 
#define MEASURE_ROUND_TRIP_TIME 0

// Size of the input buffer. This also corresponds to the largest single 
// query the client can send.
const size_t kInputBufferSize = 256;

// Size of the buffer used to receive the response from the server.
const size_t kReceiveBufferSize = kInputBufferSize;

//--    prototypes          ///{{{1///////////////////////////////////////////

/* Establish connection to server/port. Returns the fd of the socket or -1
 * if an error occurred.
 */
static int connect_to_server( const char* addr, const char* port );

#if MEASURE_ROUND_TRIP_TIME
/* Initialize timer resources. Should be called once before the first call
 * to get_time_stamp(). The implementation is platform dependent.
 */
static void initialize_timer();
/* Get current time stamp. The time stamp is given in fractional seconds since
 * an unspecified time in the past. The implementation is platform dependent.
 */
static double get_time_stamp();
#endif

//--    main()              ///{{{1///////////////////////////////////////////
int main( int argc, char* argv[] )
{
	const char* serverPort = 0;
	const char* serverAddress = 0;

	// get program arguments (server address and port)
	if( argc != 3 )
	{
		fprintf( stderr, "Error %s arguments\n", 
			argc < 3 ? "insufficient":"too many" 
		);
		fprintf( stderr, "  usage: %s <server> <port>\n", argv[0] );
		return 1;
	}

	serverPort = argv[2];
	serverAddress = argv[1];

	// initialize timer(s)
#	if MEASURE_ROUND_TRIP_TIME
	initialize_timer();
#	endif

	// establish connection to server
	int connfd = connect_to_server( serverAddress, serverPort );

	if( -1 == connfd )
		return 1;

	// main loop: read user input, send to server and receive server reply
	while( 1 )
	{
		char inputBuffer[kInputBufferSize];

		// get input from user
		printf( "Input> " );
		if( !fgets( inputBuffer, kInputBufferSize, stdin ) )
			break; // typically EOF, possibly error - either way: exit!

		// strip trailing '\n'
		size_t inputLength = strlen(inputBuffer);
		if( inputBuffer[inputLength-1] == '\n' )
			inputBuffer[--inputLength] = '\0';

		// print status
#		if VERBOSE
		printf( "Sending string `%s' (%zu bytes)\n", inputBuffer, inputLength );
#		endif

		// timing - query start time
#		if MEASURE_ROUND_TRIP_TIME
		double startTime = get_time_stamp();
#		endif

		// send input to server
		ssize_t remaining = inputLength;
		while( remaining > 0 )
		{
			ssize_t offset = inputLength - remaining;
			ssize_t ret = send( connfd, 
				inputBuffer+offset, 
				remaining,
				MSG_NOSIGNAL
			);

			if( -1 == ret )
			{
				perror( "send() failed" );
				return 1;
			}

			remaining -= ret;
		}

		// get response from server
		char recvBuffer[kReceiveBufferSize+1];

		size_t received = 0;
		size_t expected = std::min( kReceiveBufferSize, inputLength );

		while( received < expected )
		{
			ssize_t ret = recv( connfd,
				recvBuffer + received,
				expected-received,
				0
			);

			if( 0 == ret )
			{
				fprintf( stderr, "Error - connection closed unexpectedly!\n" );
				return 1;
			}
			if( -1 == ret )
			{
				perror( "recv() failed" );
				return 1;
			}

			received += ret;
		}

		// timing - query end time
#		if MEASURE_ROUND_TRIP_TIME
		double endTime = get_time_stamp();
#		endif

		// ensure that the received string is zero-terminated
		recvBuffer[received] = '\0';

		// print output to screen
		printf( "Response = `%s'\n", recvBuffer );
#		if VERIFY_MESSAGE
		bool match = 0 == strncmp( inputBuffer, recvBuffer, 
			std::min( kInputBufferSize, kReceiveBufferSize )
		);

		printf( "  - response does %smatch original query\n",
			match ? "" : "NOT "
		);
#		endif
#		if MEASURE_ROUND_TRIP_TIME
		printf( "  - round trip time is %f ms\n", (endTime-startTime)*1e3 );
#		endif
	}

	// clean up
	close( connfd );

	return 0;
}

//--    connect_to_server()     ///{{{1///////////////////////////////////////
static int connect_to_server( const char* addr, const char* port )
{
	sockaddr_in servAddr;
	memset( &servAddr, 0, sizeof(servAddr) );

	// resolve server using getaddrinfo()
	// getaddrinfo() should be preferred over gethostbyname(), which is the
	// legacy method for hostname resolution.
	{
		addrinfo hints;
		memset( &hints, 0, sizeof(hints) );
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		addrinfo* result = 0;
		int ret = getaddrinfo( addr, port, &hints, &result );
		
		if( 0 != ret )
		{
			fprintf( stderr, "Error - cannot resolve address: %s\n",
				gai_strerror(ret) 
			);

			return -1;
		}
		
		bool ok = false;
		for( addrinfo* res = result; res; res = res->ai_next )
		{
			if( res->ai_family == AF_INET 
				&& res->ai_addrlen == sizeof(sockaddr_in) )
			{
				ok = true;
				memcpy( &servAddr, res->ai_addr, sizeof(sockaddr_in) );
				break;
			}
		}

		freeaddrinfo( result );

		if( !ok )
		{
			fprintf( stderr, "Error - no appropriate address format\n" );
			return -1;
		}
	}

	// allocate socket
	int fd = socket( AF_INET, SOCK_STREAM, 0 );
	
	if( -1 == fd )
	{
		perror( "socket() failed" );
		return -1;
	}

	// attempt to establish connection
	if( -1 == connect( fd, (const sockaddr*)&servAddr, sizeof(servAddr) ) )
	{
		perror( "connect() failed" );
		close( fd );
		return -1;
	}

	// ok
	return fd;
}

//--    timing code         ///{{{1///////////////////////////////////////////

/** NOTE: you may skip this part of the code. It just contains code to measure
 * time on different platforms.
 */

/* Note: timer code implementations are provided for Linux, Mac OS X and 
 * Windows. If you're running this on a different platform, you'll probably
 * have to write your own code.
 *
 * NOTE: the OS X and Windows implementations are not quite as tested as the
 * Linux variant. Please report errors if you encounter any.
 */
#if MEASURE_ROUND_TRIP_TIME
#	if defined(__linux__)
#	include <time.h>

static timespec initTime;
static void initialize_timer()
{
	clock_gettime( CLOCK_REALTIME, &initTime );
}

static double get_time_stamp()
{
	timespec currentTime;
	clock_gettime( CLOCK_REALTIME, &currentTime );

	return (currentTime.tv_sec - initTime.tv_sec) + 
		1e-9*(currentTime.tv_nsec - initTime.tv_nsec);
}

#	elif defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>

static LARGE_INTEGER perfFreq;
static LARGE_INTEGER initTime;
static void initialize_timer()
{
	QueryPerformanceFrequency( &perfFreq );
	QueryPerformanceCounter( &initTime );
}

static double get_time_stamp()
{
	LARGE_INTEGER currentTime;
	QueryPerformanceCounter( &currentTime );

	return double(currentTime.QuadPart-initTime.QuadPart)
		/ double(perfFreq.QuadPart);
}

#	elif defined(__MACH__) // Mac OS X
#	include <stdint.h>
extern "C" {
#	include <mach/mach_time.h>
}

static uint64_t initTime;
static mach_timebase_info_data_t machData;
static void initialize_timer()
{
	mach_timebase_info( &machData );
	initTime = mach_absolute_time();
}
static double get_time_stamp()
{
	uint64_t currentTime = mach_absolute_time();
	uint64_t elapsed = currentTime - initTime;

	return 1e-9*(elapsed * machData.numer / machData.denom);
}

#	endif // platform
#endif // MEASURE_ROUND_TRIP_TIME

//--///}}}1//////////////// vim:syntax=cpp:foldmethod=marker:ts=4:noexpandtab: 
