#include <iostream>
#include <map>
#include <thread>
//#include <set>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <concurrent_unordered_map.h>
#include <concurrent_unordered_set.h>
#include <concurrent_queue.h>

#include <WS2tcpip.h>
#include <MSWSock.h>

#include <immintrin.h>

//#include "lfset.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

//#include "..\..\SimpleIOCPServer\SimpleIOCPServer\protocol.h"
#include "../RIO_Server/protocol.h"
#include "ConstDefines.h"

using namespace std;

class BufferPiece {
public:
	char* ptr = nullptr;
	int idx = 0;
};

class BufferManager {
public:
	char* startPtr = nullptr;
	BufferPiece PieceList[NUMPIECE]{};
	bool PieceinUse[NUMPIECE]{};

	bool CAS(bool* addr, bool expected, bool new_val)
	{
		return atomic_compare_exchange_strong(reinterpret_cast<volatile std::atomic_bool*>(addr), &expected, new_val);
	}

	int GetFreeBufferPieceIdx(char*& bufferptr) {
		int i = 0;
		while (true) {
			if (CAS(&PieceinUse[i], false, true)) {
				bufferptr = startPtr + i * BUFPIECESIZE;
				return i;
			}
			i++;
			if (i > NUMPIECE)
				i = 0;
		}
	}

	void ReleaseUsedBufferPiece(int idx) {
		while (CAS(&PieceinUse[idx], true, false)) {}
		//PieceinUse[idx] = false;
	}
};

class CustomTX {
public:
	int abortcnt = 0;
	bool isLocked = false;
	mutex gLock;

	void txStart(mutex& lock) 
	{
		while (true) {
			while (isLocked);

			if (abortcnt > MAX_ABORT_COUNT) {
				cout << "1" << flush;
				gLock.lock();
				isLocked = true;
				return;
			}

			unsigned stat = _xbegin();
			if (_XBEGIN_STARTED == stat) {
				if (isLocked)
					_xabort(0xff);
				return;
			}
			else 
				abortcnt++;
		}
	}

	void txEnd(mutex& lock) 
	{
		if (false == isLocked) {
			_xend();
		}
		else {
			gLock.unlock();
			isLocked = false;
		}
	}
};

CustomTX gTX;


// rio buffer 재사용
struct COMPINFO2
{
	EVENT_TYPE op;
	RIO_BUF* rio_buffer;
};

struct SOCKETINFO
{
	SOCKET	socket;
	int		id;

	// packet 재조립
	char	pre_net_buf[MAX_BUFFER];
	int		prev_packet_size;
	atomic_bool	is_connected;

	char	name[MAX_ID_LEN];
	short	x, y;
	int		seq_no;
	int		curr_zone_idx = 0;

	Concurrency::concurrent_unordered_set <int> view_list;
	mutex view_list_lock;		// 시야처리 lock

	// client당 하나의 rq
	//COMPINFO recv_info;
	RIO_RQ	req_queue;
	mutex	req_queue_lock;

	char* RioBufferPointer;
	char* RioSendBufferPtr;
	RIO_BUFFERID RioBufferId;
	BufferManager RioBufferMng;

	atomic_int storedmsgcnt = 0;
	chrono::steady_clock::time_point last_msg_time;
};

// buffer 시작 주소
char* g_rio_buf_addr;

struct RioIoContext : public RIO_BUF
{
	RioIoContext(SOCKETINFO* client, EVENT_TYPE ioType) : clientSession(client), IoType(ioType) {}

	SOCKETINFO* clientSession;
	EVENT_TYPE	IoType;
	int	SendBufIdx = -1;
};

class Rio_Memory_Manager
{
	//RIO_BUF* BuffSlot[MAX_USER * 100];
	Concurrency::concurrent_queue <int>		av_buff_slot_q;
	RIO_BUFFERID	rio_buff_id;
public:
	Rio_Memory_Manager()
	{
		DWORD buf_size = MAX_BUFFER * MAX_USER * 100;
		g_rio_buf_addr = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(), 0, buf_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		rio_buff_id = gRIO.RIORegisterBuffer(g_rio_buf_addr, buf_size);
		for (unsigned i = 0; i < buf_size / MAX_BUFFER; ++i) {
			av_buff_slot_q.push(i);
		}
	}

	//void* GetBuffPtr() { return BuffSlot; }

	RIO_BUF* new_rio_buffer()
	{
		int slot_id;
		if (false == av_buff_slot_q.try_pop(slot_id)) {
			cout << "Out of RIO buffer slot\n";
			DebugBreak();
		}

		RIO_BUF* buf = nullptr;
		try {
			buf = new RIO_BUF;
		}
		catch (bad_alloc ex)
		{
			cout << "new op failed\n";
			cout << ex.what() << endl;
			while (buf == nullptr) {
				buf = new RIO_BUF;
			}
		}

		buf->BufferId = rio_buff_id;
		buf->Length = MAX_BUFFER;
		buf->Offset = slot_id * MAX_BUFFER;
		return buf;
	}

	void delete_rio_buffer(RIO_BUF* buf)
	{
		av_buff_slot_q.push(buf->Offset / MAX_BUFFER);
		//delete buf;
	}
};

class CZone {
public:
	int ZoneIdx = -1;
	unordered_set<int> NearZoneList;
	unordered_set<int> Zone_Client_List;
	mutex Zone_lock;

	void SetNearZoneList() {
		unordered_set<int> tmp;

		int zonex = (ZoneIdx % ZONE_ONELINE_SIZE);
		int zoney = ZoneIdx / ZONE_ONELINE_SIZE;
		for (int j = -1; j < 2; ++j) {
			for (int i = -1; i < 2; ++i) {
				if ((zoney + i) < 0 || (zoney + i) >= ZONE_ONELINE_SIZE) continue;
				if ((zonex + j) < 0 || (zonex + j) >= ZONE_ONELINE_SIZE) continue;

				tmp.insert(ZoneIdx + j + (ZONE_ONELINE_SIZE * i));
			}
		}

		for (auto i : tmp) {
			if (i < 0 || i >= ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE) continue;

			NearZoneList.insert(i);
		}
	}

	void Insert(int id) {
		Zone_lock.lock();
		//gTX.txStart(Zone_lock);
		Zone_Client_List.insert(id);
		//gTX.txEnd(Zone_lock);
		Zone_lock.unlock();
	}

	void Erase(int id) {
		Zone_lock.lock();
		//gTX.txStart(Zone_lock);
		Zone_Client_List.erase(id);
		//gTX.txEnd(Zone_lock);
		Zone_lock.unlock();
	}
};

//Rio_Memory_Manager* g_rio_mm;

// 하나의 CQ로 모두 처리
RIO_RQ thread_rio_rq[MAX_THREAD];
RIO_CQ g_rio_cq[MAX_THREAD];
//RIO_CQ g_rio_cq;
mutex g_rio_cq_lock;

//Concurrency::concurrent_unordered_map <int, SOCKETINFO*> clients;
Concurrency::concurrent_queue<int> Enable_Clients;
SOCKETINFO* clients[MAX_USER];

//unordered_set<int> Zone[ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE];
//mutex Zone_lock[ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE];
CZone Zone[ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE];
//HANDLE	g_iocp;

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

int Get_Zone_idx(int id)
{
	auto& cl = clients[id];
	int curr_zone_x = cl->x / ZONE_WIDTH_SIZE;
	int curr_zone_y = cl->y / ZONE_HEIGHT_SIZE;
	return curr_zone_x + (curr_zone_y * ZONE_ONELINE_SIZE);
}

bool Chk_Is_in_Zone(int clientid, int targetid)
{
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

#pragma optimize("", off)
//void send_packet(int id, void* buff)
//{
//	char* packet = reinterpret_cast<char*>(buff);
//	int packet_size = packet[0];
//	COMPINFO* comp_info = nullptr;
//	try {
//		comp_info = new COMPINFO;
//	}
//	catch (bad_alloc ex)
//	{
//		cout << "new op failed\n";
//		cout << ex.what() << endl;
//		while (comp_info == nullptr) {
//			comp_info = new COMPINFO;
//		}
//	}
//
//	comp_info->op = EV_SEND;
//	comp_info->rio_buffer = g_rio_mm->new_rio_buffer();
//	memcpy(g_rio_buf_addr + comp_info->rio_buffer->Offset, packet, packet_size);
//	comp_info->rio_buffer->Length = packet_size;
//
//	// rq lock
//	clients[id]->req_queue_lock.lock();
//	gRIO.RIOSend(clients[id]->req_queue, comp_info->rio_buffer, 1, 0, comp_info);
//	clients[id]->req_queue_lock.unlock();
//}

void send_packet(int id, void* buff)
{
	auto& client = clients[id];
	unsigned char* p = reinterpret_cast<unsigned char*>(buff);
	char* PiecePtr = nullptr;
	int PieceIdx = -1;

	int packetlen = p[0];
	if (packetlen > BUFPIECESIZE)
		p[-1] = 0;

	PieceIdx = client->RioBufferMng.GetFreeBufferPieceIdx(PiecePtr);
	memcpy(PiecePtr, buff, packetlen);

	RioIoContext* sendContext = new RioIoContext(client, EVENT_TYPE::EV_SEND);
	sendContext->BufferId = client->RioBufferId;
	sendContext->Length = packetlen;
	sendContext->Offset = SEND_BUFFER_OFFSET + PieceIdx * BUFPIECESIZE;
	sendContext->SendBufIdx = PieceIdx;

	DWORD sendbytes = 0;
	DWORD flags = 0;

	client->req_queue_lock.lock();
	if (!gRIO.RIOSend(client->req_queue, (PRIO_BUF)sendContext, 1, flags, sendContext))
	{
		printf_s("[DEBUG] RIOSend error: %d\n", GetLastError());
	}
	//client->storedmsgcnt++;
	client->req_queue_lock.unlock();
}

void send_packet_deffer(int id, void* buff)
{
	auto& client = clients[id];
	unsigned char* p = reinterpret_cast<unsigned char*>(buff);
	char* PiecePtr = nullptr;
	int PieceIdx = -1;

	int packetlen = p[0];
	if (packetlen > BUFPIECESIZE)
		p[-1] = 0;

	PieceIdx = client->RioBufferMng.GetFreeBufferPieceIdx(PiecePtr);
	memcpy(PiecePtr, buff, packetlen);

	RioIoContext* sendContext = new RioIoContext(client, EVENT_TYPE::EV_SEND);
	sendContext->BufferId = client->RioBufferId;
	sendContext->Length = packetlen;
	sendContext->Offset = SEND_BUFFER_OFFSET + PieceIdx * BUFPIECESIZE;
	sendContext->SendBufIdx = PieceIdx;

	DWORD sendbytes = 0;
	DWORD flags = 0;

	client->req_queue_lock.lock();
	if (!gRIO.RIOSend(client->req_queue, (PRIO_BUF)sendContext, 1, RIO_MSG_DEFER, sendContext))
	{
		printf_s("[DEBUG] RIOSend error: %d\n", GetLastError());
	}
	//client->storedmsgcnt++;
	client->last_msg_time = chrono::high_resolution_clock::now();
	client->req_queue_lock.unlock();
}

void PostRecv(int id)
{
	auto& client = clients[id];
	RioIoContext* recvContext = new RioIoContext(client, EVENT_TYPE::EV_RECV);

	recvContext->BufferId = client->RioBufferId;
	recvContext->Length = SESSION_BUFFER_SIZE / 2;
	recvContext->Offset = 0;

	DWORD recvbytes = 0;
	DWORD flags = 0;

	client->req_queue_lock.lock();
	/// start async recv
	if (!gRIO.RIOReceive(client->req_queue, (PRIO_BUF)recvContext, 1, flags, recvContext))
	{
		printf_s("[DEBUG] RIOReceive error: %d\n", GetLastError());
	}
	client->req_queue_lock.unlock();
}
#pragma optimize("", on)

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
	//lock_guard<mutex>lg{ clients[client]->view_list_lock };
	clients[client]->view_list.insert(new_id);

	//Get_Zone_Ref(client).insert(new_id);
}

void send_pos_packet(int client, int mover)
{
	sc_packet_pos packet;
	packet.id = mover;
	packet.size = sizeof(packet);
	packet.type = SC_POS;
	packet.x = clients[mover]->x;
	packet.y = clients[mover]->y;
	packet.move_time = clients[client]->seq_no;

	//clients[client]->view_list_lock.lock();
	//if ((client == mover) || (0 != Get_Zone_Ref(client).count(mover))) {
	//	if (0 != clients[client]->view_list.count(mover)) {
	//		send_packet(client, &packet);
	//	}
	//}
	if ((client == mover) || (0 != clients[client]->view_list.count(mover))) {
		//clients[client]->view_list_lock.unlock();
		send_packet(client, &packet);
		//send_packet_deffer(client, &packet);
	}
	else {
		//clients[client]->view_list_lock.unlock();
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

	//lock_guard<mutex>lg{ clients[client]->view_list_lock };
	clients[client]->view_list_lock.lock();
	clients[client]->view_list.unsafe_erase(leaver);
	clients[client]->view_list_lock.unlock();

	//int zone_idx = Get_Zone_idx(client);
	//Zone_lock[zone_idx]->lock();
	//Get_Zone_Ref(client).unsafe_erase(leaver);
	//Zone_lock[zone_idx]->unlock();

}

void Disconnect(int id)
{
	auto& client = clients[id];
	closesocket(client->socket);
	client->is_connected = false;
	client->view_list_lock.lock();
	auto vl = client->view_list;
	client->view_list.clear();
	client->view_list_lock.unlock();

	int idx = clients[id]->curr_zone_idx;
	Zone[idx].Erase(id);
	//Zone_lock[clients[id]->curr_zone_idx].lock();
	//Zone[clients[id]->curr_zone_idx].Zone_Client_List.erase(id);
	//Zone_lock[clients[id]->curr_zone_idx].unlock();

	for (auto& cl : clients) {
		if (cl == nullptr) continue;
		if (cl->id == id) continue;
		if (false == cl->is_connected) continue;
		//if (false == zone.count(id)) continue;

		if (false == cl->view_list.count(id)) continue;

		cl->view_list_lock.lock();
		cl->view_list.unsafe_erase(id);
		cl->view_list_lock.unlock();

		send_remove_object_packet(cl->id, id);
	}

	Enable_Clients.push(id);
}

int dolock(int id) {
	clients[id]->view_list_lock.lock();
	volatile int a = 0;
	return a;
}
int dounlock(int id) {
	clients[id]->view_list_lock.unlock();
	volatile int a = 0;
	return a;
}

void test2(int id, concurrency::concurrent_unordered_set<int>& t, unordered_set<int>& t2)
{
	send_pos_packet(id, id);
	for (auto cl : t) {
		if (0 != t2.count(cl)) {
			send_pos_packet(cl, id);
		}
		else
		{
			send_remove_object_packet(id, cl);
			send_remove_object_packet(cl, id);
		}
	}
	for (auto cl : t2) {
		if (0 == t.count(cl)) {
			send_put_object_packet(id, cl);
			send_put_object_packet(cl, id);
		}
	}
}

void test(int id, concurrency::concurrent_unordered_set<int>& t)
{
	unordered_set <int> new_vl;
	//auto& zone = Get_Zone_Ref(id);
	int zoneidx = Get_Zone_idx(id);
	
	for (auto idx : Zone[zoneidx].NearZoneList) {
		if (idx < 0 || idx >= ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE) continue;
		for (auto& other_id : Zone[idx].Zone_Client_List) {
			auto& other = clients[other_id];
			if (other == nullptr) continue;
			if (id == other_id) continue;
			if (false == clients[other_id]->is_connected) continue;

			if (true == is_near(id, other_id)) new_vl.insert(other_id);
		}
	}
	test2(id, t, new_vl);
}

void ProcessMove(int id, unsigned char dir)
{
	short x = clients[id]->x;
	short y = clients[id]->y;
	clients[id]->view_list_lock.lock();
	//dolock(id);
	auto old_vl = clients[id]->view_list;
	//dounlock(id);
	clients[id]->view_list_lock.unlock();
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

	int newzoneidx = Get_Zone_idx(id);
	if (newzoneidx != clients[id]->curr_zone_idx) {
		//Zone_lock[zoneidx].lock();
		Zone[newzoneidx].Insert(id);
		//Zone_lock[zoneidx].unlock();

		//Zone_lock[clients[id]->curr_zone_idx].lock();
		Zone[clients[id]->curr_zone_idx].Erase(id);
		//Zone_lock[clients[id]->curr_zone_idx].unlock();
	}

	test(id, old_vl);

	//set <int> new_vl;
	//for (auto& cl : clients) {
	//	if (cl == nullptr) continue;
	//	int other = cl->id;
	//	if (id == other) continue;
	//	if (false == clients[other]->is_connected) continue;
	//
	//	//if (false == Chk_Is_in_Zone(id, other)) continue;
	//	if (true == is_near(id, other)) new_vl.insert(other);
	//}
	//
	//send_pos_packet(id, id);
	//for (auto cl : old_vl) {
	//	if (0 != new_vl.count(cl)) {
	//		send_pos_packet(cl, id);
	//	}
	//	else
	//	{
	//		send_remove_object_packet(id, cl);
	//		send_remove_object_packet(cl, id);
	//	}
	//}
	//for (auto cl : new_vl) {
	//	if (0 == old_vl.count(cl)) {
	//		send_put_object_packet(id, cl);
	//		send_put_object_packet(cl, id);
	//	}
	//}
}



void ProcessLogin(int user_id, char* id_str)
{
	for (auto& cl : clients) {
		if (cl == nullptr) continue;
		if (false == cl->is_connected) continue;
		if (0 == strcmp(cl->name, id_str)) {
			send_login_fail(user_id);
			Disconnect(user_id);
			return;
		}
	}
	strcpy_s(clients[user_id]->name, id_str);
	send_login_ok_packet(user_id);
	clients[user_id]->is_connected = true;
	clients[user_id]->curr_zone_idx = Get_Zone_idx(user_id);

	//Zone_lock[clients[user_id]->curr_zone_idx].lock();
	Zone[clients[user_id]->curr_zone_idx].Insert(user_id);
	//Zone_lock[clients[user_id]->curr_zone_idx].unlock();

	for (auto idx : Zone[clients[user_id]->curr_zone_idx].NearZoneList) {
		if (idx < 0 || idx >= ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE) continue;
		for (auto id : Zone[idx].Zone_Client_List) {
			auto& cl = clients[id];
			if (cl == nullptr) continue;
			int other_player = cl->id;
			if (false == clients[other_player]->is_connected) continue;

			//if (false == Chk_Is_in_Zone(user_id, other_player)) continue;

			if (true == is_near(other_player, user_id)) {
				send_put_object_packet(other_player, user_id);
				if (other_player != user_id) {
					send_put_object_packet(user_id, other_player);
				}
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
		clients[id]->seq_no = move_packet->move_time;
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

#pragma optimize("", off)
void do_worker(int thread_idx)
{
	int tid = thread_idx;
	while (true) {
		//DWORD num_byte;
		//ULONGLONG key64;
		//PULONG_PTR p_key = &key64;
		//WSAOVERLAPPED* p_over;

		// 완료통지가 중요
		//GetQueuedCompletionStatus(g_iocp, &num_byte, p_key, &p_over, INFINITE);

		// 완료통지 후 cq에 쌓인 result 처리
		RIORESULT results[MAX_RIO_RESULTS];
		ULONG numResults = gRIO.RIODequeueCompletion(g_rio_cq[tid], results, MAX_RIO_RESULTS);
		//g_rio_cq_lock.lock();
		//ULONG numResults = gRIO.RIODequeueCompletion(g_rio_cq, results, MAX_RIO_RESULTS);
		//g_rio_cq_lock.unlock();

		for (ULONG i = 0; i < numResults; ++i) {
			RioIoContext* context = reinterpret_cast<RioIoContext*>(results[i].RequestContext);

			//COMPINFO* comp_info = reinterpret_cast<COMPINFO*>(results[i].RequestContext);
			EVENT_TYPE op = context->IoType;
			ULONG num_byte = results[i].BytesTransferred;
			int user_id = static_cast<int>(results[i].SocketContext);

			if (0 == num_byte) {
				if (false == clients[user_id]->is_connected)
					continue;

				Disconnect(user_id);
				//clients[user_id]->is_connected = false;
				//closesocket(clients[user_id]->socket);
				printf("Disconnected : %d\n", user_id);
				// delete는 하지 않았으나 재사용 처리를 하지 않았기 때문에 추후에 적용해야한다.
				//g_rio_mm->delete_rio_buffer(clients[user_id]->recv_info.rio_buffer);

				if (op == EV_SEND) {
					// Send buf해제
					//g_rio_mm->delete_rio_buffer(comp_info->rio_buffer);
					//delete comp_info;
				}
			}
			else if (EV_RECV == op) {
				// Recv data 시작주소
				auto& client = clients[user_id];

				char* p = client->RioBufferPointer;

				//char* p = g_rio_buf_addr + clients[user_id]->recv_info.rio_buffer->Offset;
				int remain = num_byte;
				int packet_size;
				int prev_packet_size = clients[user_id]->prev_packet_size;
				if (0 == prev_packet_size)
					packet_size = 0;
				else
					packet_size = clients[user_id]->pre_net_buf[0];

				while (remain > 0)
				{
					if (0 == packet_size)
						packet_size = p[0];

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
				PostRecv(user_id);
				//gRIO.RIOReceive(clients[user_id]->req_queue, clients[user_id]->recv_info.rio_buffer, 1, 0, &clients[user_id]->recv_info);

				/// 여기서 notify를 하면 recv의 순서가 엇갈린다
				//gRIO.RIONotify(g_rio_cq);
			}
			else if (EV_SEND == op) {
				clients[user_id]->RioBufferMng.ReleaseUsedBufferPiece(context->SendBufIdx);

				//g_rio_mm->delete_rio_buffer(comp_info->rio_buffer);
				//delete comp_info;
			}
			else {
				cout << "Unknown Event Type :" << op << endl;
				while (true);
			}

			//auto du = chrono::high_resolution_clock::now() - clients[user_id]->last_msg_time;
			//if (chrono::duration_cast<chrono::milliseconds>(du).count() > 1) {
			//	gRIO.RIOSend(clients[user_id]->req_queue, NULL, 0, RIO_MSG_COMMIT_ONLY, NULL);
			//}

			//if (clients[user_id]->storedmsgcnt > 5) {
			//	clients[user_id]->storedmsgcnt = 0;
			//	clients[user_id]->storedmsgcnt = 0;
			//}
		}
		// 전체가 끝났으니 여기서 다시 완료통지 (PGCS)
		//gRIO.RIONotify(g_rio_cq);
	}
}
#pragma optimize("", on)

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
	if (SOCKET_ERROR == ::bind(listenSocket, (struct sockaddr*)&serverAddr, sizeof(SOCKADDR_IN))) {
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

	// Init IOCP
	//g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	vector <thread> worker_threads;
	for (int i = 0; i < MAX_THREAD; ++i)
		worker_threads.emplace_back(do_worker, i);

	WSAOVERLAPPED iocp_over;
	ZeroMemory(&iocp_over, sizeof(iocp_over));

	//RIO_NOTIFICATION_COMPLETION rio_noti;
	//rio_noti.Type = RIO_IOCP_COMPLETION;
	//rio_noti.Iocp.IocpHandle = g_iocp;
	//rio_noti.Iocp.Overlapped = &iocp_over;
	//rio_noti.Iocp.CompletionKey = NULL;
	//g_rio_cq = gRIO.RIOCreateCompletionQueue(MAX_CQ_SIZE_PER_RIO_THREAD, &rio_noti);
	//g_rio_cq = gRIO.RIOCreateCompletionQueue(MAX_CQ_SIZE_PER_RIO_THREAD, 0);
	for (int i = 0; i < MAX_THREAD; ++i) {
		g_rio_cq[i] = gRIO.RIOCreateCompletionQueue(MAX_CQ_SIZE_PER_RIO_THREAD, 0);
	}
	for (int i = 0; i < MAX_USER; ++i) {
		Enable_Clients.push(i);
	}

	for (int i = 0; i < ZONE_ONELINE_SIZE * ZONE_ONELINE_SIZE; ++i) {
		Zone[i].ZoneIdx = i;
		Zone[i].SetNearZoneList();
	}

	//g_rio_mm = new Rio_Memory_Manager;

	while (true) {
		SOCKET clientSocket = accept(listenSocket, (struct sockaddr*)&clientAddr, &addrLen);
		if (INVALID_SOCKET == clientSocket) {
			int err_no = WSAGetLastError();
			if (WSA_IO_PENDING != err_no)
				error_display("Accept Error :", err_no);
		}

		int user_id = new_user_id++;

		if (false == Enable_Clients.try_pop(user_id)) {
			cout << "Currently Max User\n";
			continue;
		}

		SOCKETINFO* new_player = new SOCKETINFO;
		new_player->id = user_id;
		new_player->socket = clientSocket;
		new_player->prev_packet_size = 0;
		new_player->x = rand() % WORLD_WIDTH;
		new_player->y = rand() % WORLD_HEIGHT;

		new_player->RioBufferPointer = reinterpret_cast<char*>(VirtualAllocEx(GetCurrentProcess(), 0, SESSION_BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		new_player->RioSendBufferPtr = new_player->RioBufferPointer + SEND_BUFFER_OFFSET;
		new_player->RioBufferId = gRIO.RIORegisterBuffer(new_player->RioBufferPointer, SESSION_BUFFER_SIZE);
		new_player->RioBufferMng.startPtr = new_player->RioSendBufferPtr;

		//new_player->recv_info.rio_buffer = g_rio_mm->new_rio_buffer();
		//new_player->recv_info.op = EV_RECV;
		//clients.insert(make_pair(user_id, new_player));
		clients[user_id] = new_player;

		int thread_idx = new_player->id % MAX_THREAD;
		new_player->req_queue = gRIO.RIOCreateRequestQueue(
			clientSocket,
			MAX_RECV_RQ_SIZE_PER_SOCKET, 1,
			MAX_SEND_RQ_SIZE_PER_SOCKET, 1,
			g_rio_cq[thread_idx], g_rio_cq[thread_idx],
			//g_rio_cq, g_rio_cq,
			(PVOID)static_cast<ULONGLONG>(user_id));

		PostRecv(user_id);
		//gRIO.RIOReceive(new_player->req_queue, new_player->recv_info.rio_buffer, 1, NULL, &new_player->recv_info);
		//gRIO.RIONotify(g_rio_cq[thread_idx]);
	}

	for (auto& th : worker_threads)
		th.join();


	closesocket(listenSocket);
	WSACleanup();
}

