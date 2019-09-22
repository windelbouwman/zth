#ifndef __ZTH_FSM_H
#define __ZTH_FSM_H
/*
 * Zth (libzth), a cooperative userspace multitasking library.
 * Copyright (C) 2019  Jochem Rutgers
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef __cplusplus
#include <libzth/time.h>
#include <libzth/waiter.h>

#include <set>
#include <map>

namespace zth {

	namespace guards {
		enum End { end = 0 };
		template <typename Fsm> TimeInterval always(Fsm& fsm) { return TimeInterval(); }
		template <unsigned int s, typename Fsm> TimeInterval timeout_s(Fsm& fsm) { static TimeInterval const ti((time_t)s); return ti - fsm.dt(); }
		template <unsigned int ms, typename Fsm> TimeInterval timeout_ms(Fsm& fsm) { static TimeInterval const ti((double)ms * 1e-3); return ti - fsm.dt(); }
		template <uint64_t us, typename Fsm> TimeInterval timeout_us(Fsm& fsm) { static TimeInterval const ti((double)us * 1e-6); return ti - fsm.dt(); }
	}

	template <typename Fsm>
	class FsmGuard {
	public:
		typedef TimeInterval (*Function)(Fsm& fsm);
		constexpr FsmGuard(Function function)
			: m_function(function)
		{}

		FsmGuard(guards::End) : m_function() {}

		TimeInterval operator()(Fsm& fsm) const { zth_assert(valid()); return m_function(fsm); }
		constexpr bool valid() const { return m_function; }
	private:
		Function m_function;
	};

	template <typename Fsm>
	struct FsmDescription {
		union {
			typename Fsm::State state;
			FsmDescription<Fsm> const* stateAddr;
		};
		FsmGuard<Fsm> guard;
	};

	template <typename Fsm>
	class FsmFactory {
	public:
		constexpr FsmFactory(FsmDescription<Fsm>* description)
			: m_description(description)
			, m_compiled()
		{} 

		__attribute__((malloc,returns_nonnull,warn_unused_result)) Fsm* create() const { return new Fsm(*this); }

		FsmDescription<Fsm> const* description() const { compile(); return m_description; }

	protected:
		void compile() const {
			if(likely(m_compiled))
				return;

			std::map<typename Fsm::State,FsmDescription<Fsm> const*> stateAddr;
			FsmDescription<Fsm>* p = m_description;
			while(true) {
				// Register this state
				zth_assert(stateAddr.count(p->state) == 0);
				stateAddr[p->state] = const_cast<FsmDescription<Fsm> const*>(p);

				if(!p->guard.valid())
					// End of description
					break;

				// Go to end of guard list
				while((p++)->guard.valid());
			}

			// Patch State references
			p = m_description;
			while(true) {
				if(!p->guard.valid())
					// End of description
					break;

				// Patch this state's guard list
				while((p++)->guard.valid()) {
					zth_assert(stateAddr.count(p->state) == 1);
					p->stateAddr = stateAddr[p->state];
				}
			}

			m_compiled = true;
		}

	private:
		FsmDescription<Fsm>* const m_description;
		mutable bool m_compiled;
	};

	template <typename State_>
	class Fsm : public UniqueID<Fsm<void> > {
	protected:
		enum EvalState { evalCompile, evalInit, evalReset, evalIdle, evalState, evalRecurse };
		typedef FsmDescription<Fsm> const* StateAddr;

	public:
		typedef State_ State;
		typedef FsmFactory<Fsm> Factory;
		typedef FsmDescription<Fsm> Description[];

		explicit Fsm(Factory const& factory, char const* name = "FSM")
			: UniqueID(name)
			, m_factory(&factory)
			, m_evalState(evalInit)
		{}
		
		explicit Fsm(Description description, char const* name = "FSM")
			: UniqueID(name)
			, m_description(description)
			, m_evalState(evalCompile)
		{}
		
		virtual ~Fsm() {}

		State const& state() const {
			switch(m_evalState) {
			default:
				return m_state->state;
			case evalCompile:
				return m_description->state;
			case evalInit:
				return m_factory->description()->state;
			case evalReset:
				return m_compiledDescription->state;
			}
		}

		Timestamp const& t() const { return m_t; }
		TimeInterval dt() const { return Timestamp::now() - t(); }

		State const& next() const {
			switch(m_evalState) {
			default:
				zth_assert(exit() || m_stateNext == m_state);
				return m_stateNext->state;
			case evalCompile:
				return m_description->state;
			case evalInit:
				return m_factory->description()->state;
			case evalReset:
				return m_compiledDescription->state;
			}
		}

		void reset() {
			zth_dbg(fsm, "[%s] Reset", id_str());
			switch(m_evalState) {
			default:
				m_evalState = evalReset;
				break;
			case evalCompile:
			case evalInit:
			case evalReset:
				break;
			}
		}

		bool entry() const { return m_entry; }
		bool exit() const { return m_exit; }

		TimeInterval eval(bool alwaysDoCallback = false) {
			bool didCallback = false;
			TimeInterval wait = TimeInterval::null();

			switch(m_evalState) {
			case evalCompile:
				m_compiledDescription = Factory(m_description).description();
				if(false) {
					ZTH_FALLTHROUGH
			case evalInit:
					m_compiledDescription = (StateAddr)m_factory->description();
				}
				ZTH_FALLTHROUGH
			case evalReset:
				m_stateNext = m_state = m_compiledDescription;
				m_evalState = evalState;
				zth_dbg(fsm, "[%s] Initial state %s", id_str(), str(state()).c_str());
				m_t = Timestamp::now();
				m_exit = false;
				m_entry = true;
				callback();
				m_entry = false;
				didCallback = true;
				ZTH_FALLTHROUGH
			case evalIdle: {
				do {
					m_evalState = evalState;
					while((wait = evalOnce()).hasPassed())
						didCallback = true;
					if(alwaysDoCallback && !didCallback)
						callback();
				} while(m_evalState == evalRecurse);
				m_evalState = evalIdle;
				break;
			}
			case evalState:
				m_evalState = evalRecurse;
				ZTH_FALLTHROUGH
			case evalRecurse:
				break;
			}
			return wait;
		}

		void run() {
			TimeInterval wait = eval();
			if(!m_state->guard.valid()) {
		done:
				zth_dbg(fsm, "[%s] Final state %s", id_str(), str(state()).c_str());
				return;
			}

			PolledMemberWaiting<Fsm> wakeup(*this, &Fsm::trigger_, wait);
			do {
				if(wakeup.interval().isInfinite()) {
					m_trigger.wait();
				} else {
					wakeup.setInterval(wait);
					scheduleTask(wakeup);
					m_trigger.wait();
					unscheduleTask(wakeup);
				}
				wait = eval();
			} while(m_state->guard.valid());
			goto done;
		}

		void trigger() {
			m_trigger.signal(false);
		}
	
	private:
		bool trigger_() {
			m_trigger.signal(false);
			return true;
		}

		TimeInterval evalOnce() {
			TimeInterval wait = TimeInterval::infinity();
			StateAddr p = m_state;
			while(p->guard.valid()) {
				TimeInterval ti((p++)->guard(*this));
				if(ti.hasPassed()) {
					if(p->stateAddr == m_state) {
						zth_dbg(fsm, "[%s] Selfloop of state %s", id_str(), str(state()).c_str());
						return wait; // Note that wait is returned, which indicates that no transition has been taken.
					}

					setState(p->stateAddr);
					wait = ti; // NVRO
					return wait; // Return any passed TimeInterval, which indicates that a transition was taken.
				} else if(ti < wait) {
					// Save shortest wait interval.
					wait = ti;
				}
			}
			zth_dbg(fsm, "[%s] State %s blocked for %s", id_str(), str(state()).c_str(), wait.str().c_str());
			return wait;
		}

		void setState(StateAddr nextState) {
			zth_dbg(fsm, "[%s] Transition %s -> %s", id_str(), str(state()).c_str(), str(nextState->state).c_str());
			m_stateNext = nextState;

			m_exit = true;
			callback();
			m_exit = false;

			m_state = nextState;
			m_t = Timestamp::now();

			m_entry = true;
			callback();
			m_entry = false;
		}

	protected:
		virtual void callback() = 0;

	private:
		union {
			FsmDescription<Fsm>* m_description; // only valid during stateCompile
			Factory const* m_factory; // only valid during stateInit
			FsmDescription<Fsm> const* m_compiledDescription; // otherwise
		};
		StateAddr m_state;
		StateAddr m_stateNext;
		EvalState m_evalState;
		Timestamp m_t;
		bool m_entry;
		bool m_exit;
		Signal m_trigger;
	};
	
	template <typename State_, typename CallbackArg_ = void>
	class FsmCallback : public Fsm<State_> {
	public:
		typedef Fsm<State_> base;
		typedef CallbackArg_ CallbackArg;
		typedef void (*Callback)(FsmCallback&, CallbackArg);

		explicit FsmCallback(typename base::Factory const& factory, Callback callback = NULL, CallbackArg const& callbackArg = CallbackArg(), char const* name = "FSM")
			: base(factory, name)
			, m_callback(callback)
			, m_callbackArg(callbackArg)
		{}
		
		explicit FsmCallback(typename base::Description description, Callback callback = NULL, CallbackArg const& callbackArg = CallbackArg(), char const* name = "FSM")
			: base(description, name)
			, m_callback(callback)
			, m_callbackArg(callbackArg)
		{}

		virtual ~FsmCallback() {}

	protected:
		virtual void callback() {
			if(m_callback) {
				zth_dbg(fsm, "[%s] Callback for %s%s", this->id_str(), str(this->state()).c_str(), this->entry() ? " (entry)" : this->exit() ? " (exit)" : "");
				m_callback(*this, m_callbackArg);
			}
		}

	private:
		Callback const m_callback;
		CallbackArg const m_callbackArg;
	};
	
	template <typename State_>
	class FsmCallback<State_, void> : public Fsm<State_> {
	public:
		typedef Fsm<State_> base;
		typedef void (*Callback)(FsmCallback&);

		explicit FsmCallback(typename base::Factory const& factory, Callback callback = NULL, char const* name = "FSM")
			: base(factory, name)
			, m_callback(callback)
		{}

		explicit FsmCallback(typename base::Description description, Callback callback = NULL, char const* name = "FSM")
			: base(description, name)
			, m_callback(callback)
		{}

		virtual ~FsmCallback() {}

	protected:
		virtual void callback() {
			if(m_callback) {
				zth_dbg(fsm, "[%s] Callback for %s%s", this->id_str(), str(this->state()).c_str(), this->entry() ? " (entry)" : this->exit() ? " (exit)" : "");
				m_callback(*this);
			}
		}

	private:
		Callback const m_callback;
	};

} // namespace
#endif // __cplusplus
#endif // __ZTH_FSM_H
