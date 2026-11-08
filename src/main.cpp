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
#include <vector>
#include <atomic>
#include <cfloat>
#include <cstdarg>
#include <filesystem>

// Third-party header (SKSE Menu Framework): compile it without warnings so it cannot break the build
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

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
		// --- editable from the in-game menu (stored in RNDFollowers_User.ini)
		float rateMult[3]{ 1.0f, 1.0f, 1.0f };   // multiplier on RND hunger / thirst / fatigue rates
		bool  fastTravelSlow{ false };           // slow needs down during fast travel
		float fastTravelMult{ 0.5f };            // share of travel time that counts (0..1)
		bool  diseaseEnabled{ true };            // allow NEW infections
		bool  infectRaw{ true }, infectStale{ true }, infectRiver{ true }, infectHit{ true }, infectNeglect{ true };
		float diseaseChanceMult{ 1.0f };
		bool  uiChinese{ true };
		// --- auto-eat: followers eat / drink from their own inventory
		bool  autoEat{ true }, autoDrink{ true }, autoCure{ true };
		int   autoEatStage{ 3 }, autoDrinkStage{ 2 };
		bool  autoEatRaw{ false }, autoEatSpoiled{ false }, autoEatRiver{ false };
		float autoEatCooldown{ 0.25f };   // game hours between meals
		std::string msgAte{ "ate {}" }, msgDrank{ "drank {}" }, msgCured{ "used {}" };
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

	void LoadMainConfig()
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
		M("AutoAte", cfg.msgAte);
		M("AutoDrank", cfg.msgDrank);
		M("AutoCured", cfg.msgCured);
		cfg.autoCure = B("AutoEat", "CureWhenSick", cfg.autoCure);
		cfg.autoEat = B("AutoEat", "Enabled", cfg.autoEat);
		cfg.autoDrink = B("AutoEat", "Drink", cfg.autoDrink);
		cfg.autoEatStage = std::clamp((int)ini.GetLongValue("AutoEat", "HungerStage", cfg.autoEatStage), 1, 5);
		cfg.autoDrinkStage = std::clamp((int)ini.GetLongValue("AutoEat", "ThirstStage", cfg.autoDrinkStage), 1, 4);
		cfg.autoEatRaw = B("AutoEat", "AllowRaw", cfg.autoEatRaw);
		cfg.autoEatSpoiled = B("AutoEat", "AllowSpoiled", cfg.autoEatSpoiled);
		cfg.autoEatRiver = B("AutoEat", "AllowRiverWater", cfg.autoEatRiver);
		cfg.autoEatCooldown = std::clamp(F("AutoEat", "CooldownHours", cfg.autoEatCooldown), 0.0f, 24.0f);
	}

	// ------------------------------------------- user settings (edited by the in-game menu)
	// Menu changes are stored in their own file so the hand-edited RNDFollowers.ini is never rewritten.
	constexpr const char* kUserIni = "Data/SKSE/Plugins/RNDFollowers_User.ini";
	std::mutex cfgMtx;
	std::atomic<bool> cfgDirty{ false };
	std::atomic<std::int64_t> cfgDirtyAt{ 0 };

	std::int64_t NowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
	void MarkDirty()
	{
		cfgDirtyAt = NowTicks();
		cfgDirty = true;
	}

	void LoadUserConfig()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		if (ini.LoadFile(kUserIni) < 0) return;
		auto F = [&](const char* k, float d) { return static_cast<float>(ini.GetDoubleValue("User", k, d)); };
		auto B = [&](const char* k, bool d) { return ini.GetBoolValue("User", k, d); };
		cfg.rateMult[0] = std::clamp(F("RateHunger", cfg.rateMult[0]), 0.0f, 10.0f);
		cfg.rateMult[1] = std::clamp(F("RateThirst", cfg.rateMult[1]), 0.0f, 10.0f);
		cfg.rateMult[2] = std::clamp(F("RateFatigue", cfg.rateMult[2]), 0.0f, 10.0f);
		cfg.fastTravelSlow = B("FastTravelSlow", cfg.fastTravelSlow);
		cfg.fastTravelMult = std::clamp(F("FastTravelMult", cfg.fastTravelMult), 0.0f, 1.0f);
		cfg.diseaseEnabled = B("DiseaseEnabled", cfg.diseaseEnabled);
		cfg.infectRaw = B("InfectRawFood", cfg.infectRaw);
		cfg.infectStale = B("InfectStaleFood", cfg.infectStale);
		cfg.infectRiver = B("InfectRiverWater", cfg.infectRiver);
		cfg.infectHit = B("InfectWounds", cfg.infectHit);
		cfg.infectNeglect = B("InfectNeglect", cfg.infectNeglect);
		cfg.diseaseChanceMult = std::clamp(F("DiseaseChanceMult", cfg.diseaseChanceMult), 0.0f, 10.0f);
		cfg.notify = B("Notifications", cfg.notify);
		cfg.notifyStage = std::clamp((int)ini.GetLongValue("User", "NotifyFromStage", cfg.notifyStage), 1, 5);
		cfg.uiChinese = B("ChineseUI", cfg.uiChinese);
		cfg.autoEat = B("AutoEat", cfg.autoEat);
		cfg.autoDrink = B("AutoDrink", cfg.autoDrink);
		cfg.autoCure = B("AutoCure", cfg.autoCure);
		cfg.autoEatStage = std::clamp((int)ini.GetLongValue("User", "AutoEatStage", cfg.autoEatStage), 1, 5);
		cfg.autoDrinkStage = std::clamp((int)ini.GetLongValue("User", "AutoDrinkStage", cfg.autoDrinkStage), 1, 4);
		cfg.autoEatRaw = B("AutoEatRaw", cfg.autoEatRaw);
		cfg.autoEatSpoiled = B("AutoEatSpoiled", cfg.autoEatSpoiled);
		cfg.autoEatRiver = B("AutoEatRiverWater", cfg.autoEatRiver);
		cfg.autoEatCooldown = std::clamp(F("AutoEatCooldownHours", cfg.autoEatCooldown), 0.0f, 24.0f);
	}

	void SaveUserConfig()
	{
		std::lock_guard lk(cfgMtx);
		cfgDirty = false;
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetDoubleValue("User", "RateHunger", cfg.rateMult[0]);
		ini.SetDoubleValue("User", "RateThirst", cfg.rateMult[1]);
		ini.SetDoubleValue("User", "RateFatigue", cfg.rateMult[2]);
		ini.SetBoolValue("User", "FastTravelSlow", cfg.fastTravelSlow);
		ini.SetDoubleValue("User", "FastTravelMult", cfg.fastTravelMult);
		ini.SetBoolValue("User", "DiseaseEnabled", cfg.diseaseEnabled);
		ini.SetBoolValue("User", "InfectRawFood", cfg.infectRaw);
		ini.SetBoolValue("User", "InfectStaleFood", cfg.infectStale);
		ini.SetBoolValue("User", "InfectRiverWater", cfg.infectRiver);
		ini.SetBoolValue("User", "InfectWounds", cfg.infectHit);
		ini.SetBoolValue("User", "InfectNeglect", cfg.infectNeglect);
		ini.SetDoubleValue("User", "DiseaseChanceMult", cfg.diseaseChanceMult);
		ini.SetBoolValue("User", "Notifications", cfg.notify);
		ini.SetLongValue("User", "NotifyFromStage", cfg.notifyStage);
		ini.SetBoolValue("User", "ChineseUI", cfg.uiChinese);
		ini.SetBoolValue("User", "AutoEat", cfg.autoEat);
		ini.SetBoolValue("User", "AutoDrink", cfg.autoDrink);
		ini.SetBoolValue("User", "AutoCure", cfg.autoCure);
		ini.SetLongValue("User", "AutoEatStage", cfg.autoEatStage);
		ini.SetLongValue("User", "AutoDrinkStage", cfg.autoDrinkStage);
		ini.SetBoolValue("User", "AutoEatRaw", cfg.autoEatRaw);
		ini.SetBoolValue("User", "AutoEatSpoiled", cfg.autoEatSpoiled);
		ini.SetBoolValue("User", "AutoEatRiverWater", cfg.autoEatRiver);
		ini.SetDoubleValue("User", "AutoEatCooldownHours", cfg.autoEatCooldown);
		if (ini.SaveFile(kUserIni) < 0)
			logger::warn("could not write {}", kUserIni);
		else
			Dbg("menu settings saved to {}", kUserIni);
	}

	void LoadConfig()
	{
		LoadMainConfig();
		LoadUserConfig();
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
		float lastAutoEat{ -1000.0f };   // game hours of the last auto-eat attempt (not saved)
		float missedDt{ 0 }, missedNeedDt{ 0 };   // game hours that passed while she was not loaded (not saved)
		bool  noCureLogged{ false };
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
		RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
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

	void Infect(RE::Actor* a, Record& r, const char* cause, bool force = false, int which = -1)
	{
		if (r.disease >= 0) return;
		if (!force && !cfg.diseaseEnabled) return;
		r.disease = (which >= 0 && which <= 5) ? which : RandInt(0, 5);
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

	// ---------------------------------------------------------- food: eating and auto-eat
	struct FoodInfo
	{
		float food{ 0 }, water{ 0 };
		bool  eats{ false }, drinks{ false }, raw{ false }, stale{ false }, river{ false }, cure{ false };
		std::string fx;   // effect ids, only filled when asked (log)
	};

	// What RND would do with this item (uses RND's own effect tables and globals).
	FoodInfo InspectFood(RE::AlchemyItem* alch, bool listFx)
	{
		FoodInfo fi;
		for (auto* eff : alch->effects) {
			if (!eff || !eff->baseEffect) continue;
			const auto fx = eff->baseEffect->GetFormID();
			if (listFx) fi.fx += std::format("{:08X} ", fx);
			if (auto f = R.foodFx.find(fx); f != R.foodFx.end()) { fi.food += Val(f->second); fi.eats = true; }
			else if (auto w = R.waterFx.find(fx); w != R.waterFx.end()) { fi.water += Val(w->second); fi.drinks = true; }
			else if (fx == R.rawFx) fi.raw = true;
			else if (fx == R.staleFx) fi.stale = true;
			else if (fx == R.riverFx) fi.river = true;
			else if (R.cureFx.contains(fx)) fi.cure = true;
		}
		return fi;
	}

	// Applies one consumed item to a tracked follower: needs, cure, infection chances.
	void ApplyConsume(RE::Actor* a, Record& r, RE::AlchemyItem* alch, RE::FormID baseId, const char* how)
	{
		const FoodInfo fi = InspectFood(alch, cfg.verbose);
		const float h0 = r.need[kHunger], t0 = r.need[kThirst];
		if (fi.eats) r.need[kHunger] = std::clamp(r.need[kHunger] - fi.food - RandInt(-5, 5), 0.0f, Val(R.hLvl[5], 240));
		if (fi.drinks) r.need[kThirst] = std::clamp(r.need[kThirst] - fi.water - RandInt(-5, 5), 0.0f, Val(R.tLvl[4], 240));
		if (fi.cure) Cure(a, r);
		if (fi.raw && cfg.infectRaw && Roll(Val(R.chRaw) * cfg.diseaseChanceMult)) Infect(a, r, "raw food");
		if (fi.stale && cfg.infectStale && Roll(Val(R.chStale) * cfg.diseaseChanceMult)) Infect(a, r, "stale food");
		if (fi.river && cfg.infectRiver && Roll(Val(R.chRiver) * cfg.diseaseChanceMult)) Infect(a, r, "river water");
		if (fi.eats || fi.drinks || fi.cure || fi.raw || fi.stale || fi.river)
			Dbg("{} {} {} ({:08X}): food {:.0f}, water {:.0f}, cure {}, raw {}, stale {}, river {} | hunger {:.0f} -> {:.0f}, thirst {:.0f} -> {:.0f}",
				a->GetDisplayFullName(), how, alch->GetName(), baseId, fi.food, fi.water, fi.cure, fi.raw, fi.stale, fi.river, h0, r.need[kHunger], t0,
				r.need[kThirst]);
		else
			Dbg("{} used {} ({:08X}) - no RND food/drink effect recognised; its effects: {}", a->GetDisplayFullName(), alch->GetName(), baseId,
				fi.fx.empty() ? std::string("(none)") : fi.fx);
	}

	std::string FormatMsg(const std::string& fmt, const std::string& arg)
	{
		try {
			return std::vformat(fmt, std::make_format_args(arg));
		} catch (...) {
			return fmt;
		}
	}

	// Takes ONE suitable item out of the follower's inventory and applies it. mode: 0 = food (hunger), 1 = drink (thirst), 2 = cure item.
	// Preference: an item that does not overshoot the need (the biggest such one), otherwise the smallest one.
	bool EatOne(RE::Actor* a, Record& r, int mode)
	{
		const float need = mode == 0 ? r.need[kHunger] : mode == 1 ? r.need[kThirst] : 0.0f;
		auto inv = a->GetInventory([](RE::TESBoundObject& o) { return o.GetFormType() == RE::FormType::AlchemyItem; });
		RE::AlchemyItem* best = nullptr;
		float bestScore = 0.0f;
		for (auto& [obj, entry] : inv) {
			if (!obj || entry.first <= 0) continue;
			auto* alch = obj->As<RE::AlchemyItem>();
			if (!alch || alch->IsPoison()) continue;
			const FoodInfo fi = InspectFood(alch, false);
			const float value = mode == 0 ? fi.food : mode == 1 ? fi.water : (fi.cure ? 1.0f : 0.0f);
			if (value <= 0.0f) continue;
			if ((fi.raw && !cfg.autoEatRaw) || (fi.stale && !cfg.autoEatSpoiled) || (fi.river && !cfg.autoEatRiver)) continue;
			float score = 0.0f;
			if (mode == 2) {
				score = fi.food + fi.water;   // pure cure potions first, cure foods (garlic bread, fish soup) last
			} else {
				const bool mixed = mode == 1 && fi.food > 0.0f;   // keep real meals for hunger
				const bool fits = value <= need + 5.0f;
				score = (mixed ? 10000.0f : 0.0f) + (fits ? -value : 1000.0f + value);
			}
			if (!best || score < bestScore) {
				best = alch;
				bestScore = score;
			}
		}
		if (!best) return false;
		static_cast<void>(a->RemoveItem(best, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr));
		ApplyConsume(a, r, best, best->GetFormID(), mode == 0 ? "auto-ate" : mode == 1 ? "auto-drank" : "auto-cured with");
		const std::string nm = best->GetName();
		Say(a, FormatMsg(mode == 0 ? cfg.msgAte : mode == 1 ? cfg.msgDrank : cfg.msgCured, nm));
		return true;
	}

	// Called every tick for each tracked follower: eat / drink / use a cure item from her own inventory.
	void AutoEat(RE::Actor* a, Record& r, bool sleeping, float nowHours)
	{
		if (r.disease < 0) r.noCureLogged = false;
		if ((!cfg.autoEat && !cfg.autoCure) || sleeping || a->IsInCombat() || a->IsDead()) return;
		if (nowHours < r.lastAutoEat) r.lastAutoEat = -1000.0f;   // game time went backwards (older save loaded)
		if (nowHours - r.lastAutoEat < cfg.autoEatCooldown) return;
		const bool wantFood = cfg.autoEat && StageOf(r.need[kHunger], R.hLvl) >= cfg.autoEatStage;
		const bool wantDrink = cfg.autoEat && cfg.autoDrink && StageOf(r.need[kThirst], R.tLvl) >= cfg.autoDrinkStage;
		const bool wantCure = cfg.autoCure && r.disease >= 0;
		if (!wantFood && !wantDrink && !wantCure) return;
		r.lastAutoEat = nowHours;   // also throttles the inventory scan when she has nothing to use
		for (int i = 0; wantFood && i < 3 && StageOf(r.need[kHunger], R.hLvl) >= cfg.autoEatStage; ++i) {
			if (!EatOne(a, r, 0)) {
				Dbg("{}: hungry (stage {}) but has no suitable food in her inventory", a->GetDisplayFullName(), StageOf(r.need[kHunger], R.hLvl));
				break;
			}
		}
		for (int i = 0; wantDrink && i < 3 && StageOf(r.need[kThirst], R.tLvl) >= cfg.autoDrinkStage; ++i) {
			if (!EatOne(a, r, 1)) {
				Dbg("{}: thirsty (stage {}) but has no suitable drink in her inventory", a->GetDisplayFullName(), StageOf(r.need[kThirst], R.tLvl));
				break;
			}
		}
		if (wantCure && r.disease >= 0 && !EatOne(a, r, 2) && !r.noCureLogged) {
			r.noCureLogged = true;
			Dbg("{}: is sick with {} but has no cure item in her inventory", a->GetDisplayFullName(), kDiseaseNames[std::clamp(r.disease, 0, 5)]);
		}
	}

	// ---------------------------------------------------- live view + actions for the menu
	// Tick() (game thread) publishes a snapshot; the menu (render thread) only reads that snapshot.
	// Menu buttons never touch actors directly: they queue an Action that Tick() runs on the game thread.
	constexpr float kJumpHours = 0.25f;  // one tick that advances at least this much game time = time jump

	struct AbilityView
	{
		int group{ 0 };   // 0 hunger, 1 thirst, 2 fatigue, 3 disease
		int index{ 0 };   // stage (groups 0-2) or disease * 3 + stage (group 3)
		std::vector<std::string> fx;
	};

	struct FollowerView
	{
		RE::FormID id{ 0 };
		std::string name;
		float need[3]{ 0, 0, 0 };
		float cap[3]{ 0, 0, 0 };
		float nextAt[3]{ 0, 0, 0 };
		int   stage[3]{ 0, 0, 0 };
		int   maxStage[3]{ 5, 4, 4 };
		int   disease{ -1 };
		int   dStage{ -1 };
		int   counter{ 0 };
		float timer{ 0 };
		bool  sleeping{ false };
		bool  combat{ false };
		std::vector<AbilityView> abilities;
	};

	struct GlobalView
	{
		bool  ready{ false };
		bool  rndActive{ true };
		float baseRate[3]{ 0, 0, 0 };
		float hLv[6]{ 0, 0, 0, 0, 0, 0 };
		float tLv[5]{ 0, 0, 0, 0, 0 };
		float sLv[5]{ 0, 0, 0, 0, 0 };
		float chance[3]{ 0, 0, 0 };   // raw food, stale food, river water (RND globals, percent)
		float lastJumpHours{ 0 };
		float lastJumpCounted{ 0 };
	};

	std::mutex viewMtx;
	std::vector<FollowerView> views;
	GlobalView gview;

	enum class ActType { SetNeed, ResetNeeds, Infect, Cure, ReloadConfig, EatNow };
	struct Action
	{
		ActType type{ ActType::ReloadConfig };
		RE::FormID id{ 0 };   // 0 = every tracked follower
		int   arg{ 0 };       // need index (SetNeed) or disease index (Infect, -1 = random)
		float value{ 0 };
	};
	std::mutex actMtx;
	std::vector<Action> pending;
	void QueueAction(const Action& a)
	{
		std::lock_guard lk(actMtx);
		pending.push_back(a);
	}

	std::vector<std::string> FxOf(RE::SpellItem* sp)
	{
		static std::unordered_map<RE::FormID, std::vector<std::string>> cache;
		const auto id = sp->GetFormID();
		if (auto it = cache.find(id); it != cache.end()) return it->second;
		std::vector<std::string> out;
		for (auto* eff : sp->effects) {
			if (!eff || !eff->baseEffect) continue;
			std::string n = eff->baseEffect->GetName();
			if (n.empty()) n = std::format("{:08X}", eff->baseEffect->GetFormID());
			const float mag = eff->effectItem.magnitude;
			out.push_back(std::fabs(mag) > 0.001f ? std::format("{} ({:+.0f})", n, mag) : n);
		}
		cache[id] = out;
		return out;
	}

	// abilities that are really on the actor right now (HasSpell), with the effects each one contains
	std::vector<AbilityView> CollectAbilities(RE::Actor* a)
	{
		std::vector<AbilityView> out;
		auto add = [&](RE::SpellItem* sp, int group, int index) {
			if (sp && a->HasSpell(sp)) out.push_back(AbilityView{ group, index, FxOf(sp) });
		};
		for (int i = 0; i < 6; ++i) add(R.hunger[i], 0, i);
		for (int i = 0; i < 5; ++i) add(R.thirst[i], 1, i);
		for (int i = 0; i < 5; ++i) add(R.sleep[i], 2, i);
		for (int d = 0; d < 6; ++d)
			for (int s = 0; s < 3; ++s) add(R.disease[d][s], 3, d * 3 + s);
		return out;
	}

	FollowerView BuildView(RE::Actor* a, const Record& r, bool sleeping)
	{
		FollowerView v;
		v.id = a->GetFormID();
		v.name = a->GetDisplayFullName();
		for (int n = 0; n < 3; ++n) v.need[n] = r.need[n];
		v.stage[0] = StageOf(r.need[kHunger], R.hLvl);
		v.stage[1] = StageOf(r.need[kThirst], R.tLvl);
		v.stage[2] = StageOf(r.need[kFatigue], R.sLvl);
		v.cap[0] = Val(R.hLvl[5], 240.0f);
		v.cap[1] = Val(R.tLvl[4], 240.0f);
		v.cap[2] = Val(R.sLvl[4], 240.0f);
		v.nextAt[0] = v.stage[0] < 5 ? Val(R.hLvl[v.stage[0] + 1]) : 0.0f;
		v.nextAt[1] = v.stage[1] < 4 ? Val(R.tLvl[v.stage[1] + 1]) : 0.0f;
		v.nextAt[2] = v.stage[2] < 4 ? Val(R.sLvl[v.stage[2] + 1]) : 0.0f;
		v.disease = r.disease;
		v.dStage = r.dStage;
		v.counter = r.counter;
		v.timer = r.timer;
		v.sleeping = sleeping;
		v.combat = a->IsInCombat();
		v.abilities = CollectAbilities(a);
		return v;
	}

	// Runs queued menu actions. Call on the game thread with mtx held.
	void RunActions()
	{
		std::vector<Action> todo;
		{
			std::lock_guard lk(actMtx);
			todo.swap(pending);
		}
		const float caps[3]{ Val(R.hLvl[5], 240.0f), Val(R.tLvl[4], 240.0f), Val(R.sLvl[4], 240.0f) };
		for (auto& act : todo) {
			if (act.type == ActType::ReloadConfig) {
				LoadConfig();
				logger::info("settings reloaded from the menu");
				continue;
			}
			for (auto& [id, r] : state) {
				if (act.id != 0 && act.id != id) continue;
				auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
				if (!a) continue;
				switch (act.type) {
				case ActType::SetNeed: {
					const int n = std::clamp(act.arg, 0, 2);
					r.need[n] = std::clamp(act.value, 0.0f, caps[n]);
					Dbg("menu: {} need {} set to {:.0f}", a->GetDisplayFullName(), n, r.need[n]);
					break;
				}
				case ActType::ResetNeeds:
					for (auto& v : r.need) v = 0.0f;
					Dbg("menu: {} needs reset", a->GetDisplayFullName());
					break;
				case ActType::Infect:
					Infect(a, r, "menu", true, act.arg);
					break;
				case ActType::Cure:
					Cure(a, r);
					break;
				case ActType::EatNow:
					if (!EatOne(a, r, std::clamp(act.arg, 0, 2)))
						Dbg("menu: {} has no suitable {} in her inventory", a->GetDisplayFullName(), act.arg == 0 ? "food" : act.arg == 1 ? "drink" : "cure item");
					break;
				default:
					break;
				}
			}
		}
	}

	// -------------------------------------------------------------------- tick
	void Tick()
	{
		// save menu settings ~2 s after the last change
		if (cfgDirty && NowTicks() - cfgDirtyAt >= 2000000000LL) SaveUserConfig();

		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cal = RE::Calendar::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();

		// Remember loading screens: a time jump right after one is a fast travel (or carriage ride)
		const auto nowReal = std::chrono::steady_clock::now();
		static auto prevTick = nowReal;
		static auto lastLoading = std::chrono::steady_clock::time_point{};
		const auto tickGap = nowReal - prevTick;
		prevTick = nowReal;
		if (tickGap > 3s) lastLoading = nowReal;   // ticks were paused: loading screen
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen("Loading Menu")) lastLoading = nowReal;
		if (player && !player->Is3DLoaded()) lastLoading = nowReal;

		if (!player || !cal || !lists || !player->Is3DLoaded() || !R.ready) return;

		const float now = cal->GetCurrentGameTime() * 24.0f;
		// remember when the Sleep/Wait menu was last open (to tell waiting apart from fast travel)
		static auto lastWaitMenu = std::chrono::steady_clock::time_point{};
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen("Sleep/Wait Menu")) lastWaitMenu = std::chrono::steady_clock::now();
		std::lock_guard lk(mtx);
		RunActions();
		if (lastHours < 0.0f || now < lastHours) { lastHours = now; return; }
		float dtTick = std::min(now - lastHours, 72.0f);
		lastHours = now;

		const bool rndOff = cfg.respectRndState && Val(R.state, 1.0f) < 0.5f;
		if (rndOff) dtTick = 0.0f;

		// A big jump of game time right after a loading screen (and not sleeping/waiting) is a fast travel
		static float jumpHours = 0.0f, jumpCounted = 0.0f;
		const bool fastTravel = dtTick >= kJumpHours && !playerSleeping && (nowReal - lastWaitMenu) > 6s && (nowReal - lastLoading) < 20s;
		const float needDtTick = dtTick * ((fastTravel && cfg.fastTravelSlow) ? std::clamp(cfg.fastTravelMult, 0.0f, 1.0f) : 1.0f);
		if (fastTravel) {
			jumpHours = dtTick;
			jumpCounted = needDtTick;
			Dbg("time jump / fast travel detected: {:.2f} h, counted for needs: {:.2f} h", dtTick, needDtTick);
		}
		std::vector<FollowerView> newViews;
		std::unordered_set<RE::FormID> seenNow;

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

			// time of this tick plus any time that passed while she was not loaded (e.g. she arrived late after a fast travel)
			seenNow.insert(id);
			const float dt = dtTick + r.missedDt;
			const float needDt = needDtTick + r.missedNeedDt;
			if (r.missedDt > 0.05f)
				Dbg("{}: credited {:.2f} h that passed while she was not loaded (counted for needs: {:.2f} h)", a->GetDisplayFullName(), r.missedDt,
					r.missedNeedDt);
			r.missedDt = 0.0f;
			r.missedNeedDt = 0.0f;

			const bool bedSleep = a->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
			const bool isNear = a->GetParentCell() == player->GetParentCell() &&
			                  player->GetPosition().GetDistance(a->GetPosition()) <= cfg.sleepRadius;
			const bool sleeping = bedSleep || (playerSleeping && isNear);
			if (sleeping) r.sleptThisRest = true;

			if (dt > 0.0f) {
				r.need[kHunger] += needDt * Val(R.rate[kHunger]) * cfg.rateMult[0] * (sleeping ? cfg.sleepMult[0] : 1.0f);
				r.need[kThirst] += needDt * Val(R.rate[kThirst]) * cfg.rateMult[1] * (sleeping ? cfg.sleepMult[1] : 1.0f);
				if (sleeping) {
					r.need[kFatigue] -= dt * cfg.restPerHour;
				} else {
					r.need[kFatigue] += needDt * Val(R.rate[kFatigue]) * cfg.rateMult[2] * (a->IsInCombat() ? cfg.combatFatigueMult : 1.0f);
					if (cfg.diseaseAddsFatigue && r.disease >= 0 && r.dStage >= 0)
						r.need[kFatigue] += needDt * Val(R.disFat[std::clamp(r.dStage, 0, 2)]);
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

			if (!rndOff) AutoEat(a, r, sleeping, now);

			const int st[3]{ StageOf(r.need[kHunger], R.hLvl), StageOf(r.need[kThirst], R.tLvl), StageOf(r.need[kFatigue], R.sLvl) };
			for (int n = 0; n < 3; ++n) {
				if (st[n] > r.lastStage[n] && st[n] >= cfg.notifyStage) {
					const std::string& m = n == kHunger ? cfg.msgHunger[st[n]] : n == kThirst ? cfg.msgThirst[st[n]] : cfg.msgFatigue[st[n]];
					Say(a, m);
				}
				r.lastStage[n] = st[n];
			}

			// neglect: severe starvation/dehydration/exhaustion can make followers sick
			if (cfg.diseaseEnabled && cfg.infectNeglect && dt > 0.0f && r.disease < 0 && (st[0] >= 4 || st[1] >= 4 || st[2] >= 4)) {
				const float p = 100.0f * (1.0f - std::pow(1.0f - 0.02f, dt));
				if (Roll(p * cfg.diseaseChanceMult)) Infect(a, r, "weakened by neglect");
			}

			ApplyAbilities(a, r);
			if (doSummary) LogSummary(a, r);
			newViews.push_back(BuildView(a, r, sleeping));
		}

		// followers that were not loaded this tick: remember the time so it is credited when she shows up again
		for (auto& [fid, rec] : state) {
			if (seenNow.contains(fid)) continue;
			rec.missedDt = std::min(rec.missedDt + dtTick, 72.0f);
			rec.missedNeedDt = std::min(rec.missedNeedDt + needDtTick, 72.0f);
		}

		// publish the snapshot for the menu
		GlobalView g;
		g.ready = true;
		g.rndActive = !rndOff;
		for (int n = 0; n < 3; ++n) g.baseRate[n] = Val(R.rate[n]);
		for (int i = 0; i < 6; ++i) g.hLv[i] = Val(R.hLvl[i]);
		for (int i = 0; i < 5; ++i) { g.tLv[i] = Val(R.tLvl[i]); g.sLv[i] = Val(R.sLvl[i]); }
		g.chance[0] = Val(R.chRaw);
		g.chance[1] = Val(R.chStale);
		g.chance[2] = Val(R.chRiver);
		g.lastJumpHours = jumpHours;
		g.lastJumpCounted = jumpCounted;
		std::lock_guard vlk(viewMtx);
		views = std::move(newViews);
		gview = g;
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

			ApplyConsume(a, it->second, alch, e->baseObject, "consumed");
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
			if (it == state.end() || it->second.disease >= 0 || !cfg.diseaseEnabled || !cfg.infectHit) return BSEventNotifyControl::kContinue;
			bool source = false;
			for (auto& kw : cfg.diseaseSources) if (RaceHas(attacker, kw)) { source = true; break; }
			if (!source) return BSEventNotifyControl::kContinue;
			const float resist = std::clamp(victim->AsActorValueOwner()->GetActorValue(RE::ActorValue::kResistDisease), 0.0f, 100.0f);
			Dbg("{} was hit by disease source {}, resist {:.0f}", victim->GetDisplayFullName(), attacker->GetDisplayFullName(), resist);
			if (Roll(cfg.hitDiseaseChance * cfg.diseaseChanceMult * (1.0f - resist / 100.0f))) Infect(victim, it->second, "wounded");
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

	// ------------------------------------------------------------------ in-game menu
	// Drawn by SKSE Menu Framework on the render thread. It reads the snapshot (views / gview),
	// edits plain settings in cfg, and queues Actions that the game thread runs in Tick().
	namespace Menu
	{
		namespace Gui = ImGuiMCP;

		constexpr Gui::ImVec4 kGood{ 0.45f, 0.90f, 0.45f, 1.0f };
		constexpr Gui::ImVec4 kWarn{ 0.95f, 0.85f, 0.30f, 1.0f };
		constexpr Gui::ImVec4 kOrange{ 1.00f, 0.60f, 0.25f, 1.0f };
		constexpr Gui::ImVec4 kBad{ 1.00f, 0.35f, 0.35f, 1.0f };
		constexpr Gui::ImVec4 kHead{ 0.55f, 0.80f, 1.00f, 1.0f };

		const char* TR(const char* en, const char* zh) { return cfg.uiChinese ? zh : en; }
		void Txt(const std::string& s) { Gui::Text("%s", s.c_str()); }
		void Wrapped(const char* s) { Gui::TextWrapped("%s", s); }
		void Heading(const char* s)
		{
			Gui::Spacing();
			Gui::Separator();
			Gui::TextColored(kHead, "%s", s);
			Gui::Spacing();
		}

		void LangBar()
		{
			Gui::Text("%s", "Language / 语言:");
			Gui::SameLine();
			if (Gui::Button("EN")) { cfg.uiChinese = false; MarkDirty(); }
			Gui::SameLine();
			if (Gui::Button("ZH")) { cfg.uiChinese = true; MarkDirty(); }
		}

		const char* NeedName(int n)
		{
			static const char* en[3]{ "Hunger", "Thirst", "Fatigue" };
			static const char* zh[3]{ "饥饿", "口渴", "疲劳" };
			return (cfg.uiChinese ? zh : en)[std::clamp(n, 0, 2)];
		}

		const char* StageName(int n, int s)
		{
			static const char* en[3][6]{ { "Full", "Peckish", "Hungry", "Very hungry", "Starving", "Famished" },
				{ "Quenched", "A bit thirsty", "Thirsty", "Very thirsty", "Parched", "" },
				{ "Rested", "A bit tired", "Tired", "Very tired", "Exhausted", "" } };
			static const char* zh[3][6]{ { "饱腹", "有点饿", "饥饿", "很饿", "快饿昏了", "饿到极限" },
				{ "不渴", "有点渴", "口渴", "很渴", "极度干渴", "" },
				{ "精神饱满", "有点累", "疲劳", "很累", "精疲力竭", "" } };
			return (cfg.uiChinese ? zh : en)[std::clamp(n, 0, 2)][std::clamp(s, 0, 5)];
		}

		std::string AbilityLabel(const AbilityView& ab)
		{
			if (ab.group <= 2) return std::format("{} {}: {}", NeedName(ab.group), ab.index, StageName(ab.group, ab.index));
			return std::format("{}  ({} {}/3)", kDiseaseNames[std::clamp(ab.index / 3, 0, 5)], TR("stage", "阶段"), ab.index % 3 + 1);
		}

		Gui::ImVec4 StageColor(int st, int mx)
		{
			if (st <= 0) return kGood;
			const float t = static_cast<float>(st) / static_cast<float>(mx);
			if (t < 0.35f) return kWarn;
			if (t < 0.70f) return kOrange;
			return kBad;
		}

		void NeedBar(const FollowerView& f, int n)
		{
			Gui::Text("%s", NeedName(n));
			Gui::SameLine(90.0f);
			const float frac = f.cap[n] > 0.0f ? std::clamp(f.need[n] / f.cap[n], 0.0f, 1.0f) : 0.0f;
			const std::string overlay = std::format("{:.0f} / {:.0f}   {} ({}/{})", f.need[n], f.cap[n], StageName(n, f.stage[n]), f.stage[n], f.maxStage[n]);
			Gui::PushStyleColor(Gui::ImGuiCol_PlotHistogram, StageColor(f.stage[n], f.maxStage[n]));
			Gui::ProgressBar(frac, Gui::ImVec2{ -FLT_MIN, 0.0f }, overlay.c_str());
			Gui::PopStyleColor();
		}

		void __stdcall DrawStatus()
		{
			LangBar();
			GlobalView g;
			std::vector<FollowerView> v;
			{
				std::lock_guard lk(viewMtx);
				g = gview;
				v = views;
			}
			Heading(TR("Followers", "追随者状态"));
			if (!g.ready) {
				Wrapped(TR("Waiting for data - load a save and wait a second.", "等待数据中：请读取存档并稍等片刻。"));
				return;
			}
			Txt(std::format("RND: {}", g.rndActive ? TR("active", "运行中") : TR("paused (RND_State = 0), needs are frozen", "已暂停（RND_State = 0），需求不增长")));
			Txt(std::format("{}: {:.1f} / {:.1f} / {:.1f}  ({})", TR("Speed per hour (hunger / thirst / fatigue)", "每小时增长（饥饿 / 口渴 / 疲劳）"),
				g.baseRate[0] * cfg.rateMult[0], g.baseRate[1] * cfg.rateMult[1], g.baseRate[2] * cfg.rateMult[2], TR("RND base x your multiplier", "RND 基础值 x 你的倍率")));
			if (g.lastJumpHours > 0.0f)
				Txt(std::format("{}: {:.1f} h, {}: {:.1f} h", TR("Last time jump detected (fast travel etc.)", "最近一次检测到的跳时（快速旅行等）"), g.lastJumpHours,
					TR("counted for needs", "计入需求"), g.lastJumpCounted));

			if (v.empty()) {
				Gui::Spacing();
				Wrapped(TR("No tracked followers right now. Recruit a follower and stay in the same area.", "目前没有被追踪的追随者。请招募追随者并让他们在附近。"));
				return;
			}
			for (const auto& f : v) {
				Gui::PushID(static_cast<int>(f.id));
				const std::string head = std::format("{}##hdr", f.name);
				if (Gui::CollapsingHeader(head.c_str(), Gui::ImGuiTreeNodeFlags_DefaultOpen)) {
					Txt(std::format("{:08X}   {}{}", f.id, f.sleeping ? TR("[resting] ", "[休息中] ") : "", f.combat ? TR("[in combat]", "[战斗中]") : ""));
					for (int n = 0; n < 3; ++n) NeedBar(f, n);

					if (f.disease >= 0) {
						const float left = (f.counter < 2 && cfg.stageHours > 0.0f) ? std::max(0.0f, cfg.stageHours - f.timer) : 0.0f;
						std::string line = std::format("{}: {}  ({} {}/3)", TR("Disease", "疾病"), kDiseaseNames[std::clamp(f.disease, 0, 5)], TR("stage", "阶段"), f.dStage + 1);
						if (left > 0.0f) line += std::format("  - {} {:.1f} h", TR("next stage in", "距下一阶段"), left);
						Gui::TextColored(kBad, "%s", line.c_str());
					} else {
						Gui::TextColored(kGood, "%s", TR("Disease: none", "疾病：无"));
					}

					Gui::Spacing();
					Gui::TextColored(kHead, "%s", TR("Abilities / debuffs on this follower (checked in game):", "此追随者身上实际挂着的能力 / debuff（游戏内检查）："));
					if (f.abilities.empty()) {
						Wrapped(TR("(none)", "（无）"));
					} else {
						for (const auto& ab : f.abilities) {
							Gui::BulletText("%s", AbilityLabel(ab).c_str());
							if (!ab.fx.empty()) {
								Gui::Indent(28.0f);
								for (const auto& e : ab.fx) Gui::Text("%s", e.c_str());
								Gui::Unindent(28.0f);
							}
						}
					}
				}
				Gui::PopID();
			}
		}

		void __stdcall DrawSettings()
		{
			LangBar();
			GlobalView g;
			{
				std::lock_guard lk(viewMtx);
				g = gview;
			}

			Heading(TR("Need speed", "需求增长速度"));
			Wrapped(TR("Multiplier on top of RND's own rate. 1.0 = same as RND, 0 = that need never grows. Ctrl+Click a slider to type a value.",
				"在 RND 原有速度上的倍率。1.0 = 与 RND 相同，0 = 该需求不再增长。按住 Ctrl 点击滑条可直接输入数值。"));
			Gui::Spacing();
			for (int n = 0; n < 3; ++n) {
				Gui::PushID(n);
				Gui::Text("%s", NeedName(n));
				Gui::SameLine(90.0f);
				Gui::SetNextItemWidth(260.0f);
				if (Gui::SliderFloat("##rate", &cfg.rateMult[n], 0.0f, 5.0f, "%.2fx")) MarkDirty();
				Gui::SameLine();
				Txt(std::format("RND {:.1f}/h  ->  {:.1f}/h", g.baseRate[n], g.baseRate[n] * cfg.rateMult[n]));
				Gui::PopID();
			}
			if (Gui::Button(TR("Reset all speeds to 1.0", "全部恢复为 1.0"))) {
				for (auto& m : cfg.rateMult) m = 1.0f;
				MarkDirty();
			}

			Heading(TR("Fast travel", "快速旅行"));
			if (Gui::Checkbox(TR("Slow down needs during fast travel", "快速旅行时减缓需求增长"), &cfg.fastTravelSlow)) MarkDirty();
			Gui::BeginDisabled(!cfg.fastTravelSlow);
			Gui::Text("%s", TR("Share of travel time that counts", "旅行时间计入需求的比例"));
			Gui::SameLine();
			Gui::SetNextItemWidth(220.0f);
			if (Gui::SliderFloat("##ft", &cfg.fastTravelMult, 0.0f, 1.0f, "%.2f")) MarkDirty();
			Gui::EndDisabled();
			Wrapped(TR("0 = needs do not grow while fast travelling, 1 = same as normal. A fast travel is detected as a large jump of game time right after a loading screen that is not sleeping or waiting, so carriage rides count too. Time acceleration and waiting do not.",
				"0 = 快速旅行期间需求完全不增长，1 = 与平时相同。快速旅行是通过“载入画面之后游戏时间大幅跳跃，且不是睡觉或等待”来判断的，所以乘坐马车也算；时间加速和等待不算。"));

			Heading(TR("Notifications", "提示消息"));
			if (Gui::Checkbox(TR("Show notifications (e.g. \"X is hungry\")", "显示提示（例如“某某很饿”）"), &cfg.notify)) MarkDirty();
			Gui::Text("%s", TR("Show from stage", "从该阶段开始提示"));
			Gui::SameLine();
			Gui::SetNextItemWidth(160.0f);
			if (Gui::SliderInt("##ns", &cfg.notifyStage, 1, 5, "%d")) MarkDirty();

			Heading(TR("Saving", "保存"));
			Wrapped(TR("Changes apply immediately and are saved automatically to Data/SKSE/Plugins/RNDFollowers_User.ini.",
				"修改会立即生效，并自动保存到 Data/SKSE/Plugins/RNDFollowers_User.ini。"));
			if (Gui::Button(TR("Save now", "立即保存"))) SaveUserConfig();
		}

		void __stdcall DrawDisease()
		{
			LangBar();
			GlobalView g;
			{
				std::lock_guard lk(viewMtx);
				g = gview;
			}
			Heading(TR("Disease system", "疾病系统"));
			if (Gui::Checkbox(TR("Enable disease system (new infections)", "启用疾病系统（新的感染）"), &cfg.diseaseEnabled)) MarkDirty();
			Wrapped(TR("Turning this off only stops NEW infections. Followers that are already sick keep progressing and can still be cured.",
				"关闭只会阻止新的感染。已经生病的追随者仍会继续发展，也仍可被治愈。"));

			Heading(TR("Infection sources", "感染来源"));
			Gui::BeginDisabled(!cfg.diseaseEnabled);
			auto pct = [&](float base) { return std::clamp(base * cfg.diseaseChanceMult, 0.0f, 100.0f); };
			if (Gui::Checkbox(TR("Raw food", "吃生食"), &cfg.infectRaw)) MarkDirty();
			Gui::SameLine();
			Txt(std::format("  {:.0f}%", pct(g.chance[0])));
			if (Gui::Checkbox(TR("Spoiled food", "吃变质的食物"), &cfg.infectStale)) MarkDirty();
			Gui::SameLine();
			Txt(std::format("  {:.0f}%", pct(g.chance[1])));
			if (Gui::Checkbox(TR("River / stream water", "喝溪水 / 河水"), &cfg.infectRiver)) MarkDirty();
			Gui::SameLine();
			Txt(std::format("  {:.0f}%", pct(g.chance[2])));
			if (Gui::Checkbox(TR("Wounds from animals / undead", "被动物 / 亡灵攻击"), &cfg.infectHit)) MarkDirty();
			Gui::SameLine();
			Txt(std::format("  {:.1f}% {}", cfg.hitDiseaseChance * cfg.diseaseChanceMult, TR("per hit, before resistance", "每次命中，未计抗病")));
			if (Gui::Checkbox(TR("Neglect (starving, parched or exhausted)", "长期忽视需求（极度饥饿 / 干渴 / 疲惫）"), &cfg.infectNeglect)) MarkDirty();

			Gui::Spacing();
			Gui::Text("%s", TR("Chance multiplier", "感染几率倍率"));
			Gui::SameLine();
			Gui::SetNextItemWidth(220.0f);
			if (Gui::SliderFloat("##dm", &cfg.diseaseChanceMult, 0.0f, 3.0f, "%.2fx")) MarkDirty();
			Gui::EndDisabled();
			Wrapped(TR("The percentages are RND's own chances (raw / spoiled / river) multiplied by this value. Use the Debug page to infect or cure a follower on demand.",
				"百分比为 RND 自己的感染几率（生食 / 变质 / 河水）乘以该倍率。想立刻让追随者患病或治愈，请用 Debug 页面。"));
		}

		void __stdcall DrawAutoEat()
		{
			LangBar();
			Heading(TR("Auto eat and drink", "自动进食与饮水"));
			Wrapped(TR("Followers use food and drinks from their own inventory, so no eating-animation mod is needed. One item is removed and RND's own food values are applied. Nothing happens in combat, while resting, or while RND is paused. It can be used together with eating-animation mods.",
				"追随者会自动食用自己背包里的食物和饮品，不再需要进食动画 mod。每次消耗一个物品，并按 RND 自己的食物数值降低需求。战斗中、休息时、RND 暂停时不会进食。可以与进食动画 mod 同时使用。"));
			Gui::Spacing();
			if (Gui::Checkbox(TR("Followers eat food from their own inventory", "追随者自动食用自己背包里的食物"), &cfg.autoEat)) MarkDirty();
			if (Gui::Checkbox(TR("Followers use cure items when sick (Cure Disease potion, garlic bread, fish soup...)", "追随者生病时自动使用治疗疾病的物品（解疾药水、蒜香面包、鱼汤等）"), &cfg.autoCure)) MarkDirty();
			Gui::BeginDisabled(!cfg.autoEat);
			if (Gui::Checkbox(TR("Also drink water and drinks", "同时自动饮水和饮品"), &cfg.autoDrink)) MarkDirty();

			Gui::Spacing();
			Gui::Text("%s", TR("Eat when hunger reaches stage", "饥饿达到该阶段时进食"));
			Gui::SameLine();
			Gui::SetNextItemWidth(160.0f);
			if (Gui::SliderInt("##aes", &cfg.autoEatStage, 1, 5, "%d")) MarkDirty();
			Gui::SameLine();
			Txt(StageName(0, cfg.autoEatStage));

			Gui::Text("%s", TR("Drink when thirst reaches stage", "口渴达到该阶段时饮水"));
			Gui::SameLine();
			Gui::SetNextItemWidth(160.0f);
			if (Gui::SliderInt("##ads", &cfg.autoDrinkStage, 1, 4, "%d")) MarkDirty();
			Gui::SameLine();
			Txt(StageName(1, cfg.autoDrinkStage));

			Gui::Text("%s", TR("Pause between meals (game hours)", "两次进食的间隔（游戏小时）"));
			Gui::SameLine();
			Gui::SetNextItemWidth(160.0f);
			if (Gui::SliderFloat("##aec", &cfg.autoEatCooldown, 0.0f, 3.0f, "%.2f h")) MarkDirty();

			Heading(TR("Allowed items", "允许食用的物品"));
			if (Gui::Checkbox(TR("Raw food (can cause disease)", "生食（可能致病）"), &cfg.autoEatRaw)) MarkDirty();
			if (Gui::Checkbox(TR("Spoiled food (can cause disease)", "变质的食物（可能致病）"), &cfg.autoEatSpoiled)) MarkDirty();
			if (Gui::Checkbox(TR("River / stream water (can cause disease)", "溪水 / 河水（可能致病）"), &cfg.autoEatRiver)) MarkDirty();
			Gui::EndDisabled();
			Wrapped(TR("A follower prefers food that does not overshoot her hunger. Items she does not recognise as RND food or drink are never used. Use the Eat, Drink and Use cure item buttons on the Debug page to test.",
				"追随者会优先选择不会超出当前需求的食物。不属于 RND 食物或饮品的物品不会被使用。可以用 Debug 页面的“进食”“喝水”“用解药”按钮测试。"));
		}

		void __stdcall DrawDebug()
		{
			LangBar();
			GlobalView g;
			std::vector<FollowerView> v;
			{
				std::lock_guard lk(viewMtx);
				g = gview;
				v = views;
			}
			Heading(TR("All followers", "所有追随者"));
			if (Gui::Button(TR("Reset all needs to 0", "全部需求归零"))) QueueAction(Action{ ActType::ResetNeeds, 0, 0, 0.0f });
			Gui::SameLine();
			if (Gui::Button(TR("Cure all diseases", "治愈所有疾病"))) QueueAction(Action{ ActType::Cure, 0, 0, 0.0f });
			Gui::SameLine();
			if (Gui::Button(TR("Reload INI files", "重新读取 INI"))) QueueAction(Action{ ActType::ReloadConfig, 0, 0, 0.0f });

			static int stageSel = 3;
			static int diseaseSel = 0;
			Heading(TR("Test values", "测试数值"));
			Gui::Text("%s", TR("Target stage for the Hungry / Thirsty / Tired buttons", "“饥饿 / 口渴 / 疲劳”按钮要设置到的阶段"));
			Gui::SetNextItemWidth(160.0f);
			Gui::SliderInt("##stage", &stageSel, 1, 5, "%d");
			Gui::Text("%s", TR("Disease for the Infect button", "“感染”按钮使用的疾病"));
			Gui::SetNextItemWidth(220.0f);
			Gui::Combo("##dis", &diseaseSel, "Random\0Witbane\0Rockjoint\0Rattles\0BrainRot\0BoneBreakFever\0Ataxia\0");

			Heading(TR("Per follower", "单个追随者"));
			if (v.empty()) {
				Wrapped(TR("No tracked followers right now.", "目前没有被追踪的追随者。"));
				return;
			}
			for (const auto& f : v) {
				Gui::PushID(static_cast<int>(f.id));
				Txt(f.name);
				const int hs = std::clamp(stageSel, 0, 5), ts = std::clamp(stageSel, 0, 4), fs = std::clamp(stageSel, 0, 4);
				if (Gui::Button(TR("Hungry", "饥饿"))) QueueAction(Action{ ActType::SetNeed, f.id, 0, g.hLv[hs] });
				Gui::SameLine();
				if (Gui::Button(TR("Thirsty", "口渴"))) QueueAction(Action{ ActType::SetNeed, f.id, 1, g.tLv[ts] });
				Gui::SameLine();
				if (Gui::Button(TR("Tired", "疲劳"))) QueueAction(Action{ ActType::SetNeed, f.id, 2, g.sLv[fs] });
				Gui::SameLine();
				if (Gui::Button(TR("Reset needs", "需求归零"))) QueueAction(Action{ ActType::ResetNeeds, f.id, 0, 0.0f });
				Gui::SameLine();
				if (Gui::Button(TR("Infect", "感染"))) QueueAction(Action{ ActType::Infect, f.id, diseaseSel - 1, 0.0f });
				Gui::SameLine();
				if (Gui::Button(TR("Cure", "治愈"))) QueueAction(Action{ ActType::Cure, f.id, 0, 0.0f });
				Gui::SameLine();
				if (Gui::Button(TR("Eat", "进食"))) QueueAction(Action{ ActType::EatNow, f.id, 0, 0.0f });
				Gui::SameLine();
				if (Gui::Button(TR("Drink", "喝水"))) QueueAction(Action{ ActType::EatNow, f.id, 1, 0.0f });
				Gui::SameLine();
				if (Gui::Button(TR("Use cure item", "用解药"))) QueueAction(Action{ ActType::EatNow, f.id, 2, 0.0f });
				Gui::Spacing();
				Gui::PopID();
			}
		}

		void Register()
		{
			SKSEMenuFramework::SetSection("RND Followers");
			SKSEMenuFramework::AddSectionItem("Status", DrawStatus);
			SKSEMenuFramework::AddSectionItem("Needs and Speed", DrawSettings);
			SKSEMenuFramework::AddSectionItem("Disease", DrawDisease);
			SKSEMenuFramework::AddSectionItem("Auto Eat", DrawAutoEat);
			SKSEMenuFramework::AddSectionItem("Debug", DrawDebug);
		}
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
			logger::info("menu settings: speed x{:.2f}/{:.2f}/{:.2f}, fast-travel slow={} (x{:.2f}), disease enabled={}", cfg.rateMult[0], cfg.rateMult[1],
				cfg.rateMult[2], cfg.fastTravelSlow, cfg.fastTravelMult, cfg.diseaseEnabled);
			if (GetMenuFrameworkModule()) {
				Menu::Register();
				logger::info("SKSE Menu Framework found (version {}) - menu registered under \"RND Followers\"", SKSEMenuFramework::GetMenuFrameworkVersion());
			} else {
				logger::info("SKSE Menu Framework not found - in-game menu disabled, everything else works as before");
			}
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
