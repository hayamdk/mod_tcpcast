#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#define MAX_CLIENTS 32
#define MAX_MSG_SIZE 188*1024
#define BUFSIZE 188*6*1024

#pragma comment(lib, "tsdump.lib")
#pragma comment(lib, "ws2_32.lib")

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <sys/timeb.h>
#include <time.h>
#include <inttypes.h>

#define IN_SHARED_MODULE
#include "modules_def.h"

typedef struct{
	struct in_addr addr;
	USHORT port;
	SOCKET sock;
	int buf_pos;
	int alive;
} client_t;

static int port = -1;
static int server_started = 0;
static WSADATA wsad;
static SOCKET sock;

static volatile int n_clients = 0;
static client_t clients[MAX_CLIENTS];
static unsigned char global_buf[BUFSIZE];
int global_buf_pos = 0;

static int add_client(SOCKET sock, struct in_addr addr, USHORT port)
{
	if (n_clients >= MAX_CLIENTS) {
		//fprintf(stderr, "mod_tcpcast: �N���C�A���g��������ɒB���Ă��邽�ߐڑ��ł��܂���(%s:%d)\n", inet_ntoa(addr), ntohs(port));
		output_message(MSG_ERROR, L"�N���C�A���g��������ɒB���Ă��邽�ߐڑ��ł��܂���(%S:%d)", inet_ntoa(addr), ntohs(port));
		return 0;
	}
	//fprintf(stdout, "mod_tcpcast: �N���C�A���g(%s:%d)���ڑ�����܂���\n", inet_ntoa(addr), ntohs(port));
	output_message(MSG_NOTIFY, L"mod_tcpcast: �N���C�A���g(%S:%d)���ڑ�����܂���\n", inet_ntoa(addr), ntohs(port));
	clients[n_clients].sock = sock;
	clients[n_clients].addr = addr;
	clients[n_clients].port = port;
	clients[n_clients].buf_pos = 0;
	clients[n_clients].alive = 1;
	n_clients++;
	return 1;
}

static void remove_client(int client_id)
{
	shutdown(clients[client_id].sock, SD_BOTH);
	closesocket(clients[client_id].sock);
	if (n_clients > 1) {
		clients[client_id] = clients[n_clients - 1];
	}
	n_clients--;
}

static void remove_dead_clients()
{
	int i;
	for (i = 0; i < n_clients; ) { /* n_clients �� volatile ���v�肻���H */
		if ( ! clients[i].alive ) {
			remove_client(i);
			//fprintf(stderr, "mod_tcpcast: �N���C�A���g(%s:%d)��ؒf���܂���\n",
			//	inet_ntoa(clients[i].addr), ntohs(clients[i].port));
			output_message(MSG_NOTIFY, L"mod_tcpcast: �N���C�A���g(%S:%d)��ؒf���܂���\n",
				inet_ntoa(clients[i].addr), ntohs(clients[i].port));
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
		shutdown(clients[i].sock, SD_BOTH);
		closesocket(clients[i].sock);
	}
}

static void send_to_all(const unsigned char *buf, size_t size)
{
	int i, ret, sendsize, diff;
	char recvchar;

	if (n_clients == 0) {
		//return;
	}

	/* �O�̂��ߎ󂯎�����o�b�t�@���傫�����邩���`�F�b�N */
	if (size > BUFSIZE) {
		size = BUFSIZE;
	}

	/* �o�b�t�@������Ȃ���ΌÂ��f�[�^���̂Ă� */
	diff = global_buf_pos + size - BUFSIZE;
	if (diff > 0) {
		for (i = 0; i < n_clients; i++) {
			clients[i].buf_pos -= diff;
			if (clients[i].buf_pos < 0) {
				clients[i].buf_pos = 0;
			}
		}
		memmove(global_buf, &global_buf[diff], global_buf_pos - diff);
		global_buf_pos -= diff;
		//printf("mod_tcpcast: DROP! (bytes=%d)\n", diff);
	}
	memcpy(&global_buf[global_buf_pos], buf, size);
	global_buf_pos += size;

	for (i = 0; i < n_clients; i++) {

		/* �N���C�A���g����recv������close�̍��} */
		ret = recv(clients[i].sock, &recvchar, 1, 0);
		if (ret == SOCKET_ERROR) {
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
				//print_err(L"recv()", WSAGetLastError());
				output_message(MSG_WINSOCKERROR, L"recv()�Ɏ��s���܂���");
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
		if (ret == SOCKET_ERROR) {
			if (WSAGetLastError() == WSAEWOULDBLOCK) {
				//printf("mod_tcpcast: PASS!\n");
			} else {
				//print_err(L"send()", WSAGetLastError());
				output_message(MSG_WINSOCKERROR, L"send()�Ɏ��s���܂���");
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

	if (WSAStartup(MAKEWORD(2, 0), &wsad) != 0) {
		//print_err(L"WSAStartup()�Ɏ��s", WSAGetLastError());
		output_message(MSG_WINSOCKERROR, L"WSAStartup()�Ɏ��s");
		return;
	}
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == INVALID_SOCKET){
		//print_err(L"socket�̍쐬�Ɏ��s", WSAGetLastError());
		output_message(MSG_WINSOCKERROR, L"socket�̍쐬�Ɏ��s");
		WSACleanup();
		return;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.S_un.S_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR){
		//print_err(L"bind()�Ɏ��s", WSAGetLastError());
		output_message(MSG_WINSOCKERROR, L"bind()�Ɏ��s");
		shutdown(sock, SD_BOTH);
		closesocket(sock);
		WSACleanup();
		return;
	}

	if (listen(sock, 5) == SOCKET_ERROR){
		//print_err(L"listen()�Ɏ��s", WSAGetLastError());
		output_message(MSG_WINSOCKERROR, L"listen()�Ɏ��s");
		shutdown(sock, SD_BOTH);
		closesocket(sock);
		WSACleanup();
		return;
	}

	u_long val = 1;
	if (ioctlsocket(sock, FIONBIO, &val) != NO_ERROR) {
		//print_err(L"�\�P�b�g���m���u���b�L���O���[�h�ɐݒ�ł��܂���", WSAGetLastError());
		output_message(MSG_WINSOCKERROR, L"�\�P�b�g���m���u���b�L���O���[�h�ɐݒ�ł��܂���(ioctlsocket)");
		shutdown(sock, SD_BOTH);
		closesocket(sock);
		WSACleanup();
		return;
	}

	server_started = 1;
}

static void hook_stream(const unsigned char* buf, const size_t size, const int encrypted)
{
	struct sockaddr_in client_addr;
	int client_addr_len = sizeof(client_addr);
	SOCKET sock_client;

	if (!server_started) {
		return;
	}

	/* �V�����ڑ��������accept */
	while(1) {
		sock_client = accept(sock, (struct sockaddr *)&client_addr, &client_addr_len);
		if (sock_client == INVALID_SOCKET) {
			if (WSAGetLastError() != WSAEWOULDBLOCK) {
				//print_err(L"accept()�Ɏ��s", WSAGetLastError());
				output_message(MSG_WINSOCKERROR, L"accept()�Ɏ��s");
			}
			break;
		}
		if (!add_client(sock_client, client_addr.sin_addr, client_addr.sin_port)) {
			closesocket(sock_client);
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
		shutdown(sock, SD_BOTH);
		closesocket(sock);
		WSACleanup();
	}
}

static void register_hooks()
{
	register_hook_open_stream(hook_open_stream);
	register_hook_stream(hook_stream);
	register_hook_close_stream(hook_close_stream);
}

static const WCHAR *set_port(const WCHAR *param)
{
	port = _wtoi(param);
	if (port <= 0 || port >= 65536) {
		return L"�w�肳�ꂽTCP�|�[�g�ԍ����s���ł�";
	}
	return NULL;
}

static cmd_def_t cmds[] = {
	{ L"--tcpport", L"TCP�̃|�[�g�ԍ�", 1, set_port },
	NULL,
};

MODULE_DEF module_def_t mod_tcpcast = {
	TSDUMP_MODULE_V2,
	L"mod_tcpcast",
	register_hooks,
	cmds
};