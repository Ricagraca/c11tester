#ifndef __ACTION_H__
#define __ACTION_H__

#include <list>

#include "libthreads.h"
#include "libatomic.h"

#define VALUE_NONE -1

typedef enum action_type {
	THREAD_CREATE,
	THREAD_YIELD,
	THREAD_JOIN,
	ATOMIC_READ,
	ATOMIC_WRITE
} action_type_t;

/* Forward declaration (tree.h) */
class TreeNode;

class ModelAction {
public:
	ModelAction(action_type_t type, memory_order order, void *loc, int value);
	void print(void);

	thread_id_t get_tid() { return tid; }
	action_type get_type() { return type; }
	memory_order get_mo() { return order; }
	void * get_location() { return location; }

	TreeNode * get_node() { return node; }
	void set_node(TreeNode *n) { node = n; }

	bool is_read();
	bool is_write();
	bool is_acquire();
	bool is_release();
	bool same_var(ModelAction *act);
	bool same_thread(ModelAction *act);
	bool is_dependent(ModelAction *act);
private:
	action_type type;
	memory_order order;
	void *location;
	thread_id_t tid;
	int value;
	TreeNode *node;
};

typedef std::list<class ModelAction *> action_list_t;

#endif /* __ACTION_H__ */