#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <bitset>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>

#include <pthread.h>

#include "allscale/utils/assert.h"

#include "allscale/api/core/impl/reference/lock.h"
#include "allscale/api/core/impl/reference/profiling.h"
#include "allscale/api/core/impl/reference/queue.h"
#include "allscale/api/core/impl/reference/runtime_predictor.h"

namespace allscale {
namespace api {
namespace core {
namespace impl {
namespace reference {

	// ------------------------------------- Declarations -----------------------------------------

	/**
	 * The actual treeture, referencing the computation of a value.
	 */
	template<typename T>
	class treeture;

	/**
	 * A treeture not yet released to the runtime system for execution.
	 */
	template<typename T>
	class unreleased_treeture;

	/**
	 * A handle for a lazily constructed unreleased treeture. This intermediate construct is utilized
	 * for writing templated code that can be optimized to overhead-less computed values and to facilitate
	 * the support of the sequence combinator.
	 */
	template<typename T, typename Gen>
	class lazy_unreleased_treeture;

	/**
	 * A reference to a task to synchronize upon it.
	 */
	class task_reference;

	/**
	 * A class to model task dependencies
	 */
	class dependencies;



	// ---------------------------------------------------------------------------------------------
	//								    Internal Forward Declarations
	// ---------------------------------------------------------------------------------------------


	class TaskBase;

	template<typename T>
	class Task;

	using TaskBasePtr = std::shared_ptr<TaskBase>;

	template<typename T>
	using TaskPtr = std::shared_ptr<Task<T>>;

	using TaskDependencies = dependencies;


	// ---------------------------------------------------------------------------------------------
	//											  Debugging
	// ---------------------------------------------------------------------------------------------


	// -- Declarations --

	const bool DEBUG = false;

	std::mutex g_log_mutex;

	#define LOG(MSG) \
		{  \
			if (DEBUG) { \
				std::thread::id this_id = std::this_thread::get_id(); \
				std::lock_guard<std::mutex> lock(g_log_mutex); \
				std::cerr << "Thread " << this_id << ": " << MSG << "\n"; \
			} \
		}

	const bool DEBUG_SCHEDULE = false;

	#define LOG_SCHEDULE(MSG) \
		{  \
			if (DEBUG_SCHEDULE) { \
				std::thread::id this_id = std::this_thread::get_id(); \
				std::lock_guard<std::mutex> lock(g_log_mutex); \
				std::cerr << "Thread " << this_id << ": " << MSG << "\n"; \
			} \
		}

	const bool DEBUG_TASKS = false;

	#define LOG_TASKS(MSG) \
		{  \
			if (DEBUG_TASKS) { \
				std::thread::id this_id = std::this_thread::get_id(); \
				std::lock_guard<std::mutex> lock(g_log_mutex); \
				std::cerr << "Thread " << this_id << ": " << MSG << "\n"; \
			} \
		}



	// -----------------------------------------------------------------
	//						  Monitoring (for Debugging)
	// -----------------------------------------------------------------


	const bool MONITORING_ENABLED = false;

	namespace monitoring {

		enum class EventType {
			Run, RunDirect, Split, Wait, DependencyWait
		};

		struct Event {

			EventType type;

			const TaskBase* task;

			TaskID taskId;

			bool operator==(const Event& other) const {
				return other.type == type && other.task == task && other.taskId == taskId;
			}

			friend std::ostream& operator<<(std::ostream& out, const Event& e);
		};


		class ThreadState {

			using guard = std::lock_guard<std::mutex>;

			std::thread::id thread_id;

			std::mutex lock;

			std::vector<Event> eventStack;

		public:

			ThreadState() : thread_id(std::this_thread::get_id()) {
				guard g(getStateLock());
				getStates().push_back(this);
			}

			~ThreadState() {
				assert_true(eventStack.empty());
			}

			void pushEvent(const Event& e) {
				guard g(lock);
				eventStack.push_back(e);
			}

			void popEvent(__unused const Event& e) {
				guard g(lock);
				assert_eq(e,eventStack.back());
				eventStack.pop_back();
			}

			void dumpState(std::ostream& out) {
				guard g(lock);
				out << "\nThread: " << thread_id << "\n";
				out << "\tStack:\n";
				for(const auto& cur : eventStack) {
					out << "\t\t" << cur << "\n";
				}
				out << "\t\t -- top of stack --\n";
				out << "\n";
			}

			static void dumpStates(std::ostream& out) {
				// lock states
				std::lock_guard<std::mutex> g(getStateLock());

				// provide a hint if there is no information
				if (getStates().empty()) {
					out << "No thread states recorded.";
					if (!MONITORING_ENABLED) {
						out << " You can enable it by setting the MONITORING_ENABLED flag in the code base.";
					}
					out << "\n";
					return;
				}

				// print all current states
				for(const auto& cur : getStates()) {
					cur->dumpState(out);
				}
			}

		private:

			static std::mutex& getStateLock() {
				static std::mutex state_lock;
				return state_lock;
			}

			static std::vector<ThreadState*>& getStates() {
				static std::vector<ThreadState*> states;
				return states;
			}

		};

		thread_local static ThreadState tl_thread_state;


		struct Action {

			bool active;
			Event e;

			Action() : active(false) {}

			Action(const Event& e) : active(true), e(e) {
				// register action
				tl_thread_state.pushEvent(e);
			}

			Action(Action&& other) : active(other.active), e(other.e) {
				other.active = false;
			}

			Action(const Action&) = delete;

			Action& operator=(const Action&) = delete;
			Action& operator=(Action&&) = delete;

			~Action() {
				if (!active) return;
				// remove action from action stack
				tl_thread_state.popEvent(e);
			}

		};

		Action log(EventType type, const TaskBase* task) {
			assert_true(type != EventType::DependencyWait);
			if (!MONITORING_ENABLED) return {};
			return Event{type,task,TaskID()};
		}

		Action log(EventType type, const TaskID& task) {
			assert_true(type == EventType::DependencyWait);
			if (!MONITORING_ENABLED) return {};
			return Event{type,nullptr,task};
		}

	}




	// ---------------------------------------------------------------------------------------------
	//								 	Task Dependency Manager
	// ---------------------------------------------------------------------------------------------

	template<std::size_t max_depth>
	class TaskDependencyManager {

		struct Entry {
			TaskBasePtr task;
			Entry* next;
		};

		using cell_type = std::atomic<Entry*>;

		enum { num_entries = 1<<(max_depth+1) };

		cell_type data[num_entries];

	public:

		TaskDependencyManager() {
			for(auto& cur : data) cur = nullptr;
		}

		~TaskDependencyManager() {
			for(auto& cur : data) {
				if (!isDone(cur)) delete cur;
			}
		}

		TaskDependencyManager(const TaskDependencyManager&) = delete;
		TaskDependencyManager(TaskDependencyManager&&) = delete;

		TaskDependencyManager& operator=(const TaskDependencyManager&) = delete;
		TaskDependencyManager& operator=(TaskDependencyManager&&) = delete;


		/**
		 * Adds a dependency between the given tasks such that
		 * task x depends on the completion of the task y.
		 */
		void addDependency(const TaskBasePtr& x, const TaskPath& y);

		void markComplete(const TaskPath& task);

		bool isComplete(const TaskPath& path) const {
			return isDone(data[getPosition(path)]);
		}

	private:

		std::size_t getPosition(const TaskPath& path) const {
			std::size_t res = 1;
			for(const auto& cur : path) {
				res = res * 2 + cur;
				if (res >= num_entries) return res / 2;
			}
			return res;
		}

		bool isDone(const Entry* ptr) const {
			// if the last bit is set, the task already finished
			return (intptr_t)(ptr) & 0x1;
		}

	};



	// ---------------------------------------------------------------------------------------------
	//											 Task Family
	// ---------------------------------------------------------------------------------------------


	/**
	 * A task family is a collection of tasks descending from a common (single) ancestor.
	 * Task families are created by root-level prec operator calls, and manage the dependencies
	 * of all its members.
	 *
	 * Tasks being created through recursive or combine calls are initially not members of
	 * any family, but may get adapted (by being the result of a split operation).
	 */
	class TaskFamily {

		// TODO: make task dependency manager depth target system dependent

		using DependencyManager = TaskDependencyManager<6>;

		// the unique ID of this family (for debugging)
		std::size_t id;

		// the manager of all dependencies on members of this family
		DependencyManager dependencies;

	public:

		/**
		 * Creates a new family, using a new ID.
		 */
		TaskFamily() : id(getNextID()) {}

		/**
		 * Obtain the family ID.
		 */
		std::size_t getId() const {
			return id;
		}

		/**
		 * Tests whether the given sub-task is complete.
		 */
		bool isComplete(const TaskPath& path) const {
			return dependencies.isComplete(path);
		}

		/**
		 * Register a dependency ensuring that a task x is depending on a task y.
		 */
		void addDependency(const TaskBasePtr& x, const TaskPath& y) {
			dependencies.addDependency(x,y);
		}

		/**
		 * Mark the given task as being finished.
		 */
		void markDone(const TaskPath& x) {
			dependencies.markComplete(x);
		}

		/**
		 * A family ID generator.
		 */
		static unsigned getNextID() {
			static std::atomic<int> counter(0);
			return (DEBUG || DEBUG_SCHEDULE || DEBUG_TASKS || MONITORING_ENABLED || PROFILING_ENABLED) ? ++counter : 0;
		}

	};

	// the pointer type to reference task families
	using TaskFamilyPtr = std::shared_ptr<TaskFamily>;

	// a factory for a new task family
	TaskFamilyPtr createFamily() {
		return std::make_shared<TaskFamily>();
	}



	// ---------------------------------------------------------------------------------------------
	//										task reference
	// ---------------------------------------------------------------------------------------------


	/**
	 * A reference to a task utilized for managing task synchronization. Tasks may
	 * only be synchronized on if they are members of a task family.
	 */
	class task_reference {

		TaskFamilyPtr family;

		TaskPath path;

		task_reference(const TaskFamilyPtr& family, const TaskPath& path)
			: family(family), path(path) {}

	public:

		task_reference() : path(TaskPath::root()) {}

		task_reference(const TaskBasePtr& task);

		bool isDone() const {
			return (!family || family->isComplete(path));
		}

		void wait() const;

		task_reference getLeft() const {
			return task_reference ( family, path.getLeftChildPath() );
		}

		task_reference getRight() const {
			return task_reference ( family, path.getRightChildPath() );
		}

		task_reference& descentLeft() {
			path.descentLeft();
			return *this;
		}

		task_reference& descentRight() {
			path.descentRight();
			return *this;
		}

		// -- implementation details --

		const TaskFamilyPtr& getFamily() const {
			return family;
		}

		const TaskPath& getPath() const {
			return path;
		}

	};


	/**
	 * A class to aggregate task dependencies.
	 */
	class dependencies {

		using list_type = std::vector<task_reference>;

		list_type* list;

	public:

		dependencies() : list(nullptr) {}

		dependencies(std::vector<task_reference>&& deps)
			: list(new list_type(std::move(deps))) {}

		dependencies(const dependencies&) = delete;

		dependencies(dependencies&& other) : list(other.list){
			other.list = nullptr;
		}

		~dependencies() {
			delete list;
		}

		dependencies& operator=(const dependencies&) = delete;

		dependencies& operator=(dependencies&& other) {
			if (list == other.list) return *this;
			delete list;
			list = other.list;
			other.list = nullptr;
			return *this;
		}

		bool empty() const {
			return list == nullptr;
		}

		std::size_t size() const {
			return (list) ? list->size() : 0;
		}

		void add(const task_reference& ref) {
			if (!list) list = new list_type();
			list->push_back(ref);
		}

		const task_reference* begin() const {
			return (list) ? &list->front() : nullptr;
		}

		const task_reference* end() const {
			return (list) ? (&list->back()) + 1 : nullptr;
		}

	};



	// ---------------------------------------------------------------------------------------------
	//											  Tasks
	// ---------------------------------------------------------------------------------------------



	// the RT's interface to a task
	class TaskBase : public std::enable_shared_from_this<TaskBase> {

	public:

		enum class State {
			New,          // < this task has been created, but not processed by a worker yet
			Blocked,      // < this task has unfinished dependencies
			Ready,        // < this task may be processed (scheduled in work queues)
			Running,      // < this task is running
			Aggregating,  // < this split task is aggregating results (skipped if not split)
			Done          // < this task is completed
		};

		friend std::ostream& operator<<(std::ostream& out, const State& state) {
			switch(state) {
				case State::New:           return out << "New";
				case State::Blocked:       return out << "Blocked";
				case State::Ready:         return out << "Ready";
				case State::Running:       return out << "Running";
				case State::Aggregating:   return out << "Aggregating";
				case State::Done:          return out << "Done";
			}
			return out << "Invalid";
		}

	private:

		// the family this task belongs to, if null, this task is an orphan task.
		TaskFamilyPtr family;

		// the position of this task within its family
		TaskPath path;

		// A cached version of the task ID. This id
		// is only valid if this task is not an orphan
		TaskID id;

		// the current state of this task
		std::atomic<State> state;

		// the number of tasks still active before this tasks can be released
		// + 1 dependency for the release of tasks (tasks are released once this counter goes to 0)
		std::atomic<int> num_active_dependencies;

		// indicates whether this task can be split
		bool splitable;

		// split task data
		TaskBasePtr left;
		TaskBasePtr right;

		// TODO: get rid of this
		bool parallel;

		// for the processing of split tasks
		TaskBasePtr parent;                    // < a pointer to the parent to be notified upon completion
		std::atomic<int> alive_child_counter;  // < the number of active child tasks

		// for the mutation from a simple to a split task
		TaskBasePtr substitute;

		// a flag to remember that this task got a substitute, even after the
		// substitute got cut lose
		bool substituted;

	public:

		TaskBase(bool done = true)
			: family(), path(TaskPath::root()), id(TaskFamily::getNextID()),
			  state(done ? State::Done : State::New),
			  // one initial control flow dependency, released by treeture release
			  num_active_dependencies(1),
			  splitable(false), parallel(false), parent(nullptr),
			  substituted(false) {

			LOG_TASKS( "Created " << *this );
		}

		TaskBase(TaskBasePtr&& left, TaskBasePtr&& right, bool parallel)
			: family(),
			  path(TaskPath::root()), id(TaskFamily::getNextID()),
			  state(State::New),
			  // one initial control flow dependency, released by treeture release
			  num_active_dependencies(1),
			  splitable(false),
			  left(std::move(left)), right(std::move(right)), parallel(parallel),
			  parent(nullptr), alive_child_counter(0),
			  substituted(false) {

			LOG_TASKS( "Created " << *this );
			assert(this->left);
			assert(this->right);
		}

		virtual ~TaskBase() {
			LOG_TASKS( "Destroying Task " << *this );
			assert_true(isDone()) << getId() << " - " << getState();
		};

		// -- observers --

		const TaskFamilyPtr& getTaskFamily() const {
			return family;
		}

		const TaskPath& getTaskPath() const {
			return path;
		}

		TaskID getId() const {
			return id;
		}

		bool isOrphan() const {
			return !family;
		}

		std::size_t getDepth() const {
			return path.getLength();
		}

		State getState() const {
			// the substitute takes over the control of the state
			if (substitute) return substitute->state;
			return state;
		}


		// -- mutators --

		void addDependencies(const TaskDependencies& dependencies) {

			// ignore empty dependencies
			if (dependencies.empty()) return;

			// we must still be in the new state
			assert_eq(getState(),State::New);

			// increase the number of active dependencies
			num_active_dependencies += dependencies.size();

			// register dependencies
			auto thisTask = this->shared_from_this();
			for(const auto& cur : dependencies) {

				// filter out already completed tasks (some may be orphans)
				if (cur.isDone()) {
					// notify that one dependency more is completed
					dependencyDone();
					// continue with next
					continue;
				}

				// add dependency
				assert_true(cur.getFamily());
				cur.getFamily()->addDependency(thisTask,cur.getPath());
			}

		}

		void adopt(const TaskFamilyPtr& family, const TaskPath& path = TaskPath()) {
			// check that this task is not member of another family
			assert_true(isOrphan()) << "Can not adopt a member of another family.";

			// check whether there is an actual family
			if (!family) return;

			// join the family
			this->family = family;
			this->path = path;

			// update the id
			this->id = TaskID(family->getId(),path);

			// mark as complete, if already complete
			if(isDone()) family->markDone(path);

			// propagate adoption to descendants
			if (substitute) substitute->adopt(family,path);
			if (left)  left->adopt(family, path.getLeftChildPath());
			if (right) right->adopt(family, path.getRightChildPath());
		}


		// -- state transitions --

		// New -> Blocked
		void start();

		// Blocked -> Ready transition is triggered by the last dependency

		// Ready -> Running - finish() ->  Done
		void run() {

			// make sure this task is alive until the end of this function
			auto anker = this->shared_from_this();

			// log this event
			auto action = monitoring::log(monitoring::EventType::Run, this);

			// process substituted tasks
			if (substituted) {

				// just wait for this task to finish (and thus the substitute)
				wait();

				// the finish and cleanup is conducted by the callback of the substitute
				assert_eq(State::Done, state);
				assert_false(substitute);

				// done
				return;
			}


			LOG_TASKS( "Running Task " << *this );

			// check that it is allowed to run
			assert_eq(state, State::Ready);
			assert_eq(0,num_active_dependencies);

			// update state
			setState(State::Running);

			// process split tasks
			if (isSplit()) {					// if there is a left, it is a split task

				// check some assumptions
				assert(left && right);
				assert( left->state == State::New ||  left->state == State::Done);
				assert(right->state == State::New || right->state == State::Done);

				// run task sequentially if requested
				if (!parallel) {

					// TODO: implement sequential execution dependency based

					// process left first
					left->start();
					left->wait();

					// continue with the right child
					right->start();
					right->wait();

					// finish this task
					finish();
					return;

				}

				// count number of sub-tasks to be started
				assert_eq(0,alive_child_counter);
				if (left->state  == State::New) alive_child_counter++;
				if (right->state == State::New) alive_child_counter++;

				// check whether there are sub-tasks
				if (alive_child_counter == 0) {
					// perform reduction immediately since sub-tasks are done
					finish();
					// done
					return;
				}

				// get a shared pointer to this task to be passed as a parent
				TaskBasePtr thisTask = this->shared_from_this();

				// backup right since if left finishes quickly it will be reset
				TaskBasePtr r = right;

				// schedule left (if necessary)
				if (left->state == State::New) {
					LOG_TASKS( "Starting child " << *left << " of " << *this );
					left->parent = thisTask;
					left->start();
				}

				// schedule right (if necessary)
				if (r->state == State::New) {
					LOG_TASKS( "Starting child " << *r << " of " << *this );
					r->parent = thisTask;
					r->start();
				}

				// wait until this task is completed
				wait();

				// the finish call is covered by the child-task callback
				assert_true(isDone());		// after the wait, it should be done

				// processing complete

			} else {

				// run computation
				execute();

				// finish task
				finish();

			}
		}

//		void runInline() {
//			// this feature is faulty since it may skip dependencies
//			assert_not_implemented();
//
//			// forward call to substitute if present
//			if (substitute) {
//				substitute->runInline();
//				return;
//			}
//
//			// skip done tasks
//			if (isDone()) return;
//
//			// set to ready without going through the worker pool
//			setState(TaskBase::State::Ready);
//
//			// run the task
//			run();
//		}

		// Ready -> Split (if supported, otherwise remains Ready)
		virtual void split() {
			// by default, no splitting is supported
			assert_fail() << "This should not be reachable!";
		}

		// wait for the task completion
		void wait();

		bool isDone() const {
//			// forward call to substitute if present
//			if (substitute) return substitute->isDone();
			// simply check the state of this task
			return state == State::Done;
		}

		TaskBasePtr getLeft() const {
			// forward call to substitute if present
			if (substitute) return substitute->getLeft();
			return left;
		}

		TaskBasePtr getRight() const {
			// forward call to substitute if present
			if (substitute) return substitute->getRight();
			return right;
		}

		bool isSplitable() const {
			return splitable;
		}

		bool isSplit() const {
			return (bool)left;
		}

		bool isSubstituted() const {
			return substituted;
		}

		bool isReady() const {
			if (substitute) return substitute->isReady();
			return state == State::Ready;
		}

		void dependencyDone();

	protected:

		/**
		 * A hook to define the operations to be conducted by this
		 * task instance. This function will only be triggered
		 * for non-split tasks.
		 */
		virtual void execute() =0;

		/**
		 * A hook to define post-operation operations triggered after
		 * the completion of this task or the completion of its child
		 * tasks. It should be utilized to retrieve results from
		 * substitutes or child tasks and aggregate those.
		 */
		virtual void aggregate() =0;

		void setSplitable(bool value = true) {
			splitable = value;
		}

		void setSubstitute(TaskBasePtr&& newSub) {

			// can only happen if this task is in blocked or ready state
			assert_true(state == State::Blocked || state == State::Ready)
					<< "Actual state: " << state;

			// must only be set once!
			assert_false(substitute);

			// and the substitute must be valid
			assert_true(newSub);

			// the substitute must be new
			assert_true(newSub->state == State::New || newSub->state == State::Done);

			// adapt substitute
			newSub->adopt(this->family, this->path);

			// connect substitute to parent
			newSub->parent = this->shared_from_this();

			// link substitute -- with this responsibilities are transfered
			substitute = std::move(newSub);

			// remember that a substitute has been assigned
			substituted = true;

			// forget remaining dependencies
			num_active_dependencies = 0;

			// and update this state to ready
			if (state == State::Blocked) setState(State::Ready);

			// since the substitute may be processed any time, this may finish
			// any time => thus it is in the running state
			setState(State::Running);

			// if the split task is done, this one is done
			if (substitute->isDone()) {

				// finish this task
				finish();

				// now this task should be done
				assert_eq(State::Done, state);

				// and the substitute already gone
				assert_false(substitute);

			} else {

				// start the substitute
				substitute->start();

			}

		}

	private:

		bool isValidTransition(State from, State to) {
			return (from == State::New         && to == State::Blocked     ) ||
				   (from == State::Blocked     && to == State::Ready       ) ||
				   (from == State::Ready       && to == State::Running     ) ||
				   (from == State::Running     && to == State::Aggregating ) ||
				   (from == State::Aggregating && to == State::Done        ) ;
		}

		void setState(State newState) {
			// check correctness of state transitions
			assert_true(isValidTransition(state,newState))
				<< "Illegal state transition from " << state << " to " << newState;

			// make sure that the task is not released with active dependencies
			assert_true(newState != State::Ready || num_active_dependencies == 0 || substituted)
				<< "Active dependencies: " << num_active_dependencies;

//			// a substituted task may only reach the ready state
//			assert_true(newState <= State::Ready || !substitute)
//				<< "Invalid state for task with substitution!";

			// update the state
			state = newState;
			LOG_TASKS( "Updated state: " << *this );
		}

		void childDone(const TaskBase& child) {

			// check whether it is the substitute
			if (substitute.get() == &child) {

				// check state of this task
				assert_true(State::Ready == state || State::Running == state)
					<< "Actual state: " << state;

				// log state change
				LOG_TASKS( "Substitute " << substitute << " of " << *this << " done");

				// trigger completion of task
				finish();
				return;
			}

			// make sure this task is still running
			assert_eq(State::Running, state)
				<< "\tis substitute:  " << (substitute.get() == &child) << "\n"
				<< "\tis child left:  " << (left.get() == &child) << "\n"
				<< "\tis child right: " << (right.get() == &child) << "\n";

			// process a split-child
			LOG_TASKS( "Child " << child << " of " << *this << " done" );

			// this one must be a split task
			assert_true(left.get() == &child || right.get() == &child) << *this;

			// decrement active child count
			unsigned old_child_count = alive_child_counter.fetch_sub(1);

			// log alive counter
			LOG_TASKS( "Child " << child << " of " << *this << " -- alive left: " << (old_child_count - 1) );

			// check whether this was the last child
			if (old_child_count != 1) return;

			// the last child finished => finish this task
			finish();

			// LOG_TASKS( "Child " << child << " of " << *this << " done - processing complete" );
		}

		// Running -> Aggregating -> Done
		void finish() {

			LOG_TASKS( "Finishing task " << *this );

			// check precondition
			assert_true(state == State::Running)
					<< "Actual State: " << state << "\nTask: " << *this;


			// update state to aggregation
			setState(State::Aggregating);

			// log aggregation step
			LOG( "Aggregating task " << *this );

			// aggregate result (collect results)
			aggregate();

			// cut lose children
			left.reset();
			right.reset();

			// cut lose substitutes
			substitute.reset();

			// log completion
			LOG( "Aggregating task " << *this << " complete" );

			// job is done
			setState(State::Done);

			// inform the family that the job is done
			if (family) family->markDone(path);

			// notify parent
			if (parent) {
				parent->childDone(*this);
				parent.reset();
			}
		}

		// -- support printing of tasks for debugging --

		friend std::ostream& operator<<(std::ostream& out, const TaskBase& task) {

			// if substituted, print the task and its substitute
			if (task.substitute) {
				out << task.getId() << " -> " << *task.substitute;
				return out;
			}

			// if split, print the task and its children
			if (task.isSplit()) {
				out << task.getId() << " : " << task.state << " = [";
				if (task.left) out << *task.left; else out << "nil";
				out << ",";
				if (task.right) out << *task.right; else out << "nil";
				out << "] ";
				return out;
			}

			// in all other cases, just print the id
			out << task.getId() << " : " << task.state;

			// get the total number of dependencies
			std::size_t numDependencies = task.num_active_dependencies;

			// remove release dependency
			if (task.state == State::New) numDependencies -= 1;

			// print number of task dependencies
			if (numDependencies > 0) {
				out << " waiting for " << numDependencies << " task(s)";
			}

			return out;
		}

		template<typename Process, typename Split, typename R>
		friend class SplitableTask;

	};


	// ----------- Task Dependency Manager Implementations ---------------

	template<std::size_t max_depth>
	void TaskDependencyManager<max_depth>::addDependency(const TaskBasePtr& x, const TaskPath& y) {

		// locate entry
		std::size_t pos = getPosition(y);

		// load the head
		Entry* head = data[pos].load(std::memory_order_relaxed);

		// check whether this task is already completed
		if (isDone(head)) {
			// signal that this dependency is done
			x->dependencyDone();
			return;
		}

		// insert element
		Entry* entry = new Entry();
		entry->task = x;
		entry->next = head;

		// update entry pointer lock-free
		while (!data[pos].compare_exchange_weak(entry->next,entry, std::memory_order_relaxed, std::memory_order_relaxed)) {

			// check whether the task has been completed in the meanwhile
			if (isDone(entry->next)) {
				delete entry;
				// signal that this dependency is done
				x->dependencyDone();
				return;
			}

			// otherwise, repeat until it worked
		}

		// successfully inserted
	}

	template<std::size_t max_depth>
	void TaskDependencyManager<max_depth>::markComplete(const TaskPath& task) {

		// ignore tasks that are too small
		if (task.getLength() > max_depth) return;

		// mark as complete and obtain head of depending list
		auto pos = getPosition(task);
		Entry* cur = data[pos].exchange((Entry*)0x1, std::memory_order_relaxed);

		// do not process list twice (may be called multiple times due to substitutes)
		if (isDone(cur)) return;

		// signal the completion of this task
		while(cur) {

			// signal a completed dependency
			cur->task->dependencyDone();

			// move on to next entry
			Entry* next = cur->next;
			delete cur;
			cur = next;
		}

		// and its children
		if (pos >= num_entries/2) return;
		markComplete(task.getLeftChildPath());
		markComplete(task.getRightChildPath());
	}

	// -------------------------------------------------------------------



	// ------------------------- Task Reference --------------------------

	task_reference::task_reference(const TaskBasePtr& task)
		: family(task->getTaskFamily()), path(task->getTaskPath()) {
		assert_true(family) << "Unable to reference an orphan task!";
	}

	// -------------------------------------------------------------------


	// a task computing a value of type T
	template<typename T>
	class Task : public TaskBase {

		T value;

		Task<T>* substitute;

	public:

		Task() : TaskBase(false), substitute(nullptr) {}

		Task(const T& value)
			: TaskBase(true), value(value), substitute(nullptr) {}

		Task(TaskBasePtr&& left, TaskBasePtr&& right, bool parallel)
			: TaskBase(std::move(left),std::move(right), parallel), substitute(nullptr) {}


		virtual ~Task(){};

		const T& getValue() const {
			// there must not be a substitute at this time
			assert_false(substitute);
			return value;
		}

	protected:

		void execute() override {
			value = computeValue();
		}

		void aggregate() override {
			value = computeAggregate();
		}

		virtual T computeValue() {
			// the default does nothing
			return value;
		};

		virtual T computeAggregate() {
			// the default harvests the value of a potential substitute
			if (substitute) {
				// get the value
				value = std::move(substitute->value);
				// clear link
				substitute = nullptr;
			}
			// done
			return value;
		};

		void setSubstitute(TaskPtr<T>&& substitute) {
			this->substitute = substitute.get();
			TaskBase::setSubstitute(std::move(substitute));
		}

	};

	template<>
	class Task<void> : public TaskBase {
	public:

		Task() : TaskBase(false) {}

		Task(TaskBasePtr&& left, TaskBasePtr&& right, bool parallel)
			: TaskBase(std::move(left),std::move(right),parallel) {}

		virtual ~Task(){};

		void getValue() const {
		}

	protected:

		void execute() override {
			computeValue();
		}

		void aggregate() override {
			computeAggregate();
		}

		virtual void computeValue() {};
		virtual void computeAggregate() {};

	};


	template<
		typename Process,
		typename R = std::result_of_t<Process()>
	>
	class SimpleTask : public Task<R> {

		Process task;

	public:

		SimpleTask(const Process& task)
			: Task<R>(), task(task) {}

		R computeValue() override {
			return task();
		}

	};


	template<
		typename Process,
		typename Split,
		typename R = std::result_of_t<Process()>
	>
	class SplitableTask : public Task<R> {

		Process task;
		Split decompose;

		Task<R>* subTask;

	public:

		SplitableTask(const Process& c, const Split& d)
			: Task<R>(), task(c), decompose(d), subTask(nullptr) {
			// mark this task as one that can be split
			TaskBase::setSplitable();
		}

		R computeValue() override {
			// this should not be called if split
			assert_false(subTask);
			return task();
		}

		void split() override;

	};

	template<typename R, typename A, typename B, typename C>
	class SplitTask : public Task<R> {

		Task<A>& left;
		Task<B>& right;

		C merge;

	public:

		SplitTask(TaskPtr<A>&& left, TaskPtr<B>&& right, C&& merge, bool parallel)
			: Task<R>(std::move(left),std::move(right),parallel),
			  left(static_cast<Task<A>&>(*this->getLeft())),
			  right(static_cast<Task<B>&>(*this->getRight())),
			  merge(merge) {}


		R computeValue() override {
			// should not be reached
			assert_fail() << "Should always be split!";
			return {};
		}

		R computeAggregate() override {
			return merge(left.getValue(),right.getValue());
		}

	};

	template<typename A, typename B>
	class SplitTask<void,A,B,void> : public Task<void> {
	public:

		SplitTask(TaskBasePtr&& left, TaskBasePtr&& right, bool parallel)
			: Task<void>(std::move(left),std::move(right),parallel) {}

		void computeValue() override {
			// should not be reached
			assert_fail() << "Should always be split!";
		}

		void computeAggregate() override {
			// nothing to do
		}

	};

	template<typename A, typename B, typename C, typename R = std::result_of_t<C(A,B)>>
	TaskPtr<R> make_split_task(TaskDependencies&& deps, TaskPtr<A>&& left, TaskPtr<B>&& right, C&& merge, bool parallel) {
		auto res = std::make_shared<SplitTask<R,A,B,C>>(std::move(left), std::move(right), std::move(merge), parallel);
		res->addDependencies(deps);
		return res;
	}

	inline TaskPtr<void> make_split_task(TaskDependencies&& deps, TaskBasePtr&& left, TaskBasePtr&& right, bool parallel) {
		auto res = std::make_shared<SplitTask<void,void,void,void>>(std::move(left), std::move(right), parallel);
		res->addDependencies(deps);
		return res;
	}





	// ---------------------------------------------------------------------------------------------
	//											Treetures
	// ---------------------------------------------------------------------------------------------


	namespace detail {

		/**
		 * A common base class for all treetures, providing common functionality.
		 */
		template<typename T>
		class treeture_base {

			template<typename Process, typename Split, typename R>
			friend class SplitableTask;

		protected:

			TaskBasePtr task;

			treeture_base(TaskBasePtr&& task) : task(std::move(task)) {}

		public:

			using value_type = T;

			treeture_base(const treeture_base&) = delete;
			treeture_base(treeture_base&& other) : task(std::move(other.task)) {}

			treeture_base& operator=(const treeture_base&) = delete;
			treeture_base& operator=(treeture_base&& other) {
				task = std::move(other.task);
				return *this;
			}

			void wait() const {
				if (!task) return;
				task->wait();
			}

			task_reference getLeft() const {
				return task_reference(task).descentLeft();
			}

			task_reference getRight() const {
				return task_reference(task).descentRight();
			}

			task_reference getTaskReference() const {
				return task_reference(task);
			}

			operator task_reference() const {
				return getTaskReference();
			}

		};

	}

	/**
	 * A treeture, providing a reference to the state of a task as well as to
	 * the computed value upon completion.
	 */
	template<typename T>
	class treeture : public detail::treeture_base<T> {

		using super = detail::treeture_base<T>;

		friend class unreleased_treeture<T>;

	protected:

		treeture(TaskPtr<T>&& task) : super(std::move(task)) {}

	public:

		treeture(const T& value = T()) : super(std::make_shared<Task<T>>(value)) {}

		treeture(const treeture&) = delete;
		treeture(treeture&& other) = default;

		treeture& operator=(const treeture&) = delete;
		treeture& operator=(treeture&& other) = default;

		const T& get() {
			super::wait();
			return std::static_pointer_cast<Task<T>>(this->task)->getValue();
		}

	};

	/**
	 * A specialization of the general value treeture for the void type, exhibiting
	 * a modified signature for the get() member function.
	 */
	template<>
	class treeture<void> : public detail::treeture_base<void> {

		using super = detail::treeture_base<void>;

		friend class unreleased_treeture<void>;

	protected:

		treeture(TaskBasePtr&& task) : super(std::move(task)) {}

	public:

		treeture() : super(TaskBasePtr()) {}

		treeture(const treeture&) = delete;
		treeture(treeture&& other) = default;

		treeture& operator=(const treeture&) = delete;
		treeture& operator=(treeture&& other) = default;

		void get() {
			wait();
		}

	};



	template<typename Process, typename Split, typename R>
	void SplitableTask<Process,Split,R>::split() {
		// do not split a second time
		if (!TaskBase::isSplitable()) return;

		assert_true(TaskBase::State::Blocked == this->state || TaskBase::State::Ready == this->state)
				<< "Actual state: " << this->state;

		// decompose this task
		TaskPtr<R> substitute = decompose().toTask();
		assert_true(substitute);
		assert_true(substitute->state == TaskBase::State::New || substitute->state == TaskBase::State::Done);

		// record reference to sub-task
		subTask = substitute.get();

		// mutate to new task
		Task<R>::setSubstitute(std::move(substitute));

		// mark as no longer splitable
		TaskBase::setSplitable(false);

	}



	// ---------------------------------------------------------------------------------------------
	//										 Unreleased Treetures
	// ---------------------------------------------------------------------------------------------


	/**
	 * A handle to a yet unreleased task.
	 */
	template<typename T>
	class unreleased_treeture {

		TaskPtr<T> task;

	public:

		using value_type = T;

		unreleased_treeture(TaskPtr<T>&& task)
			: task(std::move(task)) {}

		unreleased_treeture(const unreleased_treeture&) =delete;
		unreleased_treeture(unreleased_treeture&&) =default;

		unreleased_treeture& operator=(const unreleased_treeture&) =delete;
		unreleased_treeture& operator=(unreleased_treeture&&) =default;

		~unreleased_treeture() { if(task) assert_ne(TaskBase::State::New,task->getState()); }

		treeture<T> release() && {
			if (task && !task->isDone()) task->start(); 		// release the task
			return treeture<T>(std::move(*this).toTask());
		}

		operator treeture<T>() && {
			return std::move(*this).release();
		}

		T get() && {
			return std::move(*this).release().get();
		}

		TaskPtr<T> toTask() && {
			auto res = std::move(std::static_pointer_cast<Task<T>>(std::move(task)));
			task.reset();
			return res;
		}

	};



	// ---------------------------------------------------------------------------------------------
	//										   Operators
	// ---------------------------------------------------------------------------------------------



	inline dependencies after() {
		return dependencies();
	}

	template<typename ... Rest>
	dependencies after(const task_reference& r, const Rest& ... rest) {
		auto res = after(std::move(rest)...);
		res.add(r);
		return res;
	}

	dependencies after(std::vector<task_reference>&& refs) {
		return std::move(refs);
	}


	inline unreleased_treeture<void> done(dependencies&& deps) {
		auto res = std::make_shared<Task<void>>();
		res->addDependencies(deps);
		return std::move(res);
	}

	inline unreleased_treeture<void> done() {
		return done(dependencies());
	}

	template<typename T>
	unreleased_treeture<T> done(dependencies&& deps, const T& value) {
		auto res = std::make_shared<Task<T>>(value);
		res->addDependencies(deps);
		return std::move(res);
	}

	template<typename T>
	unreleased_treeture<T> done(const T& value) {
		return done(dependencies(),value);
	}


	namespace detail {

		template<bool root, typename T>
		unreleased_treeture<T> init(dependencies&& deps, TaskPtr<T>&& task) {

			// add dependencies
			task->addDependencies(deps);

			// create task family if requested
			if (root) {
				task->adopt(createFamily());
			}

			// done
			return std::move(task);
		}

	}


	template<bool root, typename Action, typename T = std::result_of_t<Action()>>
	unreleased_treeture<T> spawn(dependencies&& deps, Action&& op) {
		// create and initialize the task
		return detail::init<root>(std::move(deps), TaskPtr<T>(std::make_shared<SimpleTask<Action>>(std::move(op))));
	}

	template<bool root, typename Action>
	auto spawn(Action&& op) {
		return spawn<root>(after(),std::move(op));
	}

	template<bool root, typename Action, typename Split, typename T = std::result_of_t<Action()>>
	unreleased_treeture<T> spawn(dependencies&& deps, Action&& op, Split&& split) {
		// create and initialize the task
		return detail::init<root>(std::move(deps), TaskPtr<T>(std::make_shared<SplitableTask<Action,Split>>(std::move(op),std::move(split))));
	}

	template<bool root, typename Action, typename Split>
	auto spawn(Action&& op, Split&& split) {
		return spawn<root>(after(),std::move(op),std::move(split));
	}

	inline unreleased_treeture<void> sequential(dependencies&& deps) {
		return done(std::move(deps));
	}

	inline unreleased_treeture<void> sequential() {
		return done();
	}

	template<typename A, typename B>
	unreleased_treeture<void> sequential(dependencies&& deps, unreleased_treeture<A>&& a, unreleased_treeture<B>&& b) {
		return make_split_task(std::move(deps),std::move(a).toTask(),std::move(b).toTask(),false);
	}

	template<typename A, typename B>
	unreleased_treeture<void> sequential(unreleased_treeture<A>&& a, unreleased_treeture<B>&& b) {
		return sequential(after(),std::move(a),std::move(b));
	}

	template<typename F, typename ... R>
	unreleased_treeture<void> sequential(dependencies&& deps, unreleased_treeture<F>&& f, unreleased_treeture<R>&& ... rest) {
		// TODO: conduct a binary split to create a balanced tree
		return make_split_task(std::move(deps),std::move(f).toTask(),sequential(std::move(rest)...).toTask(),false);
	}

	template<typename F, typename ... R>
	unreleased_treeture<void> sequential(unreleased_treeture<F>&& f, unreleased_treeture<R>&& ... rest) {
		return sequential(after(), std::move(f),std::move(rest)...);
	}


	inline unreleased_treeture<void> parallel(dependencies&& deps) {
		return done(std::move(deps));
	}

	inline unreleased_treeture<void> parallel() {
		return done();
	}

	template<typename A, typename B>
	unreleased_treeture<void> parallel(dependencies&& deps, unreleased_treeture<A>&& a, unreleased_treeture<B>&& b) {
		return make_split_task(std::move(deps),std::move(a).toTask(),std::move(b).toTask(),true);
	}

	template<typename A, typename B>
	unreleased_treeture<void> parallel(unreleased_treeture<A>&& a, unreleased_treeture<B>&& b) {
		return parallel(after(),std::move(a),std::move(b));
	}

	template<typename F, typename ... R>
	unreleased_treeture<void> parallel(dependencies&& deps, unreleased_treeture<F>&& f, unreleased_treeture<R>&& ... rest) {
		// TODO: conduct a binary split to create a balanced tree
		return make_split_task(std::move(deps),std::move(f).toTask(),parallel(std::move(deps),std::move(rest)...).toTask(),true);
	}

	template<typename F, typename ... R>
	unreleased_treeture<void> parallel(unreleased_treeture<F>&& f, unreleased_treeture<R>&& ... rest) {
		return parallel(after(), std::move(f),std::move(rest)...);
	}



	template<typename A, typename B, typename M, typename R = std::result_of_t<M(A,B)>>
	unreleased_treeture<R> combine(dependencies&& deps, unreleased_treeture<A>&& a, unreleased_treeture<B>&& b, M&& m, bool parallel = true) {
		return make_split_task(std::move(deps),std::move(a).toTask(),std::move(b).toTask(),std::move(m),parallel);
	}

	template<typename A, typename B, typename M, typename R = std::result_of_t<M(A,B)>>
	unreleased_treeture<R> combine(unreleased_treeture<A>&& a, unreleased_treeture<B>&& b, M&& m, bool parallel = true) {
		return reference::combine(dependencies(),std::move(a),std::move(b),std::move(m),parallel);
	}


	// ---------------------------------------------------------------------------------------------
	//											Runtime
	// ---------------------------------------------------------------------------------------------

	namespace runtime {



		// -----------------------------------------------------------------
		//						    Worker Pool
		// -----------------------------------------------------------------

		struct Worker;

		thread_local static Worker* tl_worker = nullptr;

		static void setCurrentWorker(Worker& worker) {
			tl_worker = &worker;
		}

		static Worker& getCurrentWorker();

		class SimpleTaskPool {

			using guard = std::lock_guard<SpinLock>;

			std::vector<TaskBasePtr> pool;

			mutable SpinLock lock;

		public:

			bool empty() const {
				guard lease(lock);
				return pool.empty();
			}

			void addTask(TaskBasePtr&& task) {
				guard lease(lock);
				pool.emplace_back(std::move(task));
			}

			TaskBasePtr getReadyTask() {
				guard lease(lock);

				// 1) look for some task that is ready
				for(auto& cur : pool) {
					if(!cur->isReady()) continue;
					// found a ready one, remove it from the pool
					TaskBasePtr res = std::move(cur);
					cur = std::move(pool.back());
					pool.pop_back();
					return res;
				}

				// 2) look for the largest splitable task
				auto min = std::numeric_limits<std::size_t>::max();
				for(auto& cur : pool) {
					if (!cur->isSplitable()) continue;
					min = std::min(cur->getDepth(), min);
				}

				// 3) split the largest splitable task and return it
				for(auto& cur : pool) {
					if (cur->getDepth() != min || !cur->isSplitable()) continue;

					// found a splitable one, split it
					TaskBasePtr res = std::move(cur);
					cur = std::move(pool.back());
					pool.pop_back();

					// split the task and return the result
					res->split();

					assert_true(res->isReady());
					return res;
				}

				// no work available
				return {};
			}

			std::vector<TaskBasePtr> getSnapshot() const {
				guard g(lock);
				std::vector<TaskBasePtr> res = pool;
				return res;
			}

		};

		namespace detail {

			/**
			 * A utility to fix the affinity of the current thread to the given core.
			 */
			void fixAffinity(int core) {
				static const int num_cores = std::thread::hardware_concurrency();
				cpu_set_t mask;
				CPU_ZERO(&mask);
				CPU_SET(core % num_cores, &mask);
				pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &mask);
			}

		}

		class WorkerPool;

		struct Worker {

			using duration = RuntimePredictor::duration;

			// the targeted maximum queue length
			// (more like a guideline, may be exceeded due to high demand)
			enum { max_queue_length = 8 };

			WorkerPool& pool;

			volatile bool alive;

			// list of tasks ready to run
			UnboundQueue<TaskBasePtr> queue;

			std::thread thread;

			unsigned id;

			unsigned random_seed;

			RuntimePredictor predictions;

		public:

			Worker(WorkerPool& pool, unsigned id)
				: pool(pool), alive(true), id(id), random_seed(id) { }

			Worker(const Worker&) = delete;
			Worker(Worker&&) = delete;

			Worker& operator=(const Worker&) = delete;
			Worker& operator=(Worker&&) = delete;

			void start() {
				thread = std::thread([&](){ run(); });
			}

			void poison() {
				alive = false;
			}

			void join() {
				thread.join();
			}

			void dumpState(std::ostream& out) const {
				out << "Worker " << id << " / " << thread.get_id() << ":\n";
				out << "\tQueue:\n";
				for(const auto& cur : queue.getSnapshot()) {
					out << "\t\t" << *cur << "\n";
				}
			}

		private:

			void run();

			void runTask(const TaskBasePtr& task);

			void splitTask(const TaskBasePtr& task);

			duration estimateRuntime(const TaskBasePtr& task) {
				return predictions.predictTime(task->getDepth());
			}

		public:

			void schedule(TaskBasePtr&& task);

			bool schedule_step();

		};

		class WorkerPool {

			std::vector<Worker*> workers;

			// tools for managing idle threads
			std::mutex m;
			std::condition_variable cv;

		public:

			WorkerPool() {

				int numWorkers = std::thread::hardware_concurrency();

				// parse environment variable
				if (char* val = std::getenv("NUM_WORKERS")) {
					auto userDef = std::atoi(val);
					if (userDef != 0) numWorkers = userDef;
				}

				// there must be at least one worker
				if (numWorkers < 1) numWorkers = 1;

				// create workers
				for(int i=0; i<numWorkers; ++i) {
					workers.push_back(new Worker(*this,i));
				}

				// start additional workers (worker 0 is main thread)
				for(int i=1; i<numWorkers; ++i) {
					workers[i]->start();
				}

				// make worker 0 being linked to the main thread
				setCurrentWorker(*workers.front());

				// fix affinity of main thread
				detail::fixAffinity(0);

				// fix worker id of main thread
				setCurrentWorkerID(0);

			}

			~WorkerPool() {
				// shutdown threads

				// poison all workers
				for(auto& cur : workers) {
					cur->poison();
				}

				// make work available
				workAvailable();

				// wait for their death
				for(std::size_t i=1; i<workers.size(); ++i) {
					workers[i]->join();
				}

				// free resources
				for(auto& cur : workers) {
					delete cur;
				}

			}

			static WorkerPool& getInstance() {
				static WorkerPool pool;
				return pool;
			}

			int getNumWorkers() const {
				return workers.size();
			}

			Worker& getWorker(int i) {
				return *workers[i];
			}

			Worker& getWorker() {
				return getWorker(0);
			}

			void dumpState(std::ostream& out) {
				for(const auto& cur : workers) {
					cur->dumpState(out);
				}
			}

		protected:

			friend Worker;

			void waitForWork() {
				std::unique_lock<std::mutex> lk(m);
				LOG_SCHEDULE("Going to sleep");
				cv.wait(lk);
				LOG_SCHEDULE("Woken up again");
			}

			void workAvailable() {
				// wake up all workers
				cv.notify_all();
			}

		};

		static Worker& getCurrentWorker() {
			if (tl_worker) return *tl_worker;
			return WorkerPool::getInstance().getWorker();
		}

		inline void Worker::run() {

			// fix worker ID
			setCurrentWorkerID(id);

			// log creation of worker event
			logProfilerEvent(ProfileLogEntry::createWorkerCreatedEntry());

			// fix affinity
			detail::fixAffinity(id);

			// register worker
			setCurrentWorker(*this);

			// start processing loop
			while(alive) {

				// count number of idle cycles
				int idle_cycles = 0;

				// conduct a schedule step
				while(alive && !schedule_step()) {
					// increment idle counter
					++idle_cycles;

					// wait a moment
					cpu_relax();

					// if there was no work for quite some time
					if(idle_cycles > 100000) {

						// report sleep event
						logProfilerEvent(ProfileLogEntry::createWorkerSuspendedEntry());

						// wait for work by putting thread to sleep
						pool.waitForWork();

						// report awakening
						logProfilerEvent(ProfileLogEntry::createWorkerResumedEntry());
				
						// reset cycles counter
						idle_cycles = 0;
					}
				}
			}

			// log worker termination event
			logProfilerEvent(ProfileLogEntry::createWorkerDestroyedEntry());

			// done

		}

		void Worker::runTask(const TaskBasePtr& task) {

			// the splitting of a task may provide a done substitute => skip those
			if (task->isDone()) return;

			LOG_SCHEDULE("Starting task " << task);

			if (task->isSplit()) {
				task->run();
			} else {

				logProfilerEvent(ProfileLogEntry::createTaskStartedEntry(task->getId()));

				// check whether this run needs to be sampled
				auto level = task->getDepth();
				if (level == 0) {

					// level 0 does not need to be recorded (orphans)
					task->run();

				} else {

					// take the time to make predictions
					auto start = RuntimePredictor::clock::now();
					task->run();
					auto time = RuntimePredictor::clock::now() - start;
					predictions.registerTime(level,time);

				}

				logProfilerEvent(ProfileLogEntry::createTaskEndedEntry(task->getId()));

			}

			LOG_SCHEDULE("Finished task " << task);
		}

		void Worker::splitTask(const TaskBasePtr& task) {
			using namespace std::chrono_literals;

			// the threshold for estimated task to be split
			static const auto taskTimeThreshold = CycleCount(3*1000*1000);

			// only split the task if it is estimated to exceed a threshold
			if (task->isSplitable() && (task->getDepth() == 0 || estimateRuntime(task) > taskTimeThreshold)) {

				// split this task
				task->split();

			}
		}

		inline void Worker::schedule(TaskBasePtr&& task) {

			// assert that task has no unfinished dependencies
			assert_true(task->isReady());

			// add task to queue
			LOG_SCHEDULE( "Queue size before: " << queue.size() );

			// if the queue is full, run task directly
			//    NOTE: to avoid deadlocks due to invalid introduced dependencies
			//          only non-split tasks may be processed directly
			if (pool.getNumWorkers() == 1 || (queue.size() > max_queue_length && !task->isSplit())) {

				// run task directly, avoiding to build up to long queues
				runTask(task);

				// done
				return;
			}


			// add task to queue
			queue.push_back(task);

			// signal available work
			if (queue.size() > max_queue_length/2) {
				pool.workAvailable();
			}

			// log new queue length
			LOG_SCHEDULE( "Queue size after: " << queue.size() );

		}


		inline bool Worker::schedule_step() {

			// process a task from the local queue
			if (TaskBasePtr t = queue.pop_front()) {

				// check precondition of task
				assert_true(t->isReady());

				// if the queue is not full => create more tasks
				if (queue.size() < (max_queue_length*3)/4) {

					LOG_SCHEDULE( "Splitting tasks @ queue size: " << queue.size() );

					// split task
					splitTask(t);
				}

				// process this task
				runTask(t);
				return true;
			}

			// check that there are other workers
			int numWorker = pool.getNumWorkers();
			if (numWorker <= 1) return false;

			// otherwise, steal a task from another worker
			Worker& other = pool.getWorker(rand_r(&random_seed) % numWorker);
			if (this == &other) {
				return schedule_step();
			}

			// try to steal a task from another queue
			if (TaskBasePtr t = other.queue.try_pop_back()) {

				// log creation of worker event
				logProfilerEvent(ProfileLogEntry::createTaskStolenEntry(t->getId()));

				LOG_SCHEDULE( "Stolen task: " << t );

				// split task the task (since there is not enough work in the queue)
				splitTask(t);

				// process task
				runTask(t);
				return true;	// successfully completed a task
			}

			// no task found => wait a moment
			cpu_relax();
			return false;
		}

	}

	namespace monitoring {

		inline std::ostream& operator<<(std::ostream& out, const Event& e) {
			switch(e.type) {
			case EventType::Run:    		return out << "Running task            " << *e.task;
			case EventType::RunDirect:  	return out << "Running direct task     " << *e.task;
			case EventType::Split:  		return out << "Splitting task          " << *e.task;
			case EventType::Wait:   		return out << "Waiting for task        " << *e.task;
			case EventType::DependencyWait: return out << "Waiting for dependency: " << e.taskId;
			}
			return out << "Unknown Event";
		}

	}// end namespace monitoring


	void TaskBase::start() {
		LOG_TASKS("Starting " << *this );

		// check that the given task is a new task
		assert_eq(TaskBase::State::New, state);

		// move to next state
		setState(State::Blocked);

		// split tasks by default up to a given level
		// TODO: make this depth hardware dependent
		if (!isOrphan() && isSplitable() && getDepth() < 4) {

			// split this task
			split();

			// if the task is done, we are fine (some split end up in completed tasks)
			if (isDone()) return;
		}

		// release dummy-dependency to get task started
		dependencyDone();
	}

	void TaskBase::dependencyDone() {


		// decrease the number of active dependencies
		int oldValue = num_active_dependencies.fetch_sub(1);

		// react based on old value
		if (oldValue < 0) {

			// this task was previously substituted, thus no dependencies are present
			assert_true(substituted);

			// reset number of dependencies counter
			num_active_dependencies = 0;

			// done
			return;
		}

		// if the old value was not 1, the new one is not 0, thus there are dependencies left
		if (oldValue != 1) return;

		// make sure that this is never reached when there is a dependency left
		assert_eq(num_active_dependencies, 0);

		// at this point the state must not be new
		assert_ne(State::New, state)
			<< "A new task must not reach a state where its last dependency is released.";

		// actually, every task here must be in blocked state
		assert_eq(State::Blocked, state);

		// update the state to ready
		// (this can only be reached by one thread)
		setState(State::Ready);

		// TODO: actively distribute initial tasks, by assigning
		// them to different workers;

		// TODO: do the following only for top-level tasks!!
		if (!isOrphan() && getDepth() < 4) {

			// actively select the worker to issue the task to
			auto& pool = runtime::WorkerPool::getInstance();
			int num_workers = pool.getNumWorkers();

			auto path = getTaskPath().getPath();
			auto depth = getDepth();

			auto trgWorker = (depth==0) ? 0 : (path * num_workers) / (1 << depth);

			// submit this task to the selected worker
			pool.getWorker(trgWorker).schedule(this->shared_from_this());

		} else {

			// enqueue task to be processed by local worker
			runtime::getCurrentWorker().schedule(this->shared_from_this());

		}

	}

	inline void TaskBase::wait() {
		// log this event
		auto action = monitoring::log(monitoring::EventType::Wait, this);

		LOG_TASKS("Waiting for " << *this );

		// check that this task has been started before
		assert_lt(State::New,state);

		// wait until this task is finished
		while(!isDone()) {
			// make some progress
			runtime::getCurrentWorker().schedule_step();
		}
	}

	inline void task_reference::wait() const {
		// log this event
		// auto action = monitoring::log(monitoring::EventType::DependencyWait, TaskID(family->getId(),path));

		// keep narrowing scope until done (implicit in isDone)
		while(!isDone()) {
			runtime::getCurrentWorker().schedule_step();
		}
	}

} // end namespace reference
} // end namespace impl
} // end namespace core
} // end namespace api
} // end namespace allscale


void __dumpRuntimeState() {
	std::cout << "\n ------------------------- Runtime State Dump -------------------------\n";
	allscale::api::core::impl::reference::monitoring::ThreadState::dumpStates(std::cout);
	allscale::api::core::impl::reference::runtime::WorkerPool::getInstance().dumpState(std::cout);
	std::cout << "\n ----------------------------------------------------------------------\n";
}


