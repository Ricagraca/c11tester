#include <stdio.h>
#include <algorithm>
#include <new>
#include <stdarg.h>
#include <string.h>
#include <cstdlib>

#include "model.h"
#include "action.h"
#include "schedule.h"
#include "snapshot-interface.h"
#include "common.h"
#include "datarace.h"
#include "threads-model.h"
#include "output.h"
#include "traceanalysis.h"
#include "execution.h"
#include "history.h"
#include "bugmessage.h"
#include "params.h"

ModelChecker *model = NULL;

/** Wrapper to run the user's main function, with appropriate arguments */
void user_main_wrapper(void *)
{
	user_main(model->params.argc, model->params.argv);
}

/** @brief Constructor */
ModelChecker::ModelChecker() :
	/* Initialize default scheduler */
	params(),
	restart_flag(false),
	scheduler(new Scheduler()),
	execution(new ModelExecution(this, scheduler)),
	history(new ModelHistory()),
	execution_number(1),
	trace_analyses(),
	inspect_plugin(NULL)
{
	memset(&stats,0,sizeof(struct execution_stats));
	init_thread = new Thread(execution->get_next_id(), (thrd_t *) model_malloc(sizeof(thrd_t)), &user_main_wrapper, NULL, NULL);	// L: user_main_wrapper passes the user program
#ifdef TLS
	init_thread->setTLS((char *)get_tls_addr());
#endif
	execution->add_thread(init_thread);
	scheduler->set_current_thread(init_thread);
	execution->setParams(&params);
	param_defaults(&params);
	initRaceDetector();
}

/** @brief Destructor */
ModelChecker::~ModelChecker()
{
	delete scheduler;
}

/** Method to set parameters */
model_params * ModelChecker::getParams() {
	return &params;
}

/**
 * Restores user program to initial state and resets all model-checker data
 * structures.
 */
void ModelChecker::reset_to_initial_state()
{

	/**
	 * FIXME: if we utilize partial rollback, we will need to free only
	 * those pending actions which were NOT pending before the rollback
	 * point
	 */
	for (unsigned int i = 0;i < get_num_threads();i++)
		delete get_thread(int_to_id(i))->get_pending();

	snapshot_backtrack_before(0);
}

/** @return the number of user threads created during this execution */
unsigned int ModelChecker::get_num_threads() const
{
	return execution->get_num_threads();
}

/**
 * Must be called from user-thread context (e.g., through the global
 * thread_current() interface)
 *
 * @return The currently executing Thread.
 */
Thread * ModelChecker::get_current_thread() const
{
	return scheduler->get_current_thread();
}

/**
 * @brief Choose the next thread to execute.
 *
 * This function chooses the next thread that should execute. It can enforce
 * execution replay/backtracking or, if the model-checker has no preference
 * regarding the next thread (i.e., when exploring a new execution ordering),
 * we defer to the scheduler.
 *
 * @return The next chosen thread to run, if any exist. Or else if the current
 * execution should terminate, return NULL.
 */
Thread * ModelChecker::get_next_thread()
{

	/*
	 * Have we completed exploring the preselected path? Then let the
	 * scheduler decide
	 */
	return scheduler->select_next_thread();
}

/**
 * @brief Assert a bug in the executing program.
 *
 * Use this function to assert any sort of bug in the user program. If the
 * current trace is feasible (actually, a prefix of some feasible execution),
 * then this execution will be aborted, printing the appropriate message. If
 * the current trace is not yet feasible, the error message will be stashed and
 * printed if the execution ever becomes feasible.
 *
 * @param msg Descriptive message for the bug (do not include newline char)
 * @return True if bug is immediately-feasible
 */
bool ModelChecker::assert_bug(const char *msg, ...)
{
	char str[800];

	va_list ap;
	va_start(ap, msg);
	vsnprintf(str, sizeof(str), msg, ap);
	va_end(ap);

	return execution->assert_bug(str);
}

/**
 * @brief Assert a bug in the executing program, asserted by a user thread
 * @see ModelChecker::assert_bug
 * @param msg Descriptive message for the bug (do not include newline char)
 */
void ModelChecker::assert_user_bug(const char *msg)
{
	/* If feasible bug, bail out now */
	if (assert_bug(msg))
		switch_to_master(NULL);
}

/** @brief Print bug report listing for this execution (if any bugs exist) */
void ModelChecker::print_bugs() const
{
	SnapVector<bug_message *> *bugs = execution->get_bugs();

	model_print("Bug report: %zu bug%s detected\n",
							bugs->size(),
							bugs->size() > 1 ? "s" : "");
	for (unsigned int i = 0;i < bugs->size();i++)
		(*bugs)[i] -> print();
}

/**
 * @brief Record end-of-execution stats
 *
 * Must be run when exiting an execution. Records various stats.
 * @see struct execution_stats
 */
void ModelChecker::record_stats()
{
	stats.num_total ++;
	if (!execution->isfeasibleprefix())
		stats.num_infeasible ++;
	else if (execution->have_bug_reports())
		stats.num_buggy_executions ++;
	else if (execution->is_complete_execution())
		stats.num_complete ++;
	else {
		stats.num_redundant ++;

		/**
		 * @todo We can violate this ASSERT() when fairness/sleep sets
		 * conflict to cause an execution to terminate, e.g. with:
		 * Scheduler: [0: disabled][1: disabled][2: sleep][3: current, enabled]
		 */
		//ASSERT(scheduler->all_threads_sleeping());
	}
}

/** @brief Print execution stats */
void ModelChecker::print_stats() const
{
	model_print("Number of complete, bug-free executions: %d\n", stats.num_complete);
	model_print("Number of redundant executions: %d\n", stats.num_redundant);
	model_print("Number of buggy executions: %d\n", stats.num_buggy_executions);
	model_print("Number of infeasible executions: %d\n", stats.num_infeasible);
	model_print("Total executions: %d\n", stats.num_total);
}

/**
 * @brief End-of-exeuction print
 * @param printbugs Should any existing bugs be printed?
 */
void ModelChecker::print_execution(bool printbugs) const
{
	model_print("Program output from execution %d:\n",
							get_execution_number());
	print_program_output();

	if (params.verbose >= 3) {
		print_stats();
	}

	/* Don't print invalid bugs */
	if (printbugs && execution->have_bug_reports()) {
		model_print("\n");
		print_bugs();
	}

	model_print("\n");
	execution->print_summary();
}

/**
 * Queries the model-checker for more executions to explore and, if one
 * exists, resets the model-checker state to execute a new execution.
 *
 * @return If there are more executions to explore, return true. Otherwise,
 * return false.
 */
bool ModelChecker::next_execution()
{
	DBG();
	/* Is this execution a feasible execution that's worth bug-checking? */
	bool complete = execution->isfeasibleprefix() &&
									(execution->is_complete_execution() ||
									 execution->have_bug_reports());

	/* End-of-execution bug checks */
	if (complete) {
		if (execution->is_deadlocked())
			assert_bug("Deadlock detected");

		run_trace_analyses();
	}

	record_stats();
	/* Output */
	if ( (complete && params.verbose) || params.verbose>1 || (complete && execution->have_bug_reports()))
		print_execution(complete);
	else
		clear_program_output();

	if (restart_flag) {
		do_restart();
		return true;
	}
// test code
	execution_number ++;
	reset_to_initial_state();
	return false;
}

/** @brief Run trace analyses on complete trace */
void ModelChecker::run_trace_analyses() {
	for (unsigned int i = 0;i < trace_analyses.size();i ++)
		trace_analyses[i] -> analyze(execution->get_action_trace());
}

/**
 * @brief Get a Thread reference by its ID
 * @param tid The Thread's ID
 * @return A Thread reference
 */
Thread * ModelChecker::get_thread(thread_id_t tid) const
{
	return execution->get_thread(tid);
}

/**
 * @brief Get a reference to the Thread in which a ModelAction was executed
 * @param act The ModelAction
 * @return A Thread reference
 */
Thread * ModelChecker::get_thread(const ModelAction *act) const
{
	return execution->get_thread(act);
}

/**
 * Switch from a model-checker context to a user-thread context. This is the
 * complement of ModelChecker::switch_to_master and must be called from the
 * model-checker context
 *
 * @param thread The user-thread to switch to
 */
void ModelChecker::switch_from_master(Thread *thread)
{
	scheduler->set_current_thread(thread);
	Thread::swap(&system_context, thread);
}

/**
 * Switch from a user-context to the "master thread" context (a.k.a. system
 * context). This switch is made with the intention of exploring a particular
 * model-checking action (described by a ModelAction object). Must be called
 * from a user-thread context.
 *
 * @param act The current action that will be explored. May be NULL only if
 * trace is exiting via an assertion (see ModelExecution::set_assert and
 * ModelExecution::has_asserted).
 * @return Return the value returned by the current action
 */
uint64_t ModelChecker::switch_to_master(ModelAction *act)
{
	if (forklock) {
		static bool fork_message_printed = false;

		if (!fork_message_printed) {
			model_print("Fork handler trying to call into model checker...\n");
			fork_message_printed = true;
		}
		delete act;
		return 0;
	}
	DBG();
	Thread *old = thread_current();
	scheduler->set_current_thread(NULL);
	ASSERT(!old->get_pending());

	if (inspect_plugin != NULL) {
		inspect_plugin->inspectModelAction(act);
	}

	old->set_pending(act);
	if (Thread::swap(old, &system_context) < 0) {
		perror("swap threads");
		exit(EXIT_FAILURE);
	}
	return old->get_return_value();
}

static void runChecker() {
	model->run();
	delete model;
}

void ModelChecker::startChecker() {
	startExecution(get_system_context(), runChecker);
}

bool ModelChecker::should_terminate_execution()
{
	/* Infeasible -> don't take any more steps */
	if (execution->is_infeasible())
		return true;
	else if (execution->isfeasibleprefix() && execution->have_fatal_bug_reports()) {
		execution->set_assert();
		return true;
	}
	return false;
}

/** @brief Restart ModelChecker upon returning to the run loop of the
 *	model checker. */
void ModelChecker::restart()
{
	restart_flag = true;
}

void ModelChecker::do_restart()
{
	restart_flag = false;
	reset_to_initial_state();
	memset(&stats,0,sizeof(struct execution_stats));
	execution_number = 1;
}

void ModelChecker::startMainThread() {
	init_thread->set_state(THREAD_RUNNING);
	scheduler->set_current_thread(init_thread);
	main_thread_startup();
}

static bool is_nonsc_write(const ModelAction *act) {
	if (act->get_type() == ATOMIC_WRITE) {
		std::memory_order order = act->get_mo();
		switch(order) {
		case std::memory_order_relaxed:
		case std::memory_order_release:
			return true;
		default:
			return false;
		}
	}
	return false;
}

/** @brief Run ModelChecker for the user program */
void ModelChecker::run()
{
	//Need to initial random number generator state to avoid resets on rollback
	char random_state[256];
	initstate(423121, random_state, sizeof(random_state));

	for(int exec = 0;exec < params.maxexecutions;exec++) {
		Thread * t = init_thread;

		do {
			/*
			 * Stash next pending action(s) for thread(s). There
			 * should only need to stash one thread's action--the
			 * thread which just took a step--plus the first step
			 * for any newly-created thread
			 */
			ModelAction * pending;
			for (unsigned int i = 0;i < get_num_threads();i++) {
				thread_id_t tid = int_to_id(i);
				Thread *thr = get_thread(tid);
				if (!thr->is_model_thread() && !thr->is_complete() && ((!(pending=thr->get_pending())) || is_nonsc_write(pending)) ) {
					switch_from_master(thr);	// L: context swapped, and action type of thr changed.
					if (thr->is_waiting_on(thr))
						assert_bug("Deadlock detected (thread %u)", i);
				}
			}

			/* Don't schedule threads which should be disabled */
			for (unsigned int i = 0;i < get_num_threads();i++) {
				Thread *th = get_thread(int_to_id(i));
				ModelAction *act = th->get_pending();
				if (act && execution->is_enabled(th) && !execution->check_action_enabled(act)) {
					scheduler->sleep(th);
				}
			}

			for (unsigned int i = 1;i < get_num_threads();i++) {
				Thread *th = get_thread(int_to_id(i));
				ModelAction *act = th->get_pending();
				if (act && execution->is_enabled(th) && (th->get_state() != THREAD_BLOCKED) ) {
					if (act->is_write()) {
						std::memory_order order = act->get_mo();
						if (order == std::memory_order_relaxed || \
								order == std::memory_order_release) {
							t = th;
							break;
						}
					} else if (act->get_type() == THREAD_CREATE || \
										 act->get_type() == PTHREAD_CREATE || \
										 act->get_type() == THREAD_START || \
										 act->get_type() == THREAD_FINISH) {
						t = th;
						break;
					}
				}
			}

			/* Catch assertions from prior take_step or from
			* between-ModelAction bugs (e.g., data races) */

			if (execution->has_asserted())
				break;
			if (!t)
				t = get_next_thread();
			if (!t || t->is_model_thread())
				break;

			/* Consume the next action for a Thread */
			ModelAction *curr = t->get_pending();
			t->set_pending(NULL);
			t = execution->take_step(curr);
		} while (!should_terminate_execution());
		next_execution();
		//restore random number generator state after rollback
		setstate(random_state);
	}

	model_print("******* Model-checking complete: *******\n");
	print_stats();

	/* Have the trace analyses dump their output. */
	for (unsigned int i = 0;i < trace_analyses.size();i++)
		trace_analyses[i]->finish();
}
