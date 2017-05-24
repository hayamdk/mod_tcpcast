#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#define MAX_CLIENTS 32
#define MAX_MSG_SIZE 188*1024
#define BUFSIZE 188*6*8192

#define IS_SHARED_MODULE

#include "core/tsdump_def.h"

#include <inttypes.h>

#ifdef TSD_PLATFORM_MSVC

#pragma comment(lib, "ws2_32.lib")
#include <winsock2.h>
#include <windows.h>

#else

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

typedef uint16_t USHORT;
typedef int SOCKET;

static void WSACleanup()
{
	/*do nothing*/
}

#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/timeb.h>
#include <time.h>

#include "utils/arib_proginfo.h"
#include "core/module_api.h"

#ifdef TSD_PLATFORM_MSVC
#define MSG_WINSOCKERROR MSG_SYSERROR
#endif

typedef struct{
	struct in_addr addr;
	USHORT port;
	SOCKET sock;
	int buf_pos;
	int alive;
} client_t;

#ifdef TSD_PLATFORM_MSVC
static WSADATA wsad;
#endif

static int port = -1;
static int server_started = 0;
static SOCKET sock;

static volatile int n_clients = 0;
static client_t clients[MAX_CLIENTS];
static unsigned char global_buf[BUFSIZE];
int global_buf_pos = 0;

static int add_client(SOCKET sock, struct in_addr addr, USHORT port)
{
	if (n_clients >= MAX_CLIENTS) {
#ifdef TSD_PLATFORM_MSVC
		output_message(MSG_ERROR, L"クライアント数が上限に達しているため接続できません(%S:%d)", inet_ntoa(addr), ntohs(port));
#else
		output_message(MSG_ERROR, "クライアント数が上限に達しているため接続できません(%s:%d)", inet_ntoa(addr), ntohs(port));
#endif
		return 0;
	}
#ifdef TSD_PLATFORM_MSVC
	output_message(MSG_NOTIFY, L"mod_tcpcast: クライアント(%S:%d)が接続されました", inet_ntoa(addr), ntohs(port));
#else
	output_message(MSG_NOTIFY, "mod_tcpcast: クライアント(%s:%d)が接続されました", inet_ntoa(addr), ntohs(port));
#endif
	clients[n_clients].sock = sock;
	clients[n_clients].addr = addr;
	clients[n_clients].port = port;
	clients[n_clients].buf_pos = 0;
	clients[n_clients].alive = 1;
	n_clients++;
	return 1;
}

static void shutdown_close_socket(SOCKET sock)
{
#ifdef TSD_PLATFORM_MSVC
	shutdown(sock, SD_BOTH);
	closesocket(sock);
#else
	shutdown(sock, SHUT_RDWR);
	close(sock);
#endif
}

static void remove_client(int client_id)
{
	shutdown_close_socket(clients[client_id].sock);
	if (n_clients > 1) {
		clients[client_id] = clients[n_clients - 1];
	}
	n_clients--;
}

static void remove_dead_clients()
{
	int i;
	for (i = 0; i < n_clients; ) { /* n_clients に volatile が要りそう？ */
		if ( ! clients[i].alive ) {
			remove_client(i);
#ifdef TSD_PLATFORM_MSVC
			output_message(MSG_NOTIFY, L"mod_tcpcast: クライアント(%S:%d)を切断しました",
				inet_ntoa(clients[i].addr), ntohs(clients[i].port));
#else
			output_message(MSG_NOTIFY, "mod_tcpcast: クライアント(%s:%d)を切断しました",
				inet_ntoa(clients[i].addr), ntohs(clients[i].port));
#endif
		} else {
			i++;
		}
	}
}

static void minimize_buf()
{
	int i, minimum_pos;

	minimum_pos = global_buf_pos;
	for (i = 0; i < n_clients; i++) {
		if (clients[i].buf_pos < minimum_pos) {
			minimum_pos = clients[i].buf_pos;
		}
	}

	memmove(global_buf, &global_buf[minimum_pos], global_buf_pos - minimum_pos);
	for (i = 0; i < n_clients; i++) {
		clients[i].buf_pos -= minimum_pos;
	}
	global_buf_pos -= minimum_pos;
}

static void close_all_clients()
{
	int i;
	for (i = 0; i < n_clients; i++) {
		shutdown_close_socket(clients[i].sock);
	}
}

static void send_to_all(const unsigned char *buf, size_t size)
{
	int i, ret, sendsize, diff;
	char recvchar;

	if (n_clients == 0) {
		//return;
	}

	/* 念のため受け取ったバッファが大きすぎるかをチェック */
	if (size > BUFSIZE) {
		size = BUFSIZE;
	}

	/* バッファが足りなければ古いデータを捨てる */
	diff = global_buf_pos + size - BUFSIZE;
	if (diff > 0) {
		for (i = 0; i < n_clients; i++) {
			clients[i].buf_pos -= diff;
			if (clients[i].buf_pos < 0) {
				clients[i].buf_pos = 0;
			}
		}
		memmove(global_buf, &global_buf[diff], global_buf_pos - diff);
		//output_message(MSG_WARNING, TSD_TEXT("Drop: %d (%d +%d)"), diff, global_buf_pos, size);
		global_buf_pos -= diff;
	}
	memcpy(&global_buf[global_buf_pos], buf, size);
	global_buf_pos += size;

	for (i = 0; i < n_clients; i++) {

		/* クライアントからrecvしたらcloseの合図 */
		ret = recv(clients[i].sock, &recvchar, 1, 0);
#ifdef TSD_PLATFORM_MSVC
		if (ret == SOCKET_ERROR) {
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
		if (ret < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
				output_message(MSG_WINSOCKERROR, TSD_TEXT("recv()に失敗しました"));
				clients[i].alive = 0;
			}
		} else if(ret > 0) {
			clients[i].alive = 0;
		}

		if (!clients[i].alive) {
			continue;
		}

		sendsize = global_buf_pos - clients[i].buf_pos;
		if (sendsize > MAX_MSG_SIZE) {
			sendsize = MAX_MSG_SIZE;
		} else if (sendsize == 0) {
			continue;
		}

		/* send */
		ret = send(clients[i].sock, (const char*)&global_buf[clients[i].buf_pos], sendsize, 0);
#ifdef TSD_PLATFORM_MSVC
		if (ret == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
				//printf("mod_tcpcast: PASS!\n");
			} else {
				output_message(MSG_WINSOCKERROR, TSD_TEXT("send()に失敗しました"));
				clients[i].alive = 0;
				break;
			}
		} else {
			clients[i].buf_pos += ret;
		}
	}
}

static void hook_open_stream()
{
	struct sockaddr_in addr;

	if (port == -1) {
		return;
	}

#ifdef TSD_PLATFORM_MSVC
	if (WSAStartup(MAKEWORD(2, 0), &wsad) != 0) {
		output_message(MSG_WINSOCKERROR, TSD_TEXT("WSAStartup()に失敗"));
		return;
	}
#endif

	sock = socket(AF_INET, SOCK_STREAM, 0);
#ifdef TSD_PLATFORM_MSVC
	if (sock == INVALID_SOCKET){
#else
	if (sock < 0) {
#endif
		output_message(MSG_WINSOCKERROR, TSD_TEXT("socketの作成に失敗"));
		WSACleanup();
		return;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
#ifdef TSD_PLATFORM_MSVC
	addr.sin_addr.S_un.S_addr = INADDR_ANY;
#else
	addr.sin_addr.s_addr = INADDR_ANY;
#endif

#ifdef TSD_PLATFORM_MSVC
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR){
#else
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#endif
		output_message(MSG_WINSOCKERROR, TSD_TEXT("bind()に失敗"));
		shutdown_close_socket(sock);
		WSACleanup();
		return;
	}

#ifdef TSD_PLATFORM_MSVC
	if (listen(sock, 5) == SOCKET_ERROR){
#else
	if (listen(sock, 5) < 0) {
#endif
		output_message(MSG_WINSOCKERROR, TSD_TEXT("listen()に失敗"));
		shutdown_close_socket(sock);
		WSACleanup();
		return;
	}

#ifdef TSD_PLATFORM_MSVC
	u_long val = 1;
	if (ioctlsocket(sock, FIONBIO, &val) != NO_ERROR) {
		output_message(MSG_WINSOCKERROR, TSD_TEXT("ソケットをノンブロッキングモードに設定できません(ioctlsocket)"));
#else
	int val = 1;
	if (ioctl(sock, FIONBIO, &val) < 0) {
		output_message(MSG_WINSOCKERROR, TSD_TEXT("ソケットをノンブロッキングモードに設定できません(ioctl)"));
#endif
		shutdown_close_socket(sock);
		WSACleanup();
		return;
	}

	server_started = 1;
}

static void hook_stream(const unsigned char* buf, const size_t size, const int encrypted)
{
	struct sockaddr_in client_addr;
#ifdef TSD_PLATFORM_MSVC
	int client_addr_len = sizeof(client_addr);
#else
	socklen_t client_addr_len = sizeof(client_addr);
#endif
	SOCKET sock_client;

	if (!server_started) {
		return;
	}

	/* 新しい接続があればaccept */
	while(1) {
		sock_client = accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
#ifdef TSD_PLATFORM_MSVC
		if (sock_client == INVALID_SOCKET) {
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
#else
		if (sock_client < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
#endif
				output_message(MSG_WINSOCKERROR, TSD_TEXT("accept()に失敗"));
			}
			break;
		}
		if (!add_client(sock_client, client_addr.sin_addr, client_addr.sin_port)) {
#ifdef TSD_PLATFORM_MSVC
			closesocket(sock_client);
#else
			close(sock_client);
#endif
		}
	}

	send_to_all(buf, size);

	remove_dead_clients();
	minimize_buf();
}

static void hook_close_stream()
{
	if (server_started) {
		close_all_clients();
		shutdown_close_socket(sock);
		WSACleanup();
	}
}

static void register_hooks()
{
	register_hook_open_stream(hook_open_stream);
	register_hook_stream(hook_stream);
	register_hook_close_stream(hook_close_stream);
}

static const TSDCHAR *set_port(const TSDCHAR *param)
{
#ifdef TSD_PLATFORM_MSVC
	port = _wtoi(param);
#else
	port = atoi(param);
#endif
	if (port <= 0 || port >= 65536) {
		return TSD_TEXT("指定されたTCPポート番号が不正です");
	}
	return NULL;
}

static cmd_def_t cmds[] = {
	{ TSD_TEXT("--tcpport"), TSD_TEXT("TCPのポート番号"), 1, set_port },
	{ NULL },
};

TSD_MODULE_DEF(
	mod_tcpcast,
	register_hooks,
	cmds,
	NULL
);
