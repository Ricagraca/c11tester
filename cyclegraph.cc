#include "cyclegraph.h"
#include "action.h"
#include "common.h"
#include "promise.h"
#include "model.h"

/** Initializes a CycleGraph object. */
CycleGraph::CycleGraph() :
	discovered(new HashTable<const CycleNode *, const CycleNode *, uintptr_t, 4, model_malloc, model_calloc, model_free>(16)),
	hasCycles(false),
	oldCycles(false)
{
}

/** CycleGraph destructor */
CycleGraph::~CycleGraph()
{
	delete discovered;
}

/**
 * Add a CycleNode to the graph, corresponding to a store ModelAction
 * @param act The write action that should be added
 * @param node The CycleNode that corresponds to the store
 */
void CycleGraph::putNode(const ModelAction *act, CycleNode *node)
{
	actionToNode.put(act, node);
#if SUPPORT_MOD_ORDER_DUMP
	nodeList.push_back(node);
#endif
}

/** @return The corresponding CycleNode, if exists; otherwise NULL */
CycleNode * CycleGraph::getNode_noCreate(const ModelAction *act) const
{
	return actionToNode.get(act);
}

/** @return The corresponding CycleNode, if exists; otherwise NULL */
CycleNode * CycleGraph::getNode_noCreate(const Promise *promise) const
{
	return readerToPromiseNode.get(promise->get_action());
}

/**
 * @brief Returns the CycleNode corresponding to a given ModelAction
 *
 * Gets (or creates, if none exist) a CycleNode corresponding to a ModelAction
 *
 * @param action The ModelAction to find a node for
 * @return The CycleNode paired with this action
 */
CycleNode * CycleGraph::getNode(const ModelAction *action)
{
	CycleNode *node = getNode_noCreate(action);
	if (node == NULL) {
		node = new CycleNode(action);
		putNode(action, node);
	}
	return node;
}

/**
 * @brief Returns a CycleNode corresponding to a promise
 *
 * Gets (or creates, if none exist) a CycleNode corresponding to a promised
 * value.
 *
 * @param promise The Promise generated by a reader
 * @return The CycleNode corresponding to the Promise
 */
CycleNode * CycleGraph::getNode(const Promise *promise)
{
	const ModelAction *reader = promise->get_action();
	CycleNode *node = getNode_noCreate(promise);
	if (node == NULL) {
		node = new CycleNode(promise);
		readerToPromiseNode.put(reader, node);
	}
	return node;
}

/**
 * @return false if the resolution results in a cycle; true otherwise
 */
bool CycleGraph::resolvePromise(ModelAction *reader, ModelAction *writer,
		promise_list_t *mustResolve)
{
	CycleNode *promise_node = readerToPromiseNode.get(reader);
	CycleNode *w_node = actionToNode.get(writer);
	ASSERT(promise_node);

	if (w_node)
		return mergeNodes(w_node, promise_node, mustResolve);
	/* No existing write-node; just convert the promise-node */
	promise_node->resolvePromise(writer);
	readerToPromiseNode.put(reader, NULL); /* erase promise_node */
	putNode(writer, promise_node);
	return true;
}

/**
 * @brief Merge two CycleNodes that represent the same write
 *
 * Note that this operation cannot be rolled back.
 *
 * @param w_node The write ModelAction node with which to merge
 * @param p_node The Promise node to merge. Will be destroyed after this
 * function.
 * @param mustMerge Return (pass-by-reference) any additional Promises that
 * must also be merged with w_node
 *
 * @return false if the merge results in a cycle; true otherwise
 */
bool CycleGraph::mergeNodes(CycleNode *w_node, CycleNode *p_node,
		promise_list_t *mustMerge)
{
	ASSERT(!w_node->is_promise());
	ASSERT(p_node->is_promise());
	const Promise *promise = p_node->getPromise();
	if (!promise->is_compatible(w_node->getAction())) {
		hasCycles = true;
		return false;
	}

	/* Transfer back edges to w_node */
	while (p_node->getNumBackEdges() > 0) {
		CycleNode *back = p_node->removeBackEdge();
		if (back != w_node) {
			if (back->is_promise()) {
				if (checkReachable(w_node, back)) {
					/* Edge would create cycle; merge instead */
					mustMerge->push_back(back->getPromise());
					if (!mergeNodes(w_node, back, mustMerge))
						return false;
				} else
					back->addEdge(w_node);
			} else
				addNodeEdge(back, w_node);
		}
	}

	/* Transfer forward edges to w_node */
	while (p_node->getNumEdges() > 0) {
		CycleNode *forward = p_node->removeEdge();
		if (forward != w_node) {
			if (forward->is_promise()) {
				if (checkReachable(forward, w_node)) {
					mustMerge->push_back(forward->getPromise());
					if (!mergeNodes(w_node, forward, mustMerge))
						return false;
				} else
					w_node->addEdge(forward);
			} else
				addNodeEdge(w_node, forward);
		}
	}

	/* erase p_node */
	readerToPromiseNode.put(promise->get_action(), NULL);
	delete p_node;

	return !hasCycles;
}

/**
 * Adds an edge between two CycleNodes.
 * @param fromnode The edge comes from this CycleNode
 * @param tonode The edge points to this CycleNode
 * @return True, if new edge(s) are added; otherwise false
 */
bool CycleGraph::addNodeEdge(CycleNode *fromnode, CycleNode *tonode)
{
	bool added;

	if (!hasCycles)
		hasCycles = checkReachable(tonode, fromnode);

	if ((added = fromnode->addEdge(tonode)))
		rollbackvector.push_back(fromnode);

	/*
	 * If the fromnode has a rmwnode that is not the tonode, we should add
	 * an edge between its rmwnode and the tonode
	 */
	CycleNode *rmwnode = fromnode->getRMW();
	if (rmwnode && rmwnode != tonode) {
		if (!hasCycles)
			hasCycles = checkReachable(tonode, rmwnode);

		if (rmwnode->addEdge(tonode)) {
			rollbackvector.push_back(rmwnode);
			added = true;
		}
	}
	return added;
}

/**
 * @brief Add an edge between a write and the RMW which reads from it
 *
 * Handles special case of a RMW action, where the ModelAction rmw reads from
 * the ModelAction from. The key differences are:
 * (1) no write can occur in between the rmw and the from action.
 * (2) Only one RMW action can read from a given write.
 *
 * @param from The edge comes from this ModelAction
 * @param rmw The edge points to this ModelAction; this action must read from
 * ModelAction from
 */
void CycleGraph::addRMWEdge(const ModelAction *from, const ModelAction *rmw)
{
	ASSERT(from);
	ASSERT(rmw);

	CycleNode *fromnode = getNode(from);
	CycleNode *rmwnode = getNode(rmw);

	/* Two RMW actions cannot read from the same write. */
	if (fromnode->setRMW(rmwnode))
		hasCycles = true;
	else
		rmwrollbackvector.push_back(fromnode);

	/* Transfer all outgoing edges from the from node to the rmw node */
	/* This process should not add a cycle because either:
	 * (1) The rmw should not have any incoming edges yet if it is the
	 * new node or
	 * (2) the fromnode is the new node and therefore it should not
	 * have any outgoing edges.
	 */
	for (unsigned int i = 0; i < fromnode->getNumEdges(); i++) {
		CycleNode *tonode = fromnode->getEdge(i);
		if (tonode != rmwnode) {
			if (rmwnode->addEdge(tonode))
				rollbackvector.push_back(rmwnode);
		}
	}

	addNodeEdge(fromnode, rmwnode);
}

#if SUPPORT_MOD_ORDER_DUMP
void CycleGraph::dumpNodes(FILE *file) const
{
	for (unsigned int i = 0; i < nodeList.size(); i++) {
		CycleNode *cn = nodeList[i];
		const ModelAction *action = cn->getAction();
		fprintf(file, "N%u [label=\"%u, T%u\"];\n", action->get_seq_number(), action->get_seq_number(), action->get_tid());
		if (cn->getRMW() != NULL) {
			fprintf(file, "N%u -> N%u[style=dotted];\n", action->get_seq_number(), cn->getRMW()->getAction()->get_seq_number());
		}
		for (unsigned int j = 0; j < cn->getNumEdges(); j++) {
			CycleNode *dst = cn->getEdge(j);
			const ModelAction *dstaction = dst->getAction();
			fprintf(file, "N%u -> N%u;\n", action->get_seq_number(), dstaction->get_seq_number());
		}
	}
}

void CycleGraph::dumpGraphToFile(const char *filename) const
{
	char buffer[200];
	sprintf(buffer, "%s.dot", filename);
	FILE *file = fopen(buffer, "w");
	fprintf(file, "digraph %s {\n", filename);
	dumpNodes(file);
	fprintf(file, "}\n");
	fclose(file);
}
#endif

/**
 * Checks whether one CycleNode can reach another.
 * @param from The CycleNode from which to begin exploration
 * @param to The CycleNode to reach
 * @return True, @a from can reach @a to; otherwise, false
 */
bool CycleGraph::checkReachable(const CycleNode *from, const CycleNode *to) const
{
	std::vector< const CycleNode *, ModelAlloc<const CycleNode *> > queue;
	discovered->reset();

	queue.push_back(from);
	discovered->put(from, from);
	while (!queue.empty()) {
		const CycleNode *node = queue.back();
		queue.pop_back();
		if (node == to)
			return true;

		for (unsigned int i = 0; i < node->getNumEdges(); i++) {
			CycleNode *next = node->getEdge(i);
			if (!discovered->contains(next)) {
				discovered->put(next, next);
				queue.push_back(next);
			}
		}
	}
	return false;
}

/** @return True, if the promise has failed; false otherwise */
bool CycleGraph::checkPromise(const ModelAction *fromact, Promise *promise) const
{
	std::vector< CycleNode *, ModelAlloc<CycleNode *> > queue;
	discovered->reset();
	CycleNode *from = actionToNode.get(fromact);

	queue.push_back(from);
	discovered->put(from, from);
	while (!queue.empty()) {
		CycleNode *node = queue.back();
		queue.pop_back();

		if (!node->is_promise() &&
				promise->eliminate_thread(node->getAction()->get_tid()))
			return true;

		for (unsigned int i = 0; i < node->getNumEdges(); i++) {
			CycleNode *next = node->getEdge(i);
			if (!discovered->contains(next)) {
				discovered->put(next, next);
				queue.push_back(next);
			}
		}
	}
	return false;
}

void CycleGraph::startChanges()
{
	ASSERT(rollbackvector.empty());
	ASSERT(rmwrollbackvector.empty());
	ASSERT(oldCycles == hasCycles);
}

/** Commit changes to the cyclegraph. */
void CycleGraph::commitChanges()
{
	rollbackvector.clear();
	rmwrollbackvector.clear();
	oldCycles = hasCycles;
}

/** Rollback changes to the previous commit. */
void CycleGraph::rollbackChanges()
{
	for (unsigned int i = 0; i < rollbackvector.size(); i++)
		rollbackvector[i]->popEdge();

	for (unsigned int i = 0; i < rmwrollbackvector.size(); i++)
		rmwrollbackvector[i]->clearRMW();

	hasCycles = oldCycles;
	rollbackvector.clear();
	rmwrollbackvector.clear();
}

/** @returns whether a CycleGraph contains cycles. */
bool CycleGraph::checkForCycles() const
{
	return hasCycles;
}

/**
 * @brief Constructor for a CycleNode
 * @param act The ModelAction for this node
 */
CycleNode::CycleNode(const ModelAction *act) :
	action(act),
	promise(NULL),
	hasRMW(NULL)
{
}

/**
 * @brief Constructor for a Promise CycleNode
 * @param promise The Promise which was generated
 */
CycleNode::CycleNode(const Promise *promise) :
	action(NULL),
	promise(promise),
	hasRMW(NULL)
{
}

/**
 * @param i The index of the edge to return
 * @returns The a CycleNode edge indexed by i
 */
CycleNode * CycleNode::getEdge(unsigned int i) const
{
	return edges[i];
}

/** @returns The number of edges leaving this CycleNode */
unsigned int CycleNode::getNumEdges() const
{
	return edges.size();
}

CycleNode * CycleNode::getBackEdge(unsigned int i) const
{
	return back_edges[i];
}

unsigned int CycleNode::getNumBackEdges() const
{
	return back_edges.size();
}

/**
 * @brief Remove an element from a vector
 * @param v The vector
 * @param n The element to remove
 * @return True if the element was found; false otherwise
 */
template <typename T>
static bool vector_remove_node(std::vector<T, SnapshotAlloc<T> >& v, const T n)
{
	for (unsigned int i = 0; i < v.size(); i++) {
		if (v[i] == n) {
			v.erase(v.begin() + i);
			return true;
		}
	}
	return false;
}

/**
 * @brief Remove a (forward) edge from this CycleNode
 * @return The CycleNode which was popped, if one exists; otherwise NULL
 */
CycleNode * CycleNode::removeEdge()
{
	if (edges.empty())
		return NULL;

	CycleNode *ret = edges.back();
	edges.pop_back();
	vector_remove_node(ret->back_edges, this);
	return ret;
}

/**
 * @brief Remove a (back) edge from this CycleNode
 * @return The CycleNode which was popped, if one exists; otherwise NULL
 */
CycleNode * CycleNode::removeBackEdge()
{
	if (back_edges.empty())
		return NULL;

	CycleNode *ret = back_edges.back();
	back_edges.pop_back();
	vector_remove_node(ret->edges, this);
	return ret;
}

/**
 * Adds an edge from this CycleNode to another CycleNode.
 * @param node The node to which we add a directed edge
 * @return True if this edge is a new edge; false otherwise
 */
bool CycleNode::addEdge(CycleNode *node)
{
	for (unsigned int i = 0; i < edges.size(); i++)
		if (edges[i] == node)
			return false;
	edges.push_back(node);
	node->back_edges.push_back(this);
	return true;
}

/** @returns the RMW CycleNode that reads from the current CycleNode */
CycleNode * CycleNode::getRMW() const
{
	return hasRMW;
}

/**
 * Set a RMW action node that reads from the current CycleNode.
 * @param node The RMW that reads from the current node
 * @return True, if this node already was read by another RMW; false otherwise
 * @see CycleGraph::addRMWEdge
 */
bool CycleNode::setRMW(CycleNode *node)
{
	if (hasRMW != NULL)
		return true;
	hasRMW = node;
	return false;
}

/**
 * Convert a Promise CycleNode into a concrete-valued CycleNode. Should only be
 * used when there's no existing ModelAction CycleNode for this write.
 *
 * @param writer The ModelAction which wrote the future value represented by
 * this CycleNode
 */
void CycleNode::resolvePromise(const ModelAction *writer)
{
	ASSERT(is_promise());
	ASSERT(promise->is_compatible(writer));
	action = writer;
	promise = NULL;
	ASSERT(!is_promise());
}
