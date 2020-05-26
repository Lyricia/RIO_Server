#include <iostream>
#include <map>
#include <thread>
#include <set>
#include <mutex>
#include <queue>
#include <concurrent_unordered_map.h>
#include <concurrent_queue.h>
#include <atomic>

using namespace std;

#include <WS2tcpip.h>
#include <MSWSock.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

//#include "..\..\SimpleIOCPServer\SimpleIOCPServer\protocol.h"
#include "../RIO_Server/protocol.h"

constexpr auto MAX_THREAD = 4;

constexpr auto MAX_BUFFER = 1024;
constexpr auto VIEW_RANGE = 7;
constexpr auto MAX_USER = 10000;


constexpr auto MAX_RIO_RESULTS = 1024;
constexpr auto MAX_SEND_RQ_SIZE_PER_SOCKET = 512;
constexpr auto MAX_RECV_RQ_SIZE_PER_SOCKET = 512;
constexpr auto CQ_SIZE_FACTOR = 5000;
constexpr auto MAX_CQ_SIZE_PER_RIO_THREAD = (MAX_SEND_RQ_SIZE_PER_SOCKET + MAX_SEND_RQ_SIZE_PER_SOCKET) * CQ_SIZE_FACTOR;
RIO_EXTENSION_FUNCTION_TABLE gRIO = { 0, };

enum EVENT_TYPE { EV_RECV, EV_SEND, EV_MOVE, EV_PLAYER_MOVE_NOTIFY, EV_MOVE_TARGET, EV_ATTACK, EV_HEAL };

// rio buffer 재사용
struct COMPINFO
{
	EVENT_TYPE op;
	RIO_BUF* rio_buffer;
};

struct SOCKETINFO
{
	SOCKET	socket;
	int		id;

	// client당 하나의 rq
	RIO_RQ	req_queue;
	mutex	req_queue_lock;
	COMPINFO recv_info;

	// packet 재조립
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	atomic_bool	is_connected;

	char	name[MAX_ID_LEN];
	short	x, y;
	int		move_time;

	set <int> near_id;
	mutex near_lock;		// 시야처리 lock
};

// buffer 시작 주소
char* g_rio_buf_addr;
class Rio_Memory_Manager
{
	Concurrency::concurrent_queue <int>		av_buff_slot_q;
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
		RIO_BUF* buf = new RIO_BUF;
		buf->BufferId = rio_buff_id;
		buf->Length = MAX_BUFFER;

		int slot_id;
		if (false == av_buff_slot_q.try_pop(slot_id)) {
			cout << "Out of RIO buffer slot\n";
			DebugBreak();
		}
		buf->Offset = slot_id * MAX_BUFFER;
		return buf;
	}

	void delete_rio_buffer(RIO_BUF* buf)
	{
		av_buff_slot_q.push(buf->Offset / MAX_BUFFER);
		delete buf;
	}
};

Rio_Memory_Manager* g_rio_mm;

// 하나의 CQ로 모두 처리
RIO_CQ g_rio_cq[MAX_THREAD * 2];
mutex g_rio_cq_lock[MAX_THREAD * 2];

//Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
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

	// rq lock
	clients[id]->req_queue_lock.lock();
	gRIO.RIOSend(clients[id]->req_queue, comp_info->rio_buffer, 1, 0, comp_info);
	clients[id]->req_queue_lock.unlock();
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
	lock_guard<mutex>lg{ clients[client]->near_lock };
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

	clients[client]->near_lock.lock();
	if ((client == mover) || (0 != clients[client]->near_id.count(mover))) {
		clients[client]->near_lock.unlock();
		send_packet(client, &packet);
	}
	else {
		clients[client]->near_lock.unlock();
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

	lock_guard<mutex>lg{ clients[client]->near_lock };
	clients[client]->near_id.erase(leaver);
}

bool is_near_id(int player, int other)
{
	lock_guard <mutex> gl{ clients[player]->near_lock };
	return (0 != clients[player]->near_id.count(other));
}

void Disconnect(int id)
{
	closesocket(clients[id]->socket);
	clients[id]->is_connected = false;
	for (auto& cl : clients) {
		if (cl == nullptr) continue;
		if (cl->id == id) continue;
		if (true == cl->is_connected)
			send_remove_object_packet(cl->id, id);
	}
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	clients[id]->near_lock.lock();
	auto old_vl = clients[id]->near_id;
	clients[id]->near_lock.unlock();
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
		if (cl->is_connected == false) continue;
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

void do_worker(int id)
{
	int tid = id;
	while (true) {
		DWORD num_byte;
		ULONGLONG key64;
		PULONG_PTR p_key = &key64;
		WSAOVERLAPPED* p_over;

		// 완료통지가 중요
		GetQueuedCompletionStatus(g_iocp, &num_byte, p_key, &p_over, INFINITE);
		UINT pqcs_thread_id = static_cast<UINT>(key64);

		// 완료통지 후 cq에 쌓인 result 처리
		RIORESULT results[MAX_RIO_RESULTS];
		g_rio_cq_lock[pqcs_thread_id].lock();
		ULONG numResults = gRIO.RIODequeueCompletion(g_rio_cq[pqcs_thread_id], results, MAX_RIO_RESULTS);
		g_rio_cq_lock[pqcs_thread_id].unlock();

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
				int packet_size = 0;
				int prev_packet_size = clients[user_id]->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else
					packet_size = clients[user_id]->pre_net_buf[0];

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

				/// 여기서 notify를 하면 recv의 순서가 엇갈린다
				//gRIO.RIONotify(g_rio_cq);
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
		// 전체가 끝났으니 여기서 다시 완료통지 (PGCS)
		gRIO.RIONotify(g_rio_cq[pqcs_thread_id]);
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


	SOCKADDR_IN clientAddr;
	int addrLen = sizeof(SOCKADDR_IN);
	memset(&clientAddr, 0, addrLen);

	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	vector <thread> worker_threads;
	for (int i = 0; i < MAX_THREAD; ++i)
		worker_threads.emplace_back(do_worker, i);

	// 스레드마다 notify desc를 한개씩 가지도록
	WSAOVERLAPPED iocp_over[MAX_THREAD];
	RIO_NOTIFICATION_COMPLETION rio_noti[MAX_THREAD];
	for (int i = 0; i < MAX_THREAD * 2; ++i) {
		ZeroMemory(&iocp_over[i], sizeof(iocp_over));
		rio_noti[i].Type = RIO_IOCP_COMPLETION;
		rio_noti[i].Iocp.IocpHandle = g_iocp;
		rio_noti[i].Iocp.Overlapped = &iocp_over[i];
		rio_noti[i].Iocp.CompletionKey = (void*)i;		// 몇번 스레드인지 알 수 있도록 cq의 id key 등록
		g_rio_cq[i] = gRIO.RIOCreateCompletionQueue(MAX_CQ_SIZE_PER_RIO_THREAD, &rio_noti[i]);
	}
	g_rio_mm = new Rio_Memory_Manager;

	while (true) {
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*) & clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}

		int user_id = new_user_id++;
		SOCKETINFO* new_player = new SOCKETINFO;
		new_player->id = user_id;
		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->recv_info.op = EV_RECV;
		new_player->recv_info.rio_buffer = g_rio_mm->new_rio_buffer();
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;
		clients[user_id] = new_player;

		new_player->req_queue = gRIO.RIOCreateRequestQueue(
			clientSocket,
			MAX_RECV_RQ_SIZE_PER_SOCKET, 1,
			MAX_SEND_RQ_SIZE_PER_SOCKET, 1,
			g_rio_cq[new_user_id % (MAX_THREAD * 2)], g_rio_cq[new_user_id % (MAX_THREAD * 2)],
			(PVOID)static_cast<ULONGLONG>(user_id));

		gRIO.RIOReceive(new_player->req_queue, new_player->recv_info.rio_buffer, 1, NULL, &new_player->recv_info);
		gRIO.RIONotify(g_rio_cq[new_user_id % (MAX_THREAD * 2)]);
	}

	for (auto& th : worker_threads)
		th.join();

	closesocket(listenSocket);
	WSACleanup();
}

