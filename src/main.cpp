extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Query(const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
#ifndef DEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= Version::PROJECT;
	*path += ".log"sv;
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef DEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_skse->IsEditor()) {
		logger::critical("Loaded in editor, marking as incompatible"sv);
		return false;
	}

	const auto ver = a_skse->RuntimeVersion();
	if (ver < SKSE::RUNTIME_1_5_39) {
		logger::critical(FMT_STRING("Unsupported runtime version {}"), ver.string());
		return false;
	}

	return true;
}

#include "ResurrectionAPI.h"

class Subscribers
{
	std::vector<std::unique_ptr<ResurrectionAPI>> data;
	std::mutex subscribers_mutex;

public:
	void add(std::unique_ptr<ResurrectionAPI> api)
	{
		const std::lock_guard<std::mutex> lock(subscribers_mutex);
		data.push_back(std::move(api));
	}

	bool should_resurrect(RE::Actor* a) const
	{
		bool ans = false;
		for (auto& i : data) {
			ans = ans || i->should_resurrect(a);
		}
		return ans;
	}

	void resurrect(RE::Actor* a)
	{
		for (auto& i : data) {
			if (i->should_resurrect(a)) {
				i->resurrect(a);
				return;
			}
		}
	}
} subscribers;

extern "C" DLLEXPORT void ResurrectionAPI_AddSubscriber(std::unique_ptr<ResurrectionAPI> api)
{
	return subscribers.add(std::move(api));
}

void Character__invalidate_cached(RE::Actor* a, RE::ActorValue av)
{
	return _generic_foo_<37534, decltype(Character__invalidate_cached)>::eval(a, av);
}

void resurrect(RE::Actor* a) {
	return subscribers.resurrect(a);
}

bool should_resurrect(RE::Actor* a) {
	return subscribers.should_resurrect(a);
}

bool should_cancel_dmg(RE::Actor* a, float new_dmg_mod)
{
	float old_dmg_mod = a->healthModifiers.modifiers[RE::ACTOR_VALUE_MODIFIERS::kDamage];
	a->healthModifiers.modifiers[RE::ACTOR_VALUE_MODIFIERS::kDamage] = new_dmg_mod;
	Character__invalidate_cached(a, RE::ActorValue::kHealth);
	bool ans = a->GetActorValue(RE::ActorValue::kHealth) <= 0.0f && should_resurrect(a);
	if (ans) {
		resurrect(a);
	}
	a->healthModifiers.modifiers[RE::ACTOR_VALUE_MODIFIERS::kDamage] = old_dmg_mod;
	Character__invalidate_cached(a, RE::ActorValue::kHealth);
	return ans;
}

void apply_canceldamage()
{
	// SkyrimSE.exe+62131C
	uintptr_t ret_cancel = REL::ID(37523).address() + 0x1fc;

	// SkyrimSE.exe+62128A
	uintptr_t ret_nocancel = REL::ID(37523).address() + 0x16a;

	struct Code : Xbyak::CodeGenerator
	{
		Code(uintptr_t func_addr, uintptr_t ret_nocancel, uintptr_t ret_cancel)
		{
			Xbyak::Label nocancel;

			// rdi  = modifiers
			// rbp  = modifier
			// xmm6 = new_dmg_mod
			// rbx  = a
			// xmm1 <- old_dmg_mod
			cmp(esi, 0x18);
			jnz(nocancel);

			mov(rcx, rbx);
			movss(xmm1, xmm6);
			mov(rax, func_addr);
			movaps(xmm9, xmm0);  // restore xmm0
			call(rax);
			movaps(xmm0, xmm9);  // restore xmm0
			test(al, al);
			jz(nocancel);

			mov(rax, ret_cancel);
			jmp(rax);


			L(nocancel);
			movss(xmm1, dword[rdi + rbp * 4]);  // restore xmm1
			mov(rax, ret_nocancel);
			jmp(rax);
		}
	} xbyakCode{ uintptr_t(should_cancel_dmg), ret_nocancel, ret_cancel };

	FenixUtils::add_trampoline<5, 37523, 0x165>(&xbyakCode);  // SkyrimSE.exe+621285
}

class IsFatalAttackHook
{
public:
	static void Hook()
	{
		_isFatalAttack = SKSE::GetTrampoline().write_call<5>(REL::ID(21285).address() + 0x3b, isFatalAttack);  // SkyrimSE.exe+2E237B
	}

private:
	static bool isFatalAttack(RE::Actor* attacker, RE::Actor* victim) {
		return !should_resurrect(victim) && _isFatalAttack(attacker, victim);
	}
	static inline REL::Relocation<decltype(isFatalAttack)> _isFatalAttack;
};

void apply_hooks()
{
	IsFatalAttackHook::Hook();  // for killmoves
	apply_canceldamage();
}

static void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
{
	switch (message->type) {
	case SKSE::MessagingInterface::kDataLoaded:
		apply_hooks();

		break;
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	auto g_messaging = reinterpret_cast<SKSE::MessagingInterface*>(a_skse->QueryInterface(SKSE::LoadInterface::kMessaging));
	if (!g_messaging) {
		logger::critical("Failed to load messaging interface! This error is fatal, plugin will not load.");
		return false;
	}

	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 10);

	g_messaging->RegisterListener("SKSE", SKSEMessageHandler);

	return true;
}
