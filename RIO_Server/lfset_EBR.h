#pragma once
#include <mutex>
#include <thread>
#include <vector>
#include <memory>
#include <atomic>
#include <list>

using namespace std;

class LFNODE {
public:
	int key;
	unsigned long long next;
	unsigned long long retire_epoch = 0;

	LFNODE() {
		next = 0;
	}
	LFNODE(int x) {
		key = x;
		next = 0;
	}
	~LFNODE() {
	}
	LFNODE* GetNext() {
		return reinterpret_cast<LFNODE*>(next & -2LL);
	}

	void SetNext(LFNODE* ptr) {
		next = reinterpret_cast<unsigned long long>(ptr);
	}

	LFNODE* GetNextWithMark(bool* mark) {
		unsigned long long temp = next;
		*mark = (temp % 2) == 1;
		return reinterpret_cast<LFNODE*>(temp & -2LL);
	}

	bool CAS(unsigned long long old_value, unsigned long long new_value)
	{
		return atomic_compare_exchange_strong(
			reinterpret_cast<atomic_ullong*>(&next),
			&old_value, new_value);
	}

	bool CAS(LFNODE* old_next, LFNODE* new_next, bool old_mark, bool new_mark) {
		unsigned long long old_value = reinterpret_cast<unsigned long long>(old_next);
		if (old_mark) old_value = old_value | 0x1;
		else old_value = old_value & -2LL;
		unsigned long long new_value = reinterpret_cast<unsigned long long>(new_next);
		if (new_mark) new_value = new_value | 0x1;
		else new_value = new_value & -2LL;
		return CAS(old_value, new_value);
	}

	bool TryMark(LFNODE* ptr)
	{
		unsigned long long old_value = reinterpret_cast<unsigned long long>(ptr) & -2LL;
		unsigned long long new_value = old_value | 1;
		return CAS(old_value, new_value);
	}

	bool IsMarked() {
		return (0 != (next & 1));
	}
};

atomic_ullong set_g_epoch = 0;
atomic_ullong set_reservation[MAX_THREAD]{ ULLONG_MAX };
thread_local unsigned long long set_local_counter = 0;
thread_local std::vector<LFNODE*> set_retired_list;


void set_empty() {
	unsigned long long max_safe_epoch = set_reservation[0];
	for (int i = 0; i < MAX_THREAD; ++i) {
		if (set_reservation[i] <= max_safe_epoch)
			max_safe_epoch = set_reservation[i];
	}

	auto it = remove_if(set_retired_list.begin(), set_retired_list.end(), [max_safe_epoch](auto& remove) {
		if (remove->retire_epoch < max_safe_epoch) {
			delete remove;
			return true;
		}
		return false;
		});

	set_retired_list.erase(it, set_retired_list.end());
}

void set_epoch_clear() {
	set_local_counter = 0;
	for (auto it = set_retired_list.begin(); it != set_retired_list.end();) {
		auto& block = (*it);
		it = set_retired_list.erase(it);
		delete block;
	}
	set_retired_list.clear();
}

void set_retire(LFNODE* block) {
	set_retired_list.emplace_back(block);
	block->retire_epoch = set_g_epoch;
	set_local_counter++;

	if (set_local_counter % epoch_freq == 0) {
		//cout << "g\n";
		set_g_epoch.fetch_add(1);
	}
	if (set_retired_list.size() % empty_freq == 0) {
		//cout << retired_list.size() << "\n";
		set_empty();
	}
}

void set_start_op() {
	set_reservation[tl_idx].store(set_g_epoch);
}
void set_end_op() {
	set_reservation[tl_idx].store(ULLONG_MAX);
}

class LFSET
{
public:
	LFNODE head, tail;
	LFSET()
	{
		head.key = 0x80000000;
		tail.key = 0x7FFFFFFF;
		head.SetNext(&tail);
	}
	void Init()
	{
		set_g_epoch = 0;
		for (int i = 0; i < MAX_THREAD; ++i)
			set_reservation[i] = INT_MAX;

		while (head.GetNext() != &tail) {
			LFNODE* temp = head.GetNext();
			head.next = temp->next;
			delete temp;
		}
	}

	void Dump()
	{
		LFNODE* ptr = head.GetNext();
		cout << "Result Contains : ";
		for (int i = 0; i < 20; ++i) {
			cout << ptr->key << ", ";
			if (&tail == ptr) break;
			ptr = ptr->GetNext();
		}
		cout << endl;
	}

	void Find(int x, LFNODE** pred, LFNODE** curr)
	{
	retry:
		LFNODE* pr = &head;
		LFNODE* cu = pr->GetNext();
		while (true) {
			bool removed;
			LFNODE* su = cu->GetNextWithMark(&removed);
			while (true == removed) {
				if (false == pr->CAS(cu, su, false, false))
					goto retry;

				set_retire(cu);

				cu = su;
				su = cu->GetNextWithMark(&removed);
			}
			if (cu->key >= x) {
				*pred = pr; *curr = cu;
				return;
			}
			pr = cu;
			cu = cu->GetNext();
		}
	}
	bool Add(int x)
	{
		LFNODE* pred, * curr;
		while (true) {
			set_start_op();
			Find(x, &pred, &curr);

			if (curr->key == x) {
				set_end_op();
				return false;
			}
			else {
				LFNODE* e = new LFNODE(x);
				e->SetNext(curr);
				if (false == pred->CAS(curr, e, false, false))
					continue;
				set_end_op();
				return true;
			}
		}
	}

	bool Remove(int x)
	{
		LFNODE* pred, * curr;
		set_start_op();
		while (true) {
			Find(x, &pred, &curr);

			if (curr->key != x) {
				set_end_op();
				return false;
			}
			else {
				LFNODE* succ = curr->GetNext();
				if (false == curr->TryMark(succ)) continue;
				if (true == pred->CAS(curr, succ, false, false))
					set_retire(curr);
				set_end_op();
				// delete curr;
				return true;
			}
		}
	}
	bool Contains(int x)
	{
		LFNODE* curr = &head;
		while (curr->key < x) {
			curr = curr->GetNext();
		}

		return (false == curr->IsMarked()) && (x == curr->key);
	}
};