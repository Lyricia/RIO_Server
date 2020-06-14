#pragma once
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <list>

using namespace std;

class EBRNODE {
public:
	void* key;
	EBRNODE* next;
	ULONGLONG retire_epoch;

	EBRNODE() { next = nullptr; }
	EBRNODE(void* key_value) {
		next = nullptr;
		key = key_value;
	}
	~EBRNODE() {}
};


atomic_ullong q_g_epoch = 0;
atomic_ullong q_reservation[MAX_THREAD]{};
thread_local int q_local_counter = 0;
thread_local std::list<EBRNODE*> q_retired_list;

void empty() {
	ULONGLONG max_safe_epoch = q_reservation[0];
	for (int i = 0; i < MAX_THREAD; ++i) {
		if (q_reservation[i] <= max_safe_epoch)
			max_safe_epoch = q_reservation[i];
	}

	auto it = remove_if(q_retired_list.begin(), q_retired_list.end(), [max_safe_epoch](auto& remove) {
		if (remove->retire_epoch < max_safe_epoch) {
			delete remove;
			return true;
		}
		return false;
		});

	q_retired_list.erase(it, q_retired_list.end());
}

void epoch_clear() {
	q_local_counter = 0;
	for (auto it = q_retired_list.begin(); it != q_retired_list.end();) {
		auto& block = (*it);
		it = q_retired_list.erase(it);
		delete block;
	}
	q_retired_list.clear();
}

void retire(EBRNODE* block) {
	q_retired_list.push_back(block);
	block->retire_epoch = q_g_epoch;
	q_local_counter++;

	if (q_local_counter % epoch_freq == 0)
		q_g_epoch.fetch_add(1);
	if (q_retired_list.size() % empty_freq == 0)
		empty();
}

void q_start_op() {
	q_reservation[tl_idx].store(q_g_epoch);
}
void q_end_op() {
	q_reservation[tl_idx].store(ULLONG_MAX);
}

class LFQUEUE_epoch {
public:
	EBRNODE* volatile head;
	EBRNODE* volatile tail;
	LFQUEUE_epoch()
	{
		head = tail = new EBRNODE(0);
	}
	~LFQUEUE_epoch() {}

	void Init()
	{
		q_g_epoch = 0;
		for (int i = 0; i < MAX_THREAD; ++i)
			q_reservation[i] = INT_MAX;

		EBRNODE* ptr;
		while (head->next != nullptr) {
			ptr = head->next;
			head->next = head->next->next;
			delete ptr;
		}
		tail = head;
	}
	bool CAS(EBRNODE* volatile* addr, EBRNODE* old_EBRNODE, EBRNODE* new_EBRNODE)
	{
		return atomic_compare_exchange_strong(reinterpret_cast<volatile atomic_ullong*>(addr),
			reinterpret_cast<unsigned long long*>(&old_EBRNODE),
			reinterpret_cast<unsigned long long>(new_EBRNODE));
	}
	void Enq(void* key)
	{
		q_start_op();
		EBRNODE* e = new EBRNODE(key);
		while (true) {
			EBRNODE* last = tail;
			EBRNODE* next = last->next;
			if (last != tail) continue;
			if (next != nullptr) {
				CAS(&tail, last, next);
				continue;
			}
			if (false == CAS(&last->next, nullptr, e)) continue;
			CAS(&tail, last, e);
			q_end_op();
			return;
		}
	}
	void* Deq()
	{
		q_start_op();
		while (true) {
			EBRNODE* first = head;
			EBRNODE* next = first->next;
			EBRNODE* last = tail;
			EBRNODE* lastnext = last->next;

			if (first != head) continue;
			if (last == first) {
				if (lastnext == nullptr) {
					//cout << "EMPTY!!!\n";
					//this_thread::sleep_for(1ms);
					q_end_op();
					return nullptr;
				}
				else
				{
					CAS(&tail, last, lastnext);
					continue;
				}
			}
			if (nullptr == next) continue;
			void* result = next->key;
			if (false == CAS(&head, first, next)) continue;
			first->next = nullptr;
			retire(first);

			//delete first;
			q_end_op();
			return result;
		}
	}
	bool Empty() {
		return (head == tail);
	}
};

