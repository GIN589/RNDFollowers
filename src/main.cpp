// RNDFollowers v2 - SKSE plugin.
// Gives player followers hunger, thirst, fatigue and progressive diseases that follow
// "Realistic Needs and Diseases" (RND): same rates, same stage thresholds, same food values,
// and the same penalty effects (via the ability clones stored in RNDFollowers.esp).
// Needs/diseases are stored in the SKSE co-save. The only form the plugin adds to actors
// are the RNDF_* abilities, so removing them again is clean (see Uninstall in the INI).

#include "PCH.h"

#include <SimpleIni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace std::literals;
namespace logger = SKSE::log;

namespace
{
	constexpr auto kRND = "RealisticNeedsandDiseases.esp"sv;
	constexpr auto kOur = "RNDFollowers.esp"sv;
	enum Need { kHunger = 0, kThirst = 1, kFatigue = 2 };
	constexpr const char* kDiseaseNames[6]{ "Witbane", "Rockjoint", "Rattles", "BrainRot", "BoneBreakFever", "Ataxia" };

	// ------------------------------------------------------------------ config
	struct Config
	{
		bool  includeAnimals{ false };
		bool  notify{ true };
		int   notifyStage{ 3 };
		int   tickMs{ 1000 };
		bool  uninstall{ false };
		bool  respectRndState{ true };
		bool  verbose{ true };            // detailed log in RNDFollowers.log
		int   summarySeconds{ 30 };       // real seconds between follower summary lines (0 = off)
		bool  fullStomachSlow{ false };   // hunger stage 0 ability (RND slows the player when full)
		float startNeed[3]{ 20.0f, 20.0f, 10.0f };
		float sleepRadius{ 1500.0f };
		float restPerHour{ 40.0f };
		float sleepMult[2]{ 0.5f, 0.5f };
		float combatFatigueMult{ 1.5f };
		float hitDiseaseChance{ 4.0f };
		float stageHours{ 6.0f };
		bool  diseaseAddsFatigue{ true };
		std::vector<std::string> diseaseSources{ "ActorTypeAnimal", "ActorTypeUndead" };
		std::string msgHunger[6]{ "", "is peckish", "is hungry", "is very hungry", "is starving", "is famished" };
		std::string msgThirst[5]{ "", "is a bit thirsty", "is thirsty", "is very thirsty", "is parched" };
		std::string msgFatigue[5]{ "", "is a bit tired", "is tired", "is very tired", "is exhausted" };
		std::string msgInfected{ "caught {}" };
		std::string msgRecovered{ "recovered from {}" };
	};
	Config cfg;

	template <class... A>
	void Dbg(std::format_string<A...> f, A&&... a)
	{
		if (cfg.verbose) logger::info("{}", std::format(f, std::forward<A>(a)...));
	}

	std::vector<std::string> ParseList(const std::string& s)
	{
		std::vector<std::string> out;
		size_t i = 0;
		while (i <= s.size()) {
			auto j = s.find(',', i);
			if (j == std::string::npos) j = s.size();
			const auto t = s.substr(i, j - i);
			const auto b = t.find_first_not_of(" \t");
			if (b != std::string::npos) out.push_back(t.substr(b, t.find_last_not_of(" \t") - b + 1));
			i = j + 1;
		}
		return out;
	}

	void LoadConfig()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile("Data/SKSE/Plugins/RNDFollowers.ini") < 0) {
			logger::warn("RNDFollowers.ini not found, using defaults");
			return;
		}
		auto F = [&](const char* s, const char* k, float d) { return static_cast<float>(ini.GetDoubleValue(s, k, d)); };
		auto B = [&](const char* s, const char* k, bool d) { return ini.GetBoolValue(s, k, d); };
		cfg.includeAnimals = B("General", "IncludeAnimals", cfg.includeAnimals);
		cfg.notify = B("General", "Notifications", cfg.notify);
		cfg.notifyStage = (int)ini.GetLongValue("General", "NotifyFromStage", cfg.notifyStage);
		cfg.tickMs = (int)ini.GetLongValue("General", "TickMs", cfg.tickMs);
		cfg.uninstall = B("General", "Uninstall", cfg.uninstall);
		cfg.respectRndState = B("General", "OnlyWhenRNDActive", cfg.respectRndState);
		cfg.verbose = B("Log", "Verbose", cfg.verbose);
		cfg.summarySeconds = (int)ini.GetLongValue("Log", "SummarySeconds", cfg.summarySeconds);
		cfg.fullStomachSlow = B("General", "ApplyFullStomachSlow", cfg.fullStomachSlow);
		cfg.startNeed[0] = F("Start", "Hunger", cfg.startNeed[0]);
		cfg.startNeed[1] = F("Start", "Thirst", cfg.startNeed[1]);
		cfg.startNeed[2] = F("Start", "Fatigue", cfg.startNeed[2]);
		cfg.sleepRadius = F("Sleep", "FollowerSleepRadius", cfg.sleepRadius);
		cfg.restPerHour = F("Sleep", "RestPointsPerHour", cfg.restPerHour);
		cfg.sleepMult[0] = F("Sleep", "SleepHungerMult", cfg.sleepMult[0]);
		cfg.sleepMult[1] = F("Sleep", "SleepThirstMult", cfg.sleepMult[1]);
		cfg.combatFatigueMult = F("Sleep", "CombatFatigueMult", cfg.combatFatigueMult);
		cfg.hitDiseaseChance = F("Disease", "ChancePerHitPct", cfg.hitDiseaseChance);
		cfg.stageHours = F("Disease", "HoursPerStage", cfg.stageHours);
		cfg.diseaseAddsFatigue = B("Disease", "DiseaseAddsFatigue", cfg.diseaseAddsFatigue);
		if (auto* p = ini.GetValue("Disease", "SourceRaceKeywords")) { auto l = ParseList(p); if (!l.empty()) cfg.diseaseSources = l; }
		auto M = [&](const char* k, std::string& dst) { if (auto* p = ini.GetValue("Messages", k)) dst = p; };
		for (int i = 1; i < 6; ++i) M(std::format("Hunger{}", i).c_str(), cfg.msgHunger[i]);
		for (int i = 1; i < 5; ++i) M(std::format("Thirst{}", i).c_str(), cfg.msgThirst[i]);
		for (int i = 1; i < 5; ++i) M(std::format("Fatigue{}", i).c_str(), cfg.msgFatigue[i]);
		M("Infected", cfg.msgInfected);
		M("Recovered", cfg.msgRecovered);
	}

	// ------------------------------------------------------- RND form lookups
	struct Rules
	{
		RE::TESGlobal *state{}, *rate[3]{}, *hLvl[6]{}, *tLvl[5]{}, *sLvl[5]{};
		RE::TESGlobal *chRaw{}, *chStale{}, *chRiver{}, *disFat[3]{}, *disFatigue{};
		std::unordered_map<RE::FormID, RE::TESGlobal*> foodFx, waterFx;
		std::unordered_set<RE::FormID> cureFx;
		RE::FormID rawFx{}, staleFx{}, riverFx{};
		RE::SpellItem *hunger[6]{}, *thirst[5]{}, *sleep[5]{}, *disease[6][3]{};
		bool ready{ false };
	};
	Rules R;

	template <class T>
	T* Rnd(RE::FormID id) { return RE::TESDataHandler::GetSingleton()->LookupForm<T>(id, kRND); }

	void ResolveForms()
	{
		auto G = [](RE::FormID id) { return Rnd<RE::TESGlobal>(id); };
		auto Fx = [](RE::FormID id) -> RE::FormID { auto* m = Rnd<RE::EffectSetting>(id); return m ? m->GetFormID() : 0; };

		R.state = G(0x012C4C);
		R.rate[kHunger] = G(0x002DE7);
		R.rate[kThirst] = G(0x002E08);
		R.rate[kFatigue] = G(0x00BAC3);
		const RE::FormID hl[6]{ 0x002DE9, 0x002DEA, 0x002DEB, 0x002DEC, 0x002DED, 0x002DEE };
		const RE::FormID tl[5]{ 0x002E0A, 0x002E0B, 0x002E0C, 0x002E0D, 0x002E0E };
		const RE::FormID sl[5]{ 0x02B5B3, 0x00BAC5, 0x00BAC6, 0x00BAC7, 0x00BAC8 };
		for (int i = 0; i < 6; ++i) R.hLvl[i] = G(hl[i]);
		for (int i = 0; i < 5; ++i) { R.tLvl[i] = G(tl[i]); R.sLvl[i] = G(sl[i]); }
		R.chRaw = G(0x0069AC);
		R.chStale = G(0x007F67);
		R.chRiver = G(0x071690);
		R.disFat[0] = G(0x010BE6);
		R.disFat[1] = G(0x010BE8);
		R.disFat[2] = G(0x010BE7);
		R.disFatigue = G(0x05AD54);

		// food / water effects -> global holding their points (from the RND_EatFood/RND_DrinkWater properties)
		R.foodFx[Fx(0x003375)] = G(0x006F1B);  // Snack
		R.foodFx[Fx(0x004919)] = G(0x006F1C);  // Medium
		R.foodFx[Fx(0x004918)] = G(0x006F1D);  // Filling
		R.foodFx[Fx(0x003E4D)] = G(0x006F1E);  // Candy
		R.foodFx[Fx(0x01114D)] = G(0x01114C);  // Fruit
		R.waterFx[Fx(0x00FB9F)] = G(0x006F16); // DrinkWater
		R.waterFx[Fx(0x006437)] = G(0x01114E); // Bottle01 (low)
		R.waterFx[Fx(0x01114F)] = G(0x006F1A); // Bottle02 (mid)
		R.waterFx[Fx(0x011150)] = G(0x01010E); // Bottle03 (high)
		R.waterFx[Fx(0x003374)] = G(0x006F19); // Fruit
		R.foodFx.erase(0);
		R.waterFx.erase(0);

		R.rawFx = Fx(0x00337B);
		R.staleFx = Fx(0x007F66);
		R.riverFx = Fx(0x07168F);
		if (auto id = Fx(0x01D8C7)) R.cureFx.insert(id);  // RND_CureDiseaseEffect (shrines)
		for (auto f : { "Skyrim.esm"sv, "Update.esm"sv, "Dawnguard.esm"sv, "Dragonborn.esm"sv })
			if (auto* m = RE::TESDataHandler::GetSingleton()->LookupForm<RE::EffectSetting>(0x0AE722, f)) { R.cureFx.insert(m->GetFormID()); break; }

		// ability clones from RNDFollowers.esp (indexed by RNDFollowers_Forms.ini)
		CSimpleIniA ini;
		ini.SetUnicode();
		std::unordered_map<std::string, RE::SpellItem*> byName;
		if (ini.LoadFile("Data/SKSE/Plugins/RNDFollowers_Forms.ini") >= 0) {
			CSimpleIniA::TNamesDepend keys;
			ini.GetAllKeys("Forms", keys);
			for (auto& k : keys) {
				const std::string name = k.pItem;
				if (name.rfind("RNDF_", 0) != 0) continue;
				const auto id = (RE::FormID)std::strtoul(ini.GetValue("Forms", k.pItem, "0"), nullptr, 16) & 0x00FFFFFF;
				if (auto* sp = RE::TESDataHandler::GetSingleton()->LookupForm<RE::SpellItem>(id, kOur)) byName[name] = sp;
			}
		} else {
			logger::error("RNDFollowers_Forms.ini missing - no abilities will be applied");
		}
		auto S = [&](const std::string& n) -> RE::SpellItem* { auto it = byName.find(n); return it == byName.end() ? nullptr : it->second; };
		for (int i = 0; i < 6; ++i) R.hunger[i] = S(std::format("RNDF_HungerSpell{:02}", i));
		for (int i = 0; i < 5; ++i) R.thirst[i] = S(std::format("RNDF_ThirstSpell{:02}", i));
		for (int i = 2; i < 5; ++i) R.sleep[i] = S(std::format("RNDF_SleepSpell{:02}", i));
		for (int d = 0; d < 6; ++d)
			for (int s = 0; s < 3; ++s) R.disease[d][s] = S(std::format("RNDF_Disease{}Stage{}", kDiseaseNames[d], s));

		R.ready = R.state && R.rate[0] && R.rate[1] && R.rate[2] && R.hLvl[5] && R.tLvl[4] && R.sLvl[4];
		logger::info("RND forms resolved: ready={}, abilities found={}", R.ready, byName.size());
	}

	// ------------------------------------------------------------------- state
	struct Record
	{
		float need[3]{ 0, 0, 0 };
		std::int32_t disease{ -1 };   // index into kDiseaseNames, -1 = healthy
		std::int32_t dStage{ -1 };    // highest active stage spell 0..2
		std::int32_t counter{ 0 };    // number of stage timers elapsed (RND logic)
		float timer{ 0 };             // hours since last stage tick
		int  lastStage[3]{ 0, 0, 0 };
		bool sleptThisRest{ false };
	};

	std::recursive_mutex mtx;
	std::unordered_map<RE::FormID, Record> state;
	float lastHours = -1.0f;
	bool playerSleeping = false;
	std::mt19937 rng{ std::random_device{}() };

	float Val(RE::TESGlobal* g, float def = 0.0f) { return g ? g->value : def; }
	bool  Roll(float pct) { return std::uniform_real_distribution<float>(0.0f, 100.0f)(rng) < pct; }
	int   RandInt(int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); }

	template <size_t N>
	int StageOf(float v, RE::TESGlobal* const (&lv)[N])
	{
		int s = 0;
		for (size_t i = 1; i < N; ++i)
			if (lv[i] && v >= lv[i]->value) s = (int)i;
		return s;
	}

	bool RaceHas(RE::Actor* a, const std::string& kw)
	{
		auto* race = a ? a->GetRace() : nullptr;
		return race && race->HasKeywordString(kw);
	}

	bool IsFollower(RE::Actor* a)
	{
		if (!a || a->IsDead() || a->IsPlayerRef() || !a->IsPlayerTeammate() || a->IsCommandedActor()) return false;
		if (!cfg.includeAnimals && RaceHas(a, "ActorTypeAnimal")) return false;
		return true;
	}

	void Say(RE::Actor* a, const std::string& what)
	{
		if (!cfg.notify || what.empty()) return;
		const auto msg = std::format("{} {}", a->GetDisplayFullName(), what);
		RE::DebugNotification(msg.c_str());
	}

	// ------------------------------------------------------------- abilities
	void SyncGroup(RE::Actor* a, RE::SpellItem* want, RE::SpellItem* const* all, size_t n)
	{
		for (size_t i = 0; i < n; ++i) {
			auto* s = all[i];
			if (s && s != want && a->HasSpell(s)) {
				a->RemoveSpell(s);
				Dbg("{}: removed ability {:08X} (still present: {})", a->GetDisplayFullName(), s->GetFormID(), a->HasSpell(s));
			}
		}
		if (want && !a->HasSpell(want)) {
			a->AddSpell(want);
			Dbg("{}: added ability {:08X} (present now: {})", a->GetDisplayFullName(), want->GetFormID(), a->HasSpell(want));
		}
	}

	void StripAll(RE::Actor* a)
	{
		SyncGroup(a, nullptr, R.hunger, 6);
		SyncGroup(a, nullptr, R.thirst, 5);
		SyncGroup(a, nullptr, R.sleep, 5);
		for (auto& d : R.disease) SyncGroup(a, nullptr, d, 3);
	}

	void ApplyAbilities(RE::Actor* a, const Record& r)
	{
		const int hs = StageOf(r.need[kHunger], R.hLvl);
		const int ts = StageOf(r.need[kThirst], R.tLvl);
		const int fs = StageOf(r.need[kFatigue], R.sLvl);
		SyncGroup(a, (hs == 0 && !cfg.fullStomachSlow) ? nullptr : R.hunger[hs], R.hunger, 6);
		SyncGroup(a, R.thirst[ts], R.thirst, 5);
		SyncGroup(a, R.sleep[fs], R.sleep, 5);  // stages 0/1 have no ability
		for (int d = 0; d < 6; ++d)
			for (int s = 0; s < 3; ++s) {
				auto* sp = R.disease[d][s];
				if (!sp) continue;
				const bool want = (d == r.disease) && (s <= r.dStage);
				const bool has = a->HasSpell(sp);
				if (want && !has) {
					a->AddSpell(sp);
					Dbg("{}: added disease ability {:08X} (present now: {})", a->GetDisplayFullName(), sp->GetFormID(), a->HasSpell(sp));
				} else if (!want && has) {
					a->RemoveSpell(sp);
					Dbg("{}: removed disease ability {:08X}", a->GetDisplayFullName(), sp->GetFormID());
				}
			}
	}

	void Infect(RE::Actor* a, Record& r, const char* cause)
	{
		if (r.disease >= 0) return;
		r.disease = RandInt(0, 5);
		r.dStage = 0;
		r.counter = 0;
		r.timer = 0;
		const char* nm = kDiseaseNames[r.disease];
		Dbg("{}: infected with {} ({})", a->GetDisplayFullName(), nm, cause);
		Say(a, std::vformat(cfg.msgInfected, std::make_format_args(nm)) + " (" + cause + ")");
	}

	void Cure(RE::Actor* a, Record& r)
	{
		if (r.disease < 0) return;
		const char* nm = kDiseaseNames[r.disease];
		Dbg("{}: recovered from {}", a->GetDisplayFullName(), nm);
		Say(a, std::vformat(cfg.msgRecovered, std::make_format_args(nm)));
		r.disease = -1;
		r.dStage = -1;
		r.counter = 0;
		r.timer = 0;
	}

	const char* WhyNot(RE::Actor* a)
	{
		if (!a) return "null";
		if (a->IsDead()) return "dead";
		if (a->IsPlayerRef()) return "is the player";
		if (!a->IsPlayerTeammate()) return "not a player teammate (dismissed or not a follower)";
		if (a->IsCommandedActor()) return "commanded/summoned actor";
		if (!cfg.includeAnimals && RaceHas(a, "ActorTypeAnimal")) return "animal (IncludeAnimals=false)";
		return "uninstall mode";
	}

	void LogSummary(RE::Actor* a, const Record& r)
	{
		std::string ab;
		auto add = [&](RE::SpellItem* sp, const std::string& label) { if (sp && a->HasSpell(sp)) ab += label + " "; };
		for (int i = 0; i < 6; ++i) add(R.hunger[i], std::format("Hunger{}", i));
		for (int i = 0; i < 5; ++i) add(R.thirst[i], std::format("Thirst{}", i));
		for (int i = 0; i < 5; ++i) add(R.sleep[i], std::format("Fatigue{}", i));
		for (int d = 0; d < 6; ++d)
			for (int k = 0; k < 3; ++k) add(R.disease[d][k], std::format("{}{}", kDiseaseNames[d], k));
		if (ab.empty()) ab = "(none)";
		Dbg("[summary] {} {:08X} | hunger {:.0f} (stage {}) | thirst {:.0f} (stage {}) | fatigue {:.0f} (stage {}) | disease {} (stage {}) | abilities: {}",
			a->GetDisplayFullName(), a->GetFormID(),
			r.need[kHunger], StageOf(r.need[kHunger], R.hLvl), r.need[kThirst], StageOf(r.need[kThirst], R.tLvl),
			r.need[kFatigue], StageOf(r.need[kFatigue], R.sLvl),
			r.disease >= 0 ? kDiseaseNames[r.disease] : "none", r.dStage, ab);
	}

	// -------------------------------------------------------------------- tick
	void Tick()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cal = RE::Calendar::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!player || !cal || !lists || !player->Is3DLoaded() || !R.ready) return;

		const float now = cal->GetCurrentGameTime() * 24.0f;
		std::lock_guard lk(mtx);
		if (lastHours < 0.0f || now < lastHours) { lastHours = now; return; }
		float dt = std::min(now - lastHours, 72.0f);
		lastHours = now;

		const bool rndOff = cfg.respectRndState && Val(R.state, 1.0f) < 0.5f;
		if (rndOff) dt = 0.0f;

		const float caps[3]{ Val(R.hLvl[5], 240), Val(R.tLvl[4], 240), Val(R.sLvl[4], 240) };

		static auto lastSummary = std::chrono::steady_clock::now();
		static std::unordered_set<RE::FormID> skipLogged;
		bool doSummary = false;
		if (cfg.verbose && cfg.summarySeconds > 0 &&
		    std::chrono::steady_clock::now() - lastSummary >= std::chrono::seconds(cfg.summarySeconds)) {
			doSummary = true;
			lastSummary = std::chrono::steady_clock::now();
			Dbg("[summary] RND_State={} tracked followers={}", Val(R.state, -1.0f), state.size());
		}

		for (auto& handle : lists->highActorHandles) {
			auto ptr = handle.get();
			auto* a = ptr.get();
			if (!a) continue;
			const auto id = a->GetFormID();
			auto it = state.find(id);

			if (cfg.uninstall || !IsFollower(a)) {
				if (it != state.end()) {
					Dbg("stop tracking {} {:08X}: {}", a->GetDisplayFullName(), id, WhyNot(a));
					StripAll(a);
					state.erase(it);
				} else if (a->IsPlayerTeammate() && !a->IsPlayerRef() && skipLogged.insert(id).second) {
					Dbg("teammate {} {:08X} is not tracked: {}", a->GetDisplayFullName(), id, WhyNot(a));
				}
				continue;
			}
			if (it == state.end()) {
				Record fresh;
				for (int n = 0; n < 3; ++n) fresh.need[n] = cfg.startNeed[n];
				it = state.emplace(id, fresh).first;
				skipLogged.erase(id);
				Dbg("start tracking follower {} {:08X} (start hunger {:.0f}, thirst {:.0f}, fatigue {:.0f})", a->GetDisplayFullName(), id,
					fresh.need[0], fresh.need[1], fresh.need[2]);
			}
			auto& r = it->second;

			const bool bedSleep = a->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
			const bool isNear = a->GetParentCell() == player->GetParentCell() &&
			                  player->GetPosition().GetDistance(a->GetPosition()) <= cfg.sleepRadius;
			const bool sleeping = bedSleep || (playerSleeping && isNear);
			if (sleeping) r.sleptThisRest = true;

			if (dt > 0.0f) {
				r.need[kHunger] += dt * Val(R.rate[kHunger]) * (sleeping ? cfg.sleepMult[0] : 1.0f);
				r.need[kThirst] += dt * Val(R.rate[kThirst]) * (sleeping ? cfg.sleepMult[1] : 1.0f);
				if (sleeping) {
					r.need[kFatigue] -= dt * cfg.restPerHour;
				} else {
					r.need[kFatigue] += dt * Val(R.rate[kFatigue]) * (a->IsInCombat() ? cfg.combatFatigueMult : 1.0f);
					if (cfg.diseaseAddsFatigue && r.disease >= 0 && r.dStage >= 0)
						r.need[kFatigue] += dt * Val(R.disFat[std::clamp(r.dStage, 0, 2)]);
				}
				for (int n = 0; n < 3; ++n) r.need[n] = std::clamp(r.need[n], 0.0f, caps[n]);

				if (r.disease >= 0) {
					r.timer += dt;
					while (r.timer >= cfg.stageHours && cfg.stageHours > 0.0f) {
						r.timer -= cfg.stageHours;
						++r.counter;
						if (r.counter <= 2 && r.dStage < r.counter) r.dStage = r.counter;
					}
				}
			}

			const int st[3]{ StageOf(r.need[kHunger], R.hLvl), StageOf(r.need[kThirst], R.tLvl), StageOf(r.need[kFatigue], R.sLvl) };
			for (int n = 0; n < 3; ++n) {
				if (st[n] > r.lastStage[n] && st[n] >= cfg.notifyStage) {
					const std::string& m = n == kHunger ? cfg.msgHunger[st[n]] : n == kThirst ? cfg.msgThirst[st[n]] : cfg.msgFatigue[st[n]];
					Say(a, m);
				}
				r.lastStage[n] = st[n];
			}

			// neglect: severe starvation/dehydration/exhaustion can make followers sick
			if (dt > 0.0f && r.disease < 0 && (st[0] >= 4 || st[1] >= 4 || st[2] >= 4)) {
				const float p = 100.0f * (1.0f - std::pow(1.0f - 0.02f, dt));
				if (Roll(p)) Infect(a, r, "weakened by neglect");
			}

			ApplyAbilities(a, r);
			if (doSummary) LogSummary(a, r);
		}
	}

	// Called when the player finishes sleeping: followers who rested may shake off a disease (RND rules).
	void SleepCure()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!player || !lists) return;
		int bonus = 0;
		if (auto* loc = player->GetCurrentLocation()) {
			if (loc->HasKeywordString("LocTypePlayerHouse")) bonus += 3;
			else if (loc->HasKeywordString("LocTypeInn")) bonus += 2;
			else if (loc->HasKeywordString("LocTypeBanditCamp") || loc->HasKeywordString("LocTypeAnimalDen")) bonus -= 1;
			else if (loc->HasKeywordString("LocTypeDungeon")) bonus -= 3;
		}
		std::lock_guard lk(mtx);
		for (auto& handle : lists->highActorHandles) {
			auto ptr = handle.get();
			auto* a = ptr.get();
			if (!a) continue;
			auto it = state.find(a->GetFormID());
			if (it == state.end()) continue;
			auto& r = it->second;
			if (r.disease >= 0 && r.sleptThisRest && r.counter > 3) {
				const int adjust = (r.counter - 3) + bonus;
				const bool better = RandInt(0, 9) + adjust > (int)Val(R.disFatigue, 1.0f);
				Dbg("{} slept with {} (counter {}, adjust {}): {}", a->GetDisplayFullName(), kDiseaseNames[r.disease], r.counter, adjust,
					better ? "improves" : "no change");
				if (better && --r.dStage < 0) Cure(a, r);
			}
			r.sleptThisRest = false;
		}
	}

	// ------------------------------------------------------------------ events
	struct Sinks :
		RE::BSTEventSink<RE::TESEquipEvent>,
		RE::BSTEventSink<RE::TESHitEvent>,
		RE::BSTEventSink<RE::TESSleepStartEvent>,
		RE::BSTEventSink<RE::TESSleepStopEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* e, RE::BSTEventSource<RE::TESEquipEvent>*) override
		{
			using RE::BSEventNotifyControl;
			if (!e || !e->equipped || !e->actor || !R.ready) return BSEventNotifyControl::kContinue;
			auto* a = e->actor->As<RE::Actor>();
			if (!a) return BSEventNotifyControl::kContinue;

			auto* alch = RE::TESForm::LookupByID<RE::AlchemyItem>(e->baseObject);
			if (!alch || alch->IsPoison()) return BSEventNotifyControl::kContinue;

			std::lock_guard lk(mtx);
			auto it = state.find(a->GetFormID());
			if (it == state.end()) {
				if (cfg.verbose && a->IsPlayerTeammate())
					Dbg("consume event by UNTRACKED teammate {} {:08X}: {} ({:08X})", a->GetDisplayFullName(), a->GetFormID(),
						alch->GetName(), e->baseObject);
				return BSEventNotifyControl::kContinue;
			}

			// ignore duplicate equip events fired for the same item within half a second
			static std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point> last;
			const auto key = (std::uint64_t(a->GetFormID()) << 32) | e->baseObject;
			const auto t = std::chrono::steady_clock::now();
			if (auto l = last.find(key); l != last.end() && t - l->second < 500ms) return BSEventNotifyControl::kContinue;
			last[key] = t;

			auto& r = it->second;
			float food = 0, water = 0;
			bool eats = false, drinks = false, raw = false, stale = false, river = false, cure = false;
			std::string fxList;
			const float h0 = r.need[kHunger], t0 = r.need[kThirst];
			for (auto* eff : alch->effects) {
				if (!eff || !eff->baseEffect) continue;
				const auto fx = eff->baseEffect->GetFormID();
				if (cfg.verbose) fxList += std::format("{:08X} ", fx);
				if (auto f = R.foodFx.find(fx); f != R.foodFx.end()) { food += Val(f->second); eats = true; }
				else if (auto w = R.waterFx.find(fx); w != R.waterFx.end()) { water += Val(w->second); drinks = true; }
				else if (fx == R.rawFx) raw = true;
				else if (fx == R.staleFx) stale = true;
				else if (fx == R.riverFx) river = true;
				else if (R.cureFx.contains(fx)) cure = true;
			}
			if (eats) r.need[kHunger] = std::clamp(r.need[kHunger] - food - RandInt(-5, 5), 0.0f, Val(R.hLvl[5], 240));
			if (drinks) r.need[kThirst] = std::clamp(r.need[kThirst] - water - RandInt(-5, 5), 0.0f, Val(R.tLvl[4], 240));
			if (cure) Cure(a, r);
			if (raw && Roll(Val(R.chRaw))) Infect(a, r, "raw food");
			if (stale && Roll(Val(R.chStale))) Infect(a, r, "stale food");
			if (river && Roll(Val(R.chRiver))) Infect(a, r, "river water");
			if (eats || drinks || cure || raw || stale || river)
				Dbg("{} consumed {} ({:08X}): food {:.0f}, water {:.0f}, cure {}, raw {}, stale {}, river {} | hunger {:.0f} -> {:.0f}, thirst {:.0f} -> {:.0f}",
					a->GetDisplayFullName(), alch->GetName(), e->baseObject, food, water, cure, raw, stale, river,
					h0, r.need[kHunger], t0, r.need[kThirst]);
			else
				Dbg("{} used {} ({:08X}) - no RND food/drink effect recognised; its effects: {}", a->GetDisplayFullName(),
					alch->GetName(), e->baseObject, fxList.empty() ? "(none)" : fxList);
			return BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			using RE::BSEventNotifyControl;
			if (!e || !e->target || !e->cause || !R.ready) return BSEventNotifyControl::kContinue;
			auto* victim = e->target->As<RE::Actor>();
			auto* attacker = e->cause->As<RE::Actor>();
			if (!victim || !attacker || attacker->IsPlayerRef() || attacker->IsPlayerTeammate()) return BSEventNotifyControl::kContinue;

			std::lock_guard lk(mtx);
			auto it = state.find(victim->GetFormID());
			if (it == state.end() || it->second.disease >= 0) return BSEventNotifyControl::kContinue;
			bool source = false;
			for (auto& kw : cfg.diseaseSources) if (RaceHas(attacker, kw)) { source = true; break; }
			if (!source) return BSEventNotifyControl::kContinue;
			const float resist = std::clamp(victim->AsActorValueOwner()->GetActorValue(RE::ActorValue::kResistDisease), 0.0f, 100.0f);
			Dbg("{} was hit by disease source {}, resist {:.0f}", victim->GetDisplayFullName(), attacker->GetDisplayFullName(), resist);
			if (Roll(cfg.hitDiseaseChance * (1.0f - resist / 100.0f))) Infect(victim, it->second, "wounded");
			return BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStartEvent*, RE::BSTEventSource<RE::TESSleepStartEvent>*) override
		{
			std::lock_guard lk(mtx);
			playerSleeping = true;
			Dbg("player started sleeping");
			return RE::BSEventNotifyControl::kContinue;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESSleepStopEvent*, RE::BSTEventSource<RE::TESSleepStopEvent>*) override
		{
			SKSE::GetTaskInterface()->AddTask([] {
				Dbg("player stopped sleeping");
				Tick();           // credit the slept hours first
				SleepCure();
				std::lock_guard lk(mtx);
				playerSleeping = false;
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	Sinks sinks;

	// ----------------------------------------------------------------- co-save
	constexpr std::uint32_t kUID = 'RNDF', kRec = 'STAT', kVer = 2;

	void OnSave(SKSE::SerializationInterface* s)
	{
		std::lock_guard lk(mtx);
		if (!s->OpenRecord(kRec, kVer)) return;
		s->WriteRecordData(lastHours);
		const std::uint32_t n = (std::uint32_t)state.size();
		s->WriteRecordData(n);
		for (auto& [id, r] : state) {
			s->WriteRecordData(id);
			s->WriteRecordData(r.need);
			s->WriteRecordData(r.disease);
			s->WriteRecordData(r.dStage);
			s->WriteRecordData(r.counter);
			s->WriteRecordData(r.timer);
		}
	}

	void OnLoad(SKSE::SerializationInterface* s)
	{
		std::lock_guard lk(mtx);
		state.clear();
		lastHours = -1.0f;
		playerSleeping = false;
		std::uint32_t type, version, length;
		while (s->GetNextRecordInfo(type, version, length)) {
			if (type != kRec || version != kVer) continue;
			std::uint32_t n = 0;
			s->ReadRecordData(lastHours);
			s->ReadRecordData(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				RE::FormID oldId, newId;
				Record r;
				s->ReadRecordData(oldId);
				s->ReadRecordData(r.need);
				s->ReadRecordData(r.disease);
				s->ReadRecordData(r.dStage);
				s->ReadRecordData(r.counter);
				s->ReadRecordData(r.timer);
				if (s->ResolveFormID(oldId, newId)) {
					r.lastStage[0] = StageOf(r.need[0], R.hLvl);
					r.lastStage[1] = StageOf(r.need[1], R.tLvl);
					r.lastStage[2] = StageOf(r.need[2], R.sLvl);
					state[newId] = r;
				}
			}
		}
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		std::lock_guard lk(mtx);
		state.clear();
		lastHours = -1.0f;
		playerSleeping = false;
	}

	// ----------------------------------------------------------------- startup
	void SetupLog()
	{
		auto path = logger::log_directory();
		if (!path) return;
		*path /= "RNDFollowers.log"sv;
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S] [%l] %v");
	}

	void OnMessage(SKSE::MessagingInterface::Message* m)
	{
		switch (m->type) {
		case SKSE::MessagingInterface::kDataLoaded: {
			LoadConfig();
			ResolveForms();
			logger::info("settings: verbose={}, summary every {}s, notify from stage {}, uninstall={}", cfg.verbose, cfg.summarySeconds,
				cfg.notifyStage, cfg.uninstall);
			logger::info("RND: state={} rates H/T/F={}/{}/{} per hour, hunger stage thresholds={}/{}/{}/{}/{}",
				Val(R.state, -1.0f), Val(R.rate[0]), Val(R.rate[1]), Val(R.rate[2]), Val(R.hLvl[1]), Val(R.hLvl[2]), Val(R.hLvl[3]),
				Val(R.hLvl[4]), Val(R.hLvl[5]));
			logger::info("RND food effects known: {}, water effects known: {}, cure effects known: {}", R.foodFx.size(), R.waterFx.size(),
				R.cureFx.size());
			auto* src = RE::ScriptEventSourceHolder::GetSingleton();
			src->AddEventSink<RE::TESEquipEvent>(&sinks);
			src->AddEventSink<RE::TESHitEvent>(&sinks);
			src->AddEventSink<RE::TESSleepStartEvent>(&sinks);
			src->AddEventSink<RE::TESSleepStopEvent>(&sinks);
			std::thread([] {
				while (true) {
					std::this_thread::sleep_for(std::chrono::milliseconds(std::max(250, cfg.tickMs)));
					SKSE::GetTaskInterface()->AddTask([] { Tick(); });
				}
			}).detach();
			logger::info("RNDFollowers started (uninstall mode: {})", cfg.uninstall);
			break;
		}
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame: {
			std::lock_guard lk(mtx);
			playerSleeping = false;
			if (m->type == SKSE::MessagingInterface::kNewGame) { state.clear(); lastHours = -1.0f; }
			break;
		}
		default: break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
	SKSE::Init(skse);
	SetupLog();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	auto* ser = SKSE::GetSerializationInterface();
	ser->SetUniqueID(kUID);
	ser->SetSaveCallback(OnSave);
	ser->SetLoadCallback(OnLoad);
	ser->SetRevertCallback(OnRevert);
	return true;
}
