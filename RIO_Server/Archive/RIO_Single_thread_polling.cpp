#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <queue>
//#include <concurrent_unordered_map.h>
//#include <concurrent_queue.h>
#include <unordered_map>
#include <queue>

using namespace std;

#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

//#include "..\..\SimpleIOCPServer\SimpleIOCPServer\protocol.h"
#include "../RIO_Server/protocol.h"

constexpr auto MAX_BUFFER = 1024;
constexpr auto VIEW_RANGE = 7;
constexpr auto MAX_USER = 10000;


constexpr auto MAX_RIO_RESULTS = 1024;
constexpr auto MAX_SEND_RQ_SIZE_PER_SOCKET = 512;
constexpr auto MAX_RECV_RQ_SIZE_PER_SOCKET = 512;
constexpr auto CQ_SIZE_FACTOR = 5000;
constexpr auto MAX_CQ_SIZE_PER_RIO_THREAD = (MAX_SEND_RQ_SIZE_PER_SOCKET + MAX_SEND_RQ_SIZE_PER_SOCKET)* CQ_SIZE_FACTOR;
RIO_EXTENSION_FUNCTION_TABLE gRIO = { 0, };

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

// rio buffer 재사용
struct COMPINFO
{
	EVENT_TYPE op;
	RIO_BUF *rio_buffer;
};

struct SOCKETINFO
{
	SOCKET	socket;
	int		id;

	// client당 하나의 rq
	RIO_RQ	req_queue;
	COMPINFO recv_info;

	// packet 재조립
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	bool	is_connected;

	char	name[MAX_ID_LEN];
	short	x, y;
	int		move_time;
	set <int> near_id;
};

// buffer 시작 주소
char* g_rio_buf_addr;
class Rio_Memory_Manager
{
	queue<int>		av_buff_slot_q;
	RIO_BUFFERID	rio_buff_id;
public:
	Rio_Memory_Manager()
	{
		DWORD buf_size = MAX_BUFFER * MAX_USER * 100;
		g_rio_buf_addr = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(), 0, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		rio_buff_id = gRIO.RIORegisterBuffer(g_rio_buf_addr, buf_size);
		for (unsigned i = 0; i < buf_size / MAX_BUFFER; ++i) av_buff_slot_q.push(i);
	}

	RIO_BUF* new_rio_buffer()
	{
		int slot_id;
		RIO_BUF *buf = new RIO_BUF;
		buf->BufferId = rio_buff_id;
		buf->Length = MAX_BUFFER;

		if (true == av_buff_slot_q.empty()) {
			cout << "Out of RIO buffer slot\n";
			DebugBreak();
		}
		slot_id = av_buff_slot_q.front();
		buf->Offset = slot_id * MAX_BUFFER;
		av_buff_slot_q.pop();

		return buf;
	}

	void delete_rio_buffer(RIO_BUF* buf)
	{
		av_buff_slot_q.push(buf->Offset / MAX_BUFFER);
		delete buf;
	}
};

Rio_Memory_Manager *g_rio_mm;

// 하나의 CQ로 모두 처리
RIO_CQ g_rio_cq;

//unordered_map <int, SOCKETINFO*> clients;
SOCKETINFO* clients[MAX_USER];
HANDLE	g_iocp;

int new_user_id = 0;

void error_display(const char* msg, int err_no)
{
	WCHAR* lpMsgBuf;
	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, err_no,
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		(LPTSTR)&lpMsgBuf, 0, NULL);
	cout << msg;
	wcout << L"에러 " << lpMsgBuf << endl;
	while (true);
	LocalFree(lpMsgBuf);
}

int Get_Zone_idx(int id) {
	auto cl = clients[id];

	int curr_zone_x = cl->x / ZONE_WIDTH_SIZE;
	int curr_zone_y = cl->y / ZONE_HEIGHT_SIZE;

	return curr_zone_x + (curr_zone_y * ZONE_ONELINE_SIZE);
}

bool Chk_Is_in_Zone(int clientid, int targetid) {
	int clientspaceid = Get_Zone_idx(clientid);
	int targetspaceid = Get_Zone_idx(targetid);
	if ((clientspaceid + ZONE_ONELINE_SIZE - 1 <= targetspaceid) && (targetspaceid <= clientspaceid + ZONE_ONELINE_SIZE + 1) ||
		(clientspaceid - 1 <= targetspaceid) && (targetspaceid <= clientspaceid + 1) ||
		(clientspaceid - ZONE_ONELINE_SIZE - 1 <= targetspaceid) && (targetspaceid <= clientspaceid - ZONE_ONELINE_SIZE + 1))
		return true;

	return false;
}

bool is_near(int a, int b)
{
	if (VIEW_RANGE < abs(clients[a]->x - clients[b]->x)) return false;
	if (VIEW_RANGE < abs(clients[a]->y - clients[b]->y)) return false;
	return true;
}

void send_packet(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	int packet_size = packet[0];
	COMPINFO* comp_info = new COMPINFO;
	comp_info->op = EV_SEND;
	comp_info->rio_buffer = g_rio_mm->new_rio_buffer();
	memcpy(g_rio_buf_addr + comp_info->rio_buffer->Offset, packet, packet_size);
	comp_info->rio_buffer->Length = packet_size;

	gRIO.RIOSend(clients[id]->req_queue, comp_info->rio_buffer, 1, 0, comp_info);
}

void send_login_ok_packet(int id)
{
	sc_packet_login_ok packet;
	packet.id = id;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_OK;
	packet.x = clients[id]->x;
	packet.y = clients[id]->y;
	packet.hp = 100;
	packet.level = 1;
	packet.exp = 1;
	send_packet(id, &packet);
}

void send_login_fail(int id)
{
	sc_packet_login_fail packet;
	packet.size = sizeof(packet);
	packet.type = SC_LOGIN_FAIL;
	send_packet(id, &packet);
}

void send_put_object_packet(int client, int new_id)
{
	sc_packet_put_object packet;
	packet.id = new_id;
	packet.size = sizeof(packet);
	packet.type = SC_PUT_OBJECT;
	packet.x = clients[new_id]->x;
	packet.y = clients[new_id]->y;
	packet.o_type = 1;
	send_packet(client, &packet);

	if (client == new_id) return;
	clients[client]->near_id.insert(new_id);
}

void send_pos_packet(int client, int mover)
{
	sc_packet_pos packet;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = clients[mover]->x;
	packet.y = clients[mover]->y;
	packet.move_time = clients[client]->move_time;

	if ((client == mover) || (0 != clients[client]->near_id.count(mover))) {
		send_packet(client, &packet);
	}
	else {
		send_put_object_packet(client, mover);
	}
}

void send_remove_object_packet(int client, int leaver)
{
	sc_packet_remove_object packet;
	packet.id = leaver;
	packet.size = sizeof(packet);
	packet.type = SC_REMOVE_OBJECT;
	send_packet(client, &packet);

	clients[client]->near_id.erase(leaver);
}

bool is_near_id(int player, int other)
{
	return (0 != clients[player]->near_id.count(other));
}

void Disconnect(int id)
{
	clients[id]->is_connected = false;
	closesocket(clients[id]->socket);
	for (auto& cl : clients) {
		if (cl == nullptr) continue;
		if (cl->id == id) continue;
		if (false == Chk_Is_in_Zone(id, cl->id)) continue;
		if (true == cl->is_connected)
			send_remove_object_packet(cl->id, id);
	}
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	auto old_vl = clients[id]->near_id;
	switch (dir) {
	case D_UP: if (y > 0) y--;
		break;
	case D_DOWN: if (y < WORLD_HEIGHT - 1) y++;
		break;
	case D_LEFT: if (x > 0) x--;
		break;
	case D_RIGHT: if (x < WORLD_WIDTH - 1) x++;
		break;
	case 99:
		x = rand() % WORLD_WIDTH;
		y = rand() % WORLD_HEIGHT;
		break;
	default: cout << "Invalid Direction Error\n";
		while (true);
	}

	clients[id]->x = x;
	clients[id]->y = y;

	set <int> new_vl;
	for (auto& cl : clients) {
		if (cl == nullptr) continue;

		int other = cl->id;
		if (id == other) continue;
		if (false == clients[other]->is_connected) continue;
		if (false == Chk_Is_in_Zone(id, other)) continue;

		if (true == is_near(id, other)) new_vl.insert(other);
	}

	send_pos_packet(id, id);
	for (auto cl : old_vl) {
		if (0 != new_vl.count(cl)) {
				send_pos_packet(cl, id);
		}
		else
		{
			send_remove_object_packet(id, cl);
			send_remove_object_packet(cl, id);
		}
	}
	for (auto cl : new_vl) {
		if (0 == old_vl.count(cl)) {
			send_put_object_packet(id, cl);
			send_put_object_packet(cl, id);
		}
	}
}

void ProcessLogin(int user_id, char* id_str)
{
	for (auto cl : clients) {
		if (cl == nullptr) continue;
		if (0 == strcmp(cl->name, id_str)) {
			send_login_fail(user_id);
			Disconnect(user_id);
			return;
		}
	}
	strcpy_s(clients[user_id]->name, id_str);
	send_login_ok_packet(user_id);
	clients[user_id]->is_connected = true;

	for (auto& cl : clients) {
		if (cl == nullptr) continue;

		int other_player = cl->id;
		if (false == clients[other_player]->is_connected) continue;
		if (false == Chk_Is_in_Zone(user_id, other_player)) continue;

		if (true == is_near(other_player, user_id)) {
			send_put_object_packet(other_player, user_id);
			if (other_player != user_id) {
				send_put_object_packet(user_id, other_player);
			}
		}
	}
}

void ProcessPacket(int id, void* buff)
{
	char* packet = reinterpret_cast<char*>(buff);
	switch (packet[1]) {
	case CS_LOGIN: {
		cs_packet_login* login_packet = reinterpret_cast<cs_packet_login*>(packet);
		ProcessLogin(id, login_packet->id);
	}
				 break;
	case CS_MOVE: {
		cs_packet_move* move_packet = reinterpret_cast<cs_packet_move*>(packet);
		clients[id]->move_time = move_packet->move_time;

		ProcessMove(id, move_packet->direction);
	}
				break;
	case CS_ATTACK:
		break;
	case CS_CHAT:
		break;
	case CS_LOGOUT:
		break;
	case CS_TELEPORT:
		ProcessMove(id, 99);
		break;
	default: cout << "Invalid Packet Type Error\n";
		while (true);
	}
}


int main()
{
	wcout.imbue(std::locale("korean"));

	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	SOCKET listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(SOCKADDR_IN));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*) & serverAddr, sizeof(SOCKADDR_IN))) {
		error_display("WSARecv Error :", WSAGetLastError());
	}

	GUID functionTableId = WSAID_MULTIPLE_RIO;
	DWORD dwBytes = 0;
	if (WSAIoctl(listenSocket, SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER, &functionTableId, sizeof(GUID),
		(void**)&gRIO, sizeof(gRIO), &dwBytes, NULL, NULL)) {
		cout << "Cant Allocate Buffer Page\n";
		DebugBreak();
		return false;
	}

	listen(listenSocket, SOMAXCONN);
	g_rio_cq = gRIO.RIOCreateCompletionQueue(MAX_CQ_SIZE_PER_RIO_THREAD, 0);

	SOCKET clientsocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
	WSAOVERLAPPED ov;
	ZeroMemory(&ov, sizeof(ov));
	HANDLE accept_ev = ov.hEvent = WSACreateEvent();
	char acceptBuf[2 * (sizeof(SOCKADDR_IN) + 16)];
	AcceptEx(listenSocket,
		clientsocket,
		acceptBuf, NULL,
		sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
		NULL, &ov);

	int client_id = 0;

	g_rio_mm = new Rio_Memory_Manager;

	cout << "RUN\n";

	while (true) {
		if (client_id < MAX_USER) {
			int id = WSAWaitForMultipleEvents(1, &ov.hEvent, FALSE, 0, FALSE);
			if (id == 0) {
				WSAResetEvent(accept_ev);
				auto cl = new SOCKETINFO{};
				cl->id = client_id;
				cl->socket = clientsocket;
				cl->prev_packet_size = 0;
				cl->recv_info.op = EV_RECV;
				cl->recv_info.rio_buffer = g_rio_mm->new_rio_buffer();
				cl->x = rand() % WORLD_WIDTH;
				cl->y = rand() % WORLD_HEIGHT;
				cl->req_queue = gRIO.RIOCreateRequestQueue(clientsocket, MAX_RECV_RQ_SIZE_PER_SOCKET, 1, MAX_SEND_RQ_SIZE_PER_SOCKET, 1,
					g_rio_cq, g_rio_cq, (PVOID)static_cast<ULONGLONG>(client_id));
				clients[client_id] = cl;

				gRIO.RIOReceive(cl->req_queue, cl->recv_info.rio_buffer, 1, NULL, &cl->recv_info);


				clientsocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_REGISTERED_IO);
				ZeroMemory(&ov, sizeof(ov));
				ov.hEvent = accept_ev;
				AcceptEx(listenSocket,
					clientsocket,
					acceptBuf, NULL,
					sizeof(SOCKADDR_IN) + 16, sizeof(SOCKADDR_IN) + 16,
					NULL, &ov);

				client_id++;
			}
		}

		RIORESULT results[MAX_RIO_RESULTS];
		ULONG numResults = gRIO.RIODequeueCompletion(g_rio_cq, results, MAX_RIO_RESULTS);

		for (ULONG i = 0; i < numResults; ++i) {
			COMPINFO* comp_info = reinterpret_cast<COMPINFO*>(results[i].RequestContext);
			EVENT_TYPE op = comp_info->op;
			ULONG num_byte = results[i].BytesTransferred;
			int user_id = static_cast<int>(results[i].SocketContext);

			if (0 == num_byte) {
				closesocket(clients[user_id]->socket);
				clients[user_id]->is_connected = false;
				// delete는 하지 않았으나 재사용 처리를 하지 않았기 때문에 추후에 적용해야한다.
				g_rio_mm->delete_rio_buffer(clients[user_id]->recv_info.rio_buffer);

				if (op == EV_SEND) {
					// Send buf해제
					g_rio_mm->delete_rio_buffer(comp_info->rio_buffer);
					delete comp_info;
				}
			}
			else if (EV_RECV == op) {
				// Recv data 시작주소
				char* p = g_rio_buf_addr + clients[user_id]->recv_info.rio_buffer->Offset;
				int remain = num_byte;
				int packet_size;
				int prev_packet_size = clients[user_id]->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else packet_size = clients[user_id]->pre_net_buf[0];
				while (remain > 0) {
					if (0 == packet_size) packet_size = p[0];
					int required = packet_size - prev_packet_size;
					if (required <= remain) {
						memcpy(clients[user_id]->pre_net_buf + prev_packet_size, p, required);
						ProcessPacket(user_id, clients[user_id]->pre_net_buf);
						remain -= required;
						p += required;
						prev_packet_size = 0;
						packet_size = 0;
					}
					else {
						memcpy(clients[user_id]->pre_net_buf + prev_packet_size, p, remain);
						prev_packet_size += remain;
						remain = 0;
					}
				}
				clients[user_id]->prev_packet_size = prev_packet_size;

				// 다시 Recv
				gRIO.RIOReceive(clients[user_id]->req_queue, clients[user_id]->recv_info.rio_buffer, 1, 0, &clients[user_id]->recv_info);
			}
			else if (EV_SEND == op) {
				g_rio_mm->delete_rio_buffer(comp_info->rio_buffer);
				delete comp_info;
			}
			else {
				cout << "Unknown Event Type :" << op << endl;
				while (true);
			}
		}
	}

	closesocket(listenSocket);
	WSACleanup();
}


