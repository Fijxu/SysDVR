#ifndef USB_ONLY
#include <string.h>
#include <fcntl.h> // fcntl
#include <netinet/tcp.h> // IPPROTO_TCP, TCP_NODELAY

#include "sockets.h"
#define NX_EAGAIN 11
#define NX_O_NONBLOCK 0x800

#include "../modes/defines.h"
#include "../modes/modes.h"
#include "../capture.h"

// We want to statically allocate all socketing resources to avoid heap fragmentation
// This means we use the bsd service API directly instead of the libnx wrapper

static bool SocketReady;

#define PAGE_ALIGN(x) ((x + 0xFFF) &~ 0xFFF)

#define NON_ZERO(x, y) (x == 0 ? y : x)

// This is our biggest offender in memory usage but lower values may cause random hanging over long streaming sessions.
// They can be workarounded with auto reconnection and non-blocking sockets but i prefer keeping the connection stable
// Having enough capacity ensures that we can handle any packet size without dropping frames.
#define TCP_TX_SZ (16 * 1024)
#define TCP_RX_SZ (8 * 1024)

#define TCP_TX_MAX_SZ (128 * 1024)
#define TCP_RX_MAX_SZ 0

#define UDP_TX_SZ (8 * 1024)
#define UDP_RX_SZ (4 * 1024)

#define SOCK_EFFICIENCY 2

// Formula taken from libnx itslf
#define TMEM_SIZE PAGE_ALIGN(NON_ZERO(TCP_TX_MAX_SZ, TCP_TX_SZ) + NON_ZERO(TCP_RX_MAX_SZ, TCP_RX_SZ) + UDP_TX_SZ + UDP_RX_SZ) * SOCK_EFFICIENCY

static u8 alignas(0x1000) TmemBackingBuffer[TMEM_SIZE];

void SocketInit()
{
	if (SocketReady)
		return;

	memset(TmemBackingBuffer, 0, sizeof(TmemBackingBuffer));

	const BsdInitConfig config = {
		.version = 1,
		.tmem_buffer = TmemBackingBuffer,
		.tmem_buffer_size = TMEM_SIZE,

		.tcp_tx_buf_size = TCP_TX_SZ,
		.tcp_rx_buf_size = TCP_RX_SZ,
		.tcp_tx_buf_max_size = TCP_TX_MAX_SZ,
		.tcp_rx_buf_max_size = TCP_RX_MAX_SZ,

		.udp_tx_buf_size = UDP_TX_SZ,
		.udp_rx_buf_size = UDP_RX_SZ,

		.sb_efficiency = SOCK_EFFICIENCY
	};

	LOG("Initializing BSD with tmem size %x\n", TMEM_SIZE);
	R_THROW(bsdInitialize(&config, 3, BsdServiceType_User));
}

void SocketDeinit()
{
	if (!SocketReady)
		return;

	LOG("Exiting BSD\n");
	bsdExit();
}

void SocketClose(int* socket)
{
	if (*socket != SOCKET_INVALID)
	{
		bsdClose(*socket);
		*socket = SOCKET_INVALID;
	}
}

int SocketUdp()
{
	return bsdSocket(AF_INET, SOCK_DGRAM, 0);
}

int SocketTcpListen(short port)
{
	while (true) {
		int socket = bsdSocket(AF_INET, SOCK_STREAM, 0);
		if (socket < 0)
			goto failed;

		if (!SocketMakeNonBlocking(socket))
			goto failed;

		const int optVal = 1;
		if (bsdSetSockOpt(socket, SOL_SOCKET, SO_REUSEADDR, (void*)&optVal, sizeof(optVal)) == -1)
			goto failed;

		struct sockaddr_in addr;
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(port);

		if (bsdBind(socket, (struct sockaddr*)&addr, sizeof(addr)) == -1)
			goto failed;

		if (bsdListen(socket, 1) == -1)
			goto failed;

		return socket;

	failed:
		LOG("SocketTcpListen failed");

		if (socket != SOCKET_INVALID)
			bsdClose(socket);

		svcSleepThread(1);
		continue;
	}
}

// This is a weird hack, we need to figure out when the console is in sleep mode
// and reset the listening socket when it wakes up, the only way i found to get
// a meaningful error code is from poll, which will return 0 when no connection is pending
// and 1 otherwise but if Accept fails with EAGAIN, we know the console was in sleep mode.
// We should use nifm but that comes with its share of weirdness.....
bool SocketIsErrnoNetDown()
{
	return g_bsdErrno == NX_EAGAIN;
}

int SocketTcpAccept(int listenerHandle, struct sockaddr* addr, socklen_t* addrlen)
{
	struct pollfd pollinfo;
	pollinfo.fd = listenerHandle;
	pollinfo.events = POLLIN;
	pollinfo.revents = 0;

	int rc = bsdPoll(&pollinfo, 1, 0);
	if (rc > 0)
	{
		if (pollinfo.revents & POLLIN)
		{
			int accepted = bsdAccept(listenerHandle, addr, addrlen);

			if (accepted != SOCKET_INVALID)
			{
				// Cap the socket send timeout even in blocking mode
				struct timeval tv;
				tv.tv_sec = 1;
				tv.tv_usec = 0;
				bsdSetSockOpt(accepted, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			}

			return accepted;
		}
	}

	return SOCKET_INVALID;
}

bool SocketUDPSendTo(int socket, const void* data, u32 size, struct sockaddr* addr, socklen_t addrlen)
{
	return bsdSendTo(socket, data, size, 0, addr, addrlen) == size;
}

typedef enum {
	PollResult_Timeout,
	PollResult_Disconnected,
	PollResult_CanWrite,
	PollResult_CanRead,
} PollResult;

static PollResult PolLScoket(int socket, int timeoutMs)
{
	struct pollfd pollinfo;
	pollinfo.fd = socket;
	pollinfo.events = POLLOUT | POLLHUP;
	pollinfo.revents = 0;

	int rc = bsdPoll(&pollinfo, 1, timeoutMs);
	if (rc > 0)
	{
		if (pollinfo.revents & POLLOUT)
			return PollResult_CanWrite;
		else if (pollinfo.revents & POLLHUP)
			return PollResult_Disconnected;
		// This is not exactly correct but we use this function in the context of writing, if we can read it means the socket is closed
		else if (pollinfo.revents & POLLIN)
			return PollResult_CanRead;
		else if (pollinfo.revents & POLLERR)
			return PollResult_Disconnected;
	}

	return PollResult_Timeout;
}

bool SocketSendAll(int sock, const void* buffer, u32 size)
{
	if (sock == SOCKET_INVALID)
		return false;

	u32 sent = 0;
	while (sent < size)
	{
		int res = bsdSend(sock, (const char*)buffer + sent, size - sent, MSG_DONTWAIT);
		if (res == -1)
		{
			if (g_bsdErrno == NX_EAGAIN)
			{
				int pollCount = 0;
			poll_again:
				PollResult pollRes = PolLScoket(sock, 1000);

				// Leave early if we're switching modes
				if (!IsThreadRunning)
					return false;

				// after 10 seconds we give up (and the user probably did too)
				if (++pollCount >= 10)
					return false;

				// If we can write, retry
				if (pollRes == PollResult_CanWrite)
					continue;
				else if (pollRes == PollResult_Timeout)
					goto poll_again;
				// Any other result is probably an error and we close the socket on our end
				else return false;
			}
			else
				return false;
		}

		sent += res;
	}

	return true;
}

s32 SocketRecv(int socket, void* buffer, u32 size)
{
	ssize_t r = bsdRecv(socket, buffer, size, MSG_DONTWAIT);
	if (r == -1)
	{
		if (g_bsdErrno == NX_EAGAIN)
			return 0;
		else
			return -1;
	}
	return (s32)r;
}

bool SocketMakeNonBlocking(int socket)
{
	return bsdFcntl(socket, F_SETFL, NX_O_NONBLOCK) != -1;
}

void SocketCloseReceivingEnd(int socket)
{
	if (bsdShutdown(socket, SHUT_RD) < 0)
		LOG("SocketCloseReceivingEnd: %d\n", g_bsdErrno);
}
#endif