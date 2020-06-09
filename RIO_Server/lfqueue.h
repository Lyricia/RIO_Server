#pragma once
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>

using namespace std;
using namespace std::chrono;

class NODE {
public:
	void* key;
	NODE* next;

	NODE() { next = nullptr; }
	NODE(void* key_value) {
		next = nullptr;
		key = key_value;
	}
	~NODE() {}
};

class SPTR {
public:
	NODE* volatile ptr;
	volatile int stamp;
	SPTR() {
		ptr = nullptr;
		stamp = 0;
	}
	SPTR(NODE* p, int v) {
		ptr = p;
		stamp = v;
	}
};

class LFQUEUE {
	SPTR head;
	SPTR tail;
public:
	LFQUEUE()
	{
		head.ptr = tail.ptr = new NODE(0);
	}
	~LFQUEUE() {}

	void Init()
	{
		NODE* ptr;
		while (head.ptr->next != nullptr) {
			ptr = head.ptr->next;
			head.ptr->next = head.ptr->next->next;
			delete ptr;
		}
		tail = head;
	}
	bool CAS(NODE* volatile* addr, NODE* old_node, NODE* new_node)
	{
		return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_ullong*>(addr),
			reinterpret_cast<unsigned long long*>(&old_node),
			reinterpret_cast<unsigned long long>(new_node));
	}
	bool STAMP_CAS(SPTR* addr, NODE* old_node, int old_stamp, NODE* new_node)
	{
		SPTR old_ptr{ old_node, old_stamp };
		SPTR new_ptr{ new_node, old_stamp + 1 };
		return atomic_compare_exchange_strong(reinterpret_cast<atomic_ullong*>(addr),
			reinterpret_cast<unsigned long long*>(&old_ptr),
			*(reinterpret_cast<unsigned long long*>(&new_ptr)));
	}
	void Enq(void* key)
	{
		NODE* e = new NODE(key);
		while (true) {
			SPTR last = tail;
			NODE* next = last.ptr->next;
			if (last.stamp != tail.stamp) continue;
			if (next != nullptr) {
				STAMP_CAS(&tail, last.ptr, last.stamp, next);
				continue;
			}
			if (false == CAS(&last.ptr->next, nullptr, e)) continue;
			STAMP_CAS(&tail, last.ptr, last.stamp, e);
			return;
		}
	}
	void* Deq()
	{
		while (true) {
			SPTR first = head;
			NODE* next = first.ptr->next;
			SPTR last = tail;
			NODE* lastnext = last.ptr->next;
			if (first.ptr != head.ptr) continue;
			if (last.ptr == first.ptr) {
				if (lastnext == nullptr) {
					// cout << "EMPTY!!!\n";
					this_thread::sleep_for(1ms);
					return nullptr;
				}
				else
				{
					STAMP_CAS(&tail, last.ptr, last.stamp, lastnext);
					continue;
				}
			}
			if (nullptr == next) continue;
			void* result = next->key;
			if (false == STAMP_CAS(&head, first.ptr, first.stamp, next))
				continue;
			delete first.ptr;
			return result;
		}
	}

	bool Empty() {
		return (head.ptr == tail.ptr);
	}
};
