/*********************************************************** -- HEAD -{{{1- */
/* Multi-Client Emulator for Network API Lab in Internet Technology 2012.
*/
/******************************************************************* -}}}1- */

#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <limits>
#include <algorithm>

//--//////////////////////////////////////////////////////////////////////////
//--    configurables       ///{{{1///////////////////////////////////////////

// Set VERBOSE to 0 to suppress non-essential output. 
#define VERBOSE 0

// Verify that the message received from the server is equal to the one
// the client sends.
#define VERIFY_MESSAGE 1

// Measure time taken to establish a connection
#define MEASURE_CONNECT_TIME 1

// Measure round trip time. 
#define MEASURE_ROUND_TRIP_TIME 1

// Set TCPNODELAY in connect_to_server()
#define SET_TCPNODELAY 1

// Buffer size for data transfers. Note: each open connection allocates this
// buffer independently, so defining it to be a few 100 MB and the attempting
// to have >1000 concurrent connections is probably a bad idea(tm).
const size_t kConnectionBufferSize = 256;

//--    constants           ///{{{1///////////////////////////////////////////

/* Connection States. 
 */
enum EConnState
{
	eConnStateConnecting, // async connect() issued, but not yet done
	eConnStateSending, // connect() finished, ready to send data
	eConnStateReceiving, // send() finished, ready to receive response
	eConnStateDead // done (either due to error or completion)
};

//--    structures          ///{{{1///////////////////////////////////////////

/* Per-connection data
 */
struct ConnectionData
{
	EConnState state; 

	// socket fd
	int sock;

	// expected response size
	size_t expectedSize;

	// transfer buffer; used for both sending and receiving
	size_t bufferOffset, bufferSize;
	char buffer[kConnectionBufferSize+1];

	// aux. data
	size_t repeatsLeft;
	//double timeout;

	// per-connection measurements
#	if MEASURE_CONNECT_TIME
	double connectStart, connectEnd;
#	endif
#	if MEASURE_ROUND_TRIP_TIME
	double roundTripStart, roundTripEnd;
#	endif
};

//--    global state        ///{{{1///////////////////////////////////////////

// Message sent by client. 
static const char* g_clientMessage = "client%d";

//--    prototypes          ///{{{1///////////////////////////////////////////

/* Process client that is ready for writing.
 */
static bool client_process_send( size_t cid, ConnectionData& cd );
/* Process client that is ready for sending.
 */
static bool client_process_recv( size_t cid, ConnectionData& cd );

/* Resolves/parses address given by `host' and `port', and initializes the
 * IPv4 address `sa' with this data.
 *
 * Returns true/false to indicate success/failure.
 */
static bool resolve_address( sockaddr_in& sa, const char* host, const char* port );

/* Initiates connection to the address specified in `sa'. The socket is put 
 * into non-blocking mode before the call to connect() - to use the connection,
 * wait until the socket is ready for writing and then retrieve the status with
 * getsockopt() / SO_ERROR.
 */
static int connect_to_server_nonblock( const sockaddr_in& sa );

#if MEASURE_ROUND_TRIP_TIME || MEASURE_CONNECT_TIME
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
	size_t numClients = 1;
	size_t numRepeats = 1;

	const char* serverPort = 0;
	const char* serverAddress = 0;

	// get program arguments (server address and port)
	if( argc < 4 || argc > 6 )
	{
		fprintf( stderr, "Error %s arguments\n", 
			argc < 4 ? "insufficient":"too many" 
		);
		fprintf( stderr, "  synopsis: %s <server> <port> <#num> [<#rep>] [<msg>]\n", 
			argv[0] 
		);
		fprintf( stderr, "    - <#num> -- number of clients to be emulated\n" );
		fprintf( stderr, "    - <#rep> -- number of times message is sent\n" );
		fprintf( stderr, "    - <msg> -- message sent by clients (see below)\n" );
		fprintf( stderr, "\n" );
		fprintf( stderr, "If the client message includes a `%%d' place holder, it\n" );
		fprintf( stderr, "is replaced by an unique client id (0 < cid < #clients)\n" );
		fprintf( stderr, "before the message is sent to the server.\n" );
		fprintf( stderr, "The default message is `%s'\n", g_clientMessage );
		return 1;
	}

	serverPort = argv[2];
	serverAddress = argv[1];

	numClients = atol(argv[3]);

	if( argc >= 5 ) numRepeats = atol(argv[4]);
	if( argc >= 6 ) g_clientMessage = argv[5];

	// print short status
	printf( "Simulating %zu clients.\n", numClients );

	// initialize timer(s)
#	if MEASURE_ROUND_TRIP_TIME
	initialize_timer();
#	endif

	// resolve address of server once
	sockaddr_in servAddr;
	if( !resolve_address( servAddr, serverAddress, serverPort ) )
		return 1;

	// establish N connections (async)
	printf( "Establishing %zu connections... \n", numClients );

	size_t connErrors = 0;
	ConnectionData* connections = new ConnectionData[numClients];

	for( size_t i = 0; i < numClients; ++i )
	{
#		if MEASURE_CONNECT_TIME
		connections[i].connectEnd = connections[i].connectStart = -1.0;
		connections[i].connectStart = get_time_stamp();
#		endif

		connections[i].sock = connect_to_server_nonblock( servAddr );
		connections[i].state = eConnStateConnecting;

		// initialize client aux. data
		connections[i].repeatsLeft = numRepeats-1;
	
#		if MEASURE_ROUND_TRIP_TIME
		connections[i].roundTripEnd = connections[i].roundTripStart = -1.0;
#		endif

		// on error: mark client as dead
		if( -1 == connections[i].sock )
		{
			connections[i].state = eConnStateDead;
			++connErrors;
		}
	}

	if( connErrors > 0 )
	{
		printf( "  %zu errors while establishing connections\n", connErrors );

		if( connErrors == numClients )
		{
			printf( "All clients errored. Bye\n" );
			return 1;
		}
	}

	printf( "  successfully initiated %zu connection attempts!\n", 
		numClients-connErrors );

	// event handling loop
	size_t clientsAlive = numClients - connErrors;

	while( clientsAlive > 0 )
	{
		int maxfd = 0;
		fd_set rset, wset;

		FD_ZERO( &rset ); 
		FD_ZERO( &wset );

		// put active clients into their respective sets
		for( size_t i = 0; i < numClients; ++i )
		{
			switch( connections[i].state )
			{
				case eConnStateSending:
				case eConnStateConnecting:
					FD_SET( connections[i].sock, &wset );
					break;

				case eConnStateReceiving:
					FD_SET( connections[i].sock, &rset );
					break;

				case eConnStateDead: break;
			}

			maxfd = std::max( connections[i].sock, maxfd );
		}

		// wait for any event
		int ret = select( maxfd+1, &rset, &wset, 0, 0 );

		if( 0 == ret )
		{
			continue;
		}

		if( -1 == ret )
		{
			perror( "select()" );
			return 1;
		}

		// handle events
		size_t finishedClients = 0;

		for( size_t i = 0; i < numClients; ++i )
		{
			if( connections[i].state == eConnStateDead ) continue;

			bool keep = true;
			if( FD_ISSET( connections[i].sock, &wset ) )
			{
				keep = client_process_send( i, connections[i] );
			}
			else if( FD_ISSET( connections[i].sock, &rset ) )
			{
				keep = client_process_recv( i, connections[i] );
			}

			if( !keep )
			{
				close( connections[i].sock );

				connections[i].sock = -1;
				connections[i].state = eConnStateDead;

				++finishedClients;
			}
		}

		clientsAlive -= finishedClients;
	}

	// gather and display some statistics
#	if MEASURE_CONNECT_TIME
	{
		double avgTime = 0.0;
		double maxTime = 0.0;
		double minTime = std::numeric_limits<double>::infinity();
		size_t timedItems = 0, erroredItems = 0;

		for( size_t i = 0; i < numClients; ++i )
		{
			if( connections[i].connectEnd < 0.0 )
			{
				++erroredItems;
				continue;
			}
			
			double delta = connections[i].connectEnd - connections[i].connectStart;

			minTime = std::min( delta, minTime );
			maxTime = std::max( delta, maxTime );
			avgTime += delta;
			++timedItems;

#		if VERBOSE
			printf( "  - conn %zu : connect time = %f ms\n", 
				i, delta*1e3 );
#		endif
		}

		avgTime /= timedItems;

		printf( "Connect timing results for %zu successful connections\n",
			timedItems );
		printf( "  - min time: %f ms\n", minTime*1e3 );
		printf( "  - max time: %f ms\n", maxTime*1e3 );
		printf( "  - average time: %f ms\n", avgTime*1e3 );
		printf( " (%zu connections failed!)\n", erroredItems );
	}
#	endif
#	if MEASURE_ROUND_TRIP_TIME
	{
		double avgTime = 0.0;
		double maxTime = 0.0;
		double minTime = std::numeric_limits<double>::infinity();
		size_t timedItems = 0;

		for( size_t i = 0; i < numClients; ++i )
		{
			if( connections[i].roundTripEnd < 0.0 ) continue;
			
			double delta = connections[i].roundTripEnd - connections[i].roundTripStart;

			minTime = std::min( delta, minTime );
			maxTime = std::max( delta, maxTime );
			avgTime += delta;
			++timedItems;

#		if VERBOSE
			printf( "  - conn %zu : round trip time = %f ms for %zu round trips\n", 
				i, delta*1e3, numRepeats );
#		endif
		}

		avgTime /= timedItems;

		printf( "Roundtrip timing results for %zu connections for %zu round trips\n",
			timedItems, numRepeats );
		printf( "  - min time: %f ms\n", minTime*1e3 );
		printf( "  - max time: %f ms\n", maxTime*1e3 );
		printf( "  - average time: %f ms\n", avgTime*1e3 );
	}
#	endif
	
	// clean up
	for( size_t i = 0; i < numClients; ++i )
		close( connections[i].sock );

	delete [] connections;
	
	return 0;
}

//--    client_process_send()   ///{{{1///////////////////////////////////////
static bool client_process_send( size_t cid, ConnectionData& cd )
{
#	if VERBOSE
	printf( "  - conn %zu is read to send\n", cid );
#	endif

	if( cd.state == eConnStateConnecting )
	{
		// connection finished. check socket state/error
		int error = 0;
		socklen_t errlen = sizeof(error);

		int ret = getsockopt( cd.sock, SOL_SOCKET, SO_ERROR, &error, &errlen );

		if( -1 == ret )
		{
			perror( "getsockopt(SO_ERROR)" );
			return false;
		}

		if( 0 != error )
		{
			fprintf( stderr, "  - conn %zu : async connect() error: %s\n", 
				cid, strerror(error)
			);
			return false;
		}

#		if MEASURE_CONNECT_TIME
		// record time when connection was established
		cd.connectEnd = get_time_stamp();
#		endif

		// construct message for client
		cd.bufferOffset = 0;
		snprintf( cd.buffer, kConnectionBufferSize, g_clientMessage, int(cid) );

		cd.bufferSize = strlen(cd.buffer);

		// client is now sending stuff
		cd.state = eConnStateSending;

		// record starting time of send
#		if MEASURE_ROUND_TRIP_TIME
		cd.roundTripStart = get_time_stamp();
#		endif
	}

	// send as much data as possible
	int ret = send( cd.sock, 
		cd.buffer+cd.bufferOffset, 
		cd.bufferSize-cd.bufferOffset,
		MSG_NOSIGNAL
	);

	if( ret == -1 )
	{
		fprintf( stderr, "  - conn %zu : send() error: %s\n", 
			cid, strerror(errno) 
		);
		return false;
	}

	cd.bufferOffset += ret;

	// was the whole message sent?
	if( cd.bufferOffset == cd.bufferSize )
	{
		cd.expectedSize = cd.bufferSize;

		// clean up connection buffer
		cd.bufferSize = 0;
		cd.bufferOffset = 0;
		memset( cd.buffer, 0, kConnectionBufferSize );

		// proceed with the receiving state
		cd.state = eConnStateReceiving;
	}

	// carry on handling this connection
	return true;
}

//--    client_process_recv()   ///{{{1///////////////////////////////////////
static bool client_process_recv( size_t cid, ConnectionData& cd )
{
#	if VERBOSE
	printf( "  - conn %zu is read to receive\n", cid );
#	endif

	// receive all available data
	int ret = recv( cd.sock,
		cd.buffer+cd.bufferOffset,
		cd.expectedSize - cd.bufferOffset,
		0
	);

	if( 0 == ret )
	{
		fprintf( stderr, "  - conn %zu : connection closed by peer\n", cid );
		return false;
	}
	if( -1 == ret )
	{
		fprintf( stderr, "  - conn %zu : error in recv() : %s\n", 
			cid, strerror(errno) );
		return false;
	}

	// update buffer
	cd.bufferOffset += ret;
	cd.buffer[cd.bufferOffset] = '\0';

	// did the whole message arrive?
	if( cd.bufferOffset == cd.expectedSize )
	{
		// record end time
#		if MEASURE_ROUND_TRIP_TIME
		if( cd.repeatsLeft == 0 )
		{
			cd.roundTripEnd = get_time_stamp();
		}
#		endif

		// verify message
#		if VERIFY_MESSAGE
		char reconstructed[kConnectionBufferSize+1];
		snprintf( reconstructed, kConnectionBufferSize, g_clientMessage, int(cid) );

		if( 0 != strncmp( reconstructed, cd.buffer, cd.expectedSize ) )
		{
			fprintf( stderr, "  - conn %zu : message mismatch!\n", cid );
		}
#		endif

		// ok, done
		if( cd.repeatsLeft > 0 )
		{
			cd.state = eConnStateSending;

			snprintf( cd.buffer, kConnectionBufferSize, g_clientMessage, int(cid) );
			cd.bufferOffset = 0;
			cd.bufferSize = strlen(cd.buffer);

			--cd.repeatsLeft;
		}
		else
		{
			return false;
		}
	}

	return true;
}

//--    resolve_address()       ///{{{1///////////////////////////////////////
static bool resolve_address( sockaddr_in& sa, const char* host, const char* port )
{
	// zero data
	memset( &sa, 0, sizeof(sa) );

	// resolve server using getaddrinfo()
	addrinfo hints;
	memset( &hints, 0, sizeof(hints) );
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* result = 0;
	int ret = getaddrinfo( host, port, &hints, &result );
	
	if( 0 != ret )
	{
		fprintf( stderr, "Error - cannot resolve address: %s\n",
			gai_strerror(ret) 
		);

		return false;
	}
	
	bool ok = false;
	for( addrinfo* res = result; res; res = res->ai_next )
	{
		if( res->ai_family == AF_INET 
			&& res->ai_addrlen == sizeof(sockaddr_in) )
		{
			ok = true;
			memcpy( &sa, res->ai_addr, sizeof(sockaddr_in) );
			break;
		}
	}

	freeaddrinfo( result );

	if( !ok )
	{
		fprintf( stderr, "Error - no appropriate address format\n" );
		return false;
	}

	return true;
}

//--    connect_to_server()     ///{{{1///////////////////////////////////////
static int connect_to_server_nonblock( const sockaddr_in& sa )
{
	// allocate socket
	int fd = socket( AF_INET, SOCK_STREAM, 0 );
	
	if( -1 == fd )
	{
		perror( "socket() failed" );
		return -1;
	}

	// put socket into non-blocking mode
	int oldFlags = fcntl( fd, F_GETFL, 0 );
	if( -1 == oldFlags )
	{
		perror( "fcntl(F_GETFL) failed" );
		close(fd);
		return -1;
	}

	if( -1 == fcntl( fd, F_SETFL, oldFlags | O_NONBLOCK ) )
	{
		perror( "fcntl(F_SETFL) failed" );
		close(fd);
		return -1;
	}

	// attempt to establish connection
	// Note: the socket is in non-blocking mode, so we'll probably get an
	// EINPROGRESS, which is OK.
	if( -1 == connect( fd, (const sockaddr*)&sa, sizeof(sa) ) )
	{
		if( errno != EINPROGRESS )
		{
			perror( "connect() failed" );
			close( fd );
			return -1;
		}
	}

#	if SET_TCPNODELAY
	// set TCPNODELAY option on socket
	int yes = 1;
	if( -1 == setsockopt( fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes) ) )
	{
		perror( "setsockopt(TCP_NODELAY) failed" );
		close(fd);
		return -1;
	}
#	endif

	// ok
	return fd;
}

//--    timing code         ///{{{1///////////////////////////////////////////

/* Note: timer code implementations are provided for Linux, Mac OS X and 
 * Windows. If you're running this on a different platform, you'll probably
 * have to write your own code.
 *
 * NOTE: the OS X and Windows implementations are not quite as tested as the
 * Linux variant. Please report errors if you encounter any.
 */
#if MEASURE_ROUND_TRIP_TIME || MEASURE_CONNECT_TIME
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
#endif // MEASURE_ROUND_TRIP_TIME || MEASURE_CONNECT_TIME

//--///}}}1//////////////// vim:syntax=cpp:foldmethod=marker:ts=4:noexpandtab: 
