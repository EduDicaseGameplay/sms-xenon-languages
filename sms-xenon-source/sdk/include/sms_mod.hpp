// sms_mod.hpp: a C++ face on sms_mod.h, header only (C++11 or later).
//
// The game only ever sees the C API: this wraps it so a mod can subscribe
// lambdas, read payloads by type, and keep its state in a class. Nothing here
// crosses into the game but plain C calls, so a mod built with any C++
// compiler works with every game build of the same major API version.
//
//   #include <sms_mod.hpp>
//
//   struct Hello : sms::Mod {
//       using sms::Mod::Mod;
//       int load() override {
//           on<SmsEvStage>(SMS_EV_STAGE_LOADED, [this](SmsEvStage& s) {
//               info("stage %d, episode %d", s.area, s.episode);
//           });
//           on(SMS_EV_DRAW, [this](SmsEvent&) {
//               draw().text(20, 400, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, "Hello");
//           });
//           return 0;
//       }
//   };
//   SMS_MOD_CLASS(Hello)
//
// Exceptions never reach the game: a handler that throws is logged and
// treated as SMS_CONTINUE; a constructor or load() that throws refuses the
// mod.
#ifndef SMS_MOD_HPP
#define SMS_MOD_HPP

#include "sms_mod.h"

#include <stdarg.h>
#include <stdio.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sms {

// The payload of `ev` as a T, or null when the event carries none or a
// smaller one (a game older than the header).
template <class T> T* payload(SmsEvent& ev)
{
	return ev.data && ev.size >= sizeof(T) ? static_cast<T*>(ev.data) : nullptr;
}

class Mod {
public:
	Mod(SmsMod* mod, const SmsApi* api) : mod_(mod), api_(api) {}
	virtual ~Mod() {}
	Mod(const Mod&) = delete;
	Mod& operator=(const Mod&) = delete;

	// Called once the object exists: subscribe, add options, load images.
	// 0 loads the mod; anything else refuses it.
	virtual int load() { return 0; }

	SmsMod* handle() const { return mod_; }
	const SmsApi& api() const { return *api_; }
	const SmsCoreApi& core() const { return *api_->core; }
	const SmsGameApi& game() const { return *api_->game; }
	const SmsPlayerApi& player() const { return *api_->player; }
	const SmsCameraApi& camera() const { return *api_->camera; }
	const SmsDrawApi& draw() const { return *api_->draw; }
	const SmsInputApi& input() const { return *api_->input; }
	const SmsConfigApi& config() const { return *api_->config; }
	const SmsAudioApi& audio() const { return *api_->audio; }

	// ---------------------------------------------------------- events

	// fn(SmsEvent&), returning SMS_CONTINUE / SMS_CANCEL, or void (continue).
	template <class F> int on(uint32_t type, F fn, int priority = SMS_PRIORITY_NORMAL)
	{
		handlers_.emplace_back(new Handler{this, wrap(std::move(fn))});
		return core().subscribe(mod_, type, &Mod::thunk, handlers_.back().get(), priority);
	}

	// fn(T&) with the payload as a T (SmsEvTick, SmsEvStage, ...); events
	// whose payload is smaller than T are not passed on.
	template <class T, class F> int on(uint32_t type, F fn, int priority = SMS_PRIORITY_NORMAL)
	{
		return on(type, [fn](SmsEvent& ev) -> int {
			T* p = payload<T>(ev);
			return p ? call(fn, *p) : SMS_CONTINUE;
		}, priority);
	}

	void off(int handle) { core().unsubscribe(mod_, handle); }

	// A custom event's id, and sending one.
	uint32_t eventId(const char* name) const { return core().event_id(name); }
	template <class T> bool emit(uint32_t type, T& data) { return core().emit(mod_, type, &data, sizeof data) != 0; }

	// ---------------------------------------------------------- the rest

	void info(const char* fmt, ...) SMS_MOD_PRINTF(2, 3)
	{
		va_list ap;
		va_start(ap, fmt);
		vlog(SMS_LOG_INFO, fmt, ap);
		va_end(ap);
	}
	void warn(const char* fmt, ...) SMS_MOD_PRINTF(2, 3)
	{
		va_list ap;
		va_start(ap, fmt);
		vlog(SMS_LOG_WARN, fmt, ap);
		va_end(ap);
	}
	void debug(const char* fmt, ...) SMS_MOD_PRINTF(2, 3)
	{
		va_list ap;
		va_start(ap, fmt);
		vlog(SMS_LOG_DEBUG, fmt, ap);
		va_end(ap);
	}

	SmsGameState state() const
	{
		SmsGameState s = SmsGameState();
		s.size         = sizeof s;
		game().state(&s);
		return s;
	}
	// Mario now; .valid is 0 outside stages.
	SmsPlayer mario() const
	{
		SmsPlayer p = SmsPlayer();
		p.size      = sizeof p;
		player().get(&p);
		return p;
	}
	SmsPad pad(int port = 0) const
	{
		SmsPad p = SmsPad();
		p.size   = sizeof p;
		input().pad(port, &p);
		return p;
	}

	int setting(const char* key, int def) const { return config().get_int(mod_, key, def); }
	float setting(const char* key, float def) const { return config().get_float(mod_, key, def); }
	std::string setting(const char* key, const char* def) const { return config().get_string(mod_, key, def); }

	// Options for the Mods menu.
	void optionBool(const char* key, const char* label, bool def)
	{
		SmsOption o = option(key, label, SMS_OPTION_BOOL);
		o.def       = def ? 1.0f : 0.0f;
		config().add_option(mod_, &o);
	}
	void optionNumber(const char* key, const char* label, float def, float min, float max, float step,
	                  bool integer = true)
	{
		SmsOption o = option(key, label, integer ? SMS_OPTION_INT : SMS_OPTION_FLOAT);
		o.def       = def;
		o.min       = min;
		o.max       = max;
		o.step      = step;
		config().add_option(mod_, &o);
	}
	// `choices` must outlive the call only: the game copies them.
	void optionChoice(const char* key, const char* label, const std::vector<const char*>& choices, int def = 0)
	{
		SmsOption o    = option(key, label, SMS_OPTION_CHOICE);
		o.def          = (float)def;
		o.choices      = choices.data();
		o.choice_count = (int)choices.size();
		config().add_option(mod_, &o);
	}

	int bindKey(const char* action, const char* defaultKey) { return input().bind_key(mod_, action, defaultKey); }

private:
	struct Handler {
		Mod* self;
		std::function<int(SmsEvent&)> fn;
	};

	template <class F, class A> static int call(F& fn, A& a)
	{
		return callImpl(fn, a, std::is_void<decltype(fn(a))>());
	}
	template <class F, class A> static int callImpl(F& fn, A& a, std::true_type)
	{
		fn(a);
		return SMS_CONTINUE;
	}
	template <class F, class A> static int callImpl(F& fn, A& a, std::false_type) { return (int)fn(a); }

	template <class F> static std::function<int(SmsEvent&)> wrap(F fn)
	{
		return [fn](SmsEvent& ev) mutable -> int { return call(fn, ev); };
	}

	static int thunk(SmsMod*, SmsEvent* ev, void* user)
	{
		Handler* h = static_cast<Handler*>(user);
		try {
			return h->fn(*ev);
		} catch (const std::exception& e) {
			h->self->warn("a handler of event %u threw: %s", (unsigned)ev->type, e.what());
		} catch (...) {
			h->self->warn("a handler of event %u threw", (unsigned)ev->type);
		}
		return SMS_CONTINUE;
	}

	SmsOption option(const char* key, const char* label, int type) const
	{
		SmsOption o = SmsOption();
		o.size      = sizeof o;
		o.key       = key;
		o.label     = label;
		o.type      = type;
		return o;
	}

	void vlog(int level, const char* fmt, va_list ap)
	{
		char buf[1024];
		vsnprintf(buf, sizeof buf, fmt, ap);
		core().log(mod_, level, "%s", buf);
	}

	SmsMod* mod_;
	const SmsApi* api_;
	std::vector<std::unique_ptr<Handler>> handlers_;
};

} // namespace sms

// The entry points for a mod that is one sms::Mod subclass T: the game's
// sms_mod_load makes a T and calls its load(); sms_mod_unload deletes it.
#define SMS_MOD_CLASS(T)                                                                                   \
	SMS_MOD_DEFINE()                                                                                       \
	static T* sms_mod_instance_ = nullptr;                                                                 \
	SMS_MOD_EXTERN_C SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* api)                       \
	{                                                                                                      \
		try {                                                                                              \
			sms_mod_instance_ = new T(mod, api);                                                           \
			const int rc      = sms_mod_instance_->load();                                                 \
			if (rc != 0) {                                                                                 \
				delete sms_mod_instance_;                                                                  \
				sms_mod_instance_ = nullptr;                                                               \
			}                                                                                              \
			return rc;                                                                                     \
		} catch (...) {                                                                                    \
			return -1;                                                                                     \
		}                                                                                                  \
	}                                                                                                      \
	SMS_MOD_EXTERN_C SMS_MOD_EXPORT void sms_mod_unload(SmsMod*)                                           \
	{                                                                                                      \
		delete sms_mod_instance_;                                                                          \
		sms_mod_instance_ = nullptr;                                                                       \
	}

#endif
