// SPDX-License-Identifier: GPL-3.0-only
// Modern Physique Framework - applies named TRI morphs to an NPC's head at runtime,
// on top of whatever FaceGeom NIF the load order produced.
//
// It never regenerates FaceGen and never touches the face tint. That is the whole point: the two
// failure modes people hit when they morph NPC faces - the head reverting to
// vanilla, and the black face bug - both come from *regenerating* the head.
// This adds per-vertex deltas to the base morph buffer ("FOD") of the head that
// is already loaded, so the replacer's sculpt is preserved and the engine's own
// expression morphs keep composing on top.
//
// Measured facts this is built on (FaceProbe session, 2026-09-04):
//   - "FOD" is present on every morphable head part of an NPC whose head came
//     from a baked FaceGeom NIF, replacer or vanilla.
//   - Writing into it reaches the buffer the GPU draws immediately; no explicit
//     update call is needed, because the morph job recomposes every frame.
//   - The write does NOT survive a cell reload, so it is reapplied on 3D load.
//   - The buffer is in different coordinate spaces for different NPCs, so only
//     relative deltas are ever written.
//   - Some head parts carry a "FOD" whose contents are collapsed, so the buffer
//     is validated before writing, not just its vertex count.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.hpp>

#include "bodytri.h"
#include "ini_edit.h"
#include "menu.h"
#include "tri.h"
#include "third_party/skee/SKEE.h"

#ifndef MPF_VERSION
#	define MPF_VERSION "unknown"
#endif
#ifndef MPF_BUILD_ID
#	define MPF_BUILD_ID "unknown"
#endif
#ifndef MPF_BODY_BACKEND
#	define MPF_BODY_BACKEND "unknown"
#endif

namespace
{
	// -----------------------------------------------------------------------
	// BSFaceGenBaseMorphExtraData is absent from CommonLibSSE-NG (only its RTTI
	// and vtable ids are there). The layout is measured, not guessed: the class
	// destructor at SkyrimSE RVA 0x3D8170 frees 0x28 bytes and NiExtraData is
	// 0x18, so the class adds exactly a pointer and a count.
	// -----------------------------------------------------------------------
	class BSFaceGenBaseMorphExtraData : public RE::NiExtraData
	{
	public:
		RE::NiPoint3* vertexData;   // 18
		std::uint32_t vertexCount;  // 20
		std::uint32_t pad24;        // 24
	};
	static_assert(sizeof(BSFaceGenBaseMorphExtraData) == 0x28);

	// F6/Shift+F7 deliberately: the throwaway FaceProbe diagnostic sits on F8-F12, and
	// the two need to be usable in the same session.
	constexpr std::uint32_t kKeyApply = 0x40;   // F6 - apply now, for testing
	constexpr std::uint32_t kKeyReport = 0x41;  // Shift+F7 - say what would be applied

	struct MorphRequest
	{
		std::string name;
		float       scale{ 1.0f };
	};

	struct NpcEntry
	{
		std::string               targetText;
		RE::FormID                baseFormID{ 0 };
		std::string               physique;
		std::string               bodyMorph;
		std::vector<MorphRequest> morphs;
		int                       priority{ 0 };
		std::string               sourceFile;
		std::string               sourcePath;
		bool                      fromFaction{ false };
		// Physique strength from the list, 0-100. It drives the face and body
		// morphs together without changing Skyrim's engine weight. A negative value
		// means "auto": use the original record weight as morph strength.
		float                     strength{ -1.0f };
		bool                      strengthFromRecord{ false };
		// What [Outfits] and [Skins] named for this NPC, verbatim. Resolved late,
		// not here, because a name only resolves once the load order is up.
		std::string               outfitText;
		std::string               skinText;
		// What outfitText resolved to at kDataLoaded, or 0. Kept so the per-actor
		// pass can dress somebody without resolving a name a second time.
		RE::FormID                outfitFormID{ 0 };
	};

	using HeadPartType = RE::BGSHeadPart::HeadPartType;

	// Sex and vertex count only narrow the field; they cannot decide it. On a
	// COtR head both the eyebrow and the mouth are 141 vertices, so the count
	// alone would put the eyebrow morph on the mouth.
	//
	// Names cannot decide it either, which is the measured part: Ordinary
	// People's FaceGeom names the head "KWA_FemaleHead" while the head part it
	// comes from is named "FemaleHead", and no head part of the former name
	// exists anywhere in the load order.
	//
	// So the decision is geometric - see PickSet.
	struct LibraryKey
	{
		bool          female{ false };
		std::uint32_t vertexCount{ 0 };

		bool operator==(const LibraryKey&) const = default;
	};

	struct LibraryKeyHash
	{
		std::size_t operator()(const LibraryKey& k) const noexcept
		{
			return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(k.vertexCount) << 1) | (k.female ? 1u : 0u));
		}
	};

	struct LibraryEntry
	{
		std::string   file;
		std::uint32_t partType{ 0 };
		rfm::TriFile  tri;
	};

	const char* PartTypeName(std::uint32_t a_type)
	{
		switch (static_cast<HeadPartType>(a_type)) {
		case HeadPartType::kMisc:       return "misc";
		case HeadPartType::kFace:       return "face";
		case HeadPartType::kEyes:       return "eyes";
		case HeadPartType::kHair:       return "hair";
		case HeadPartType::kFacialHair: return "facialhair";
		case HeadPartType::kScar:       return "scar";
		case HeadPartType::kEyebrows:   return "eyebrows";
		default:                        return "unknown";
		}
	}

	bool ParsePartType(std::string_view a_text, std::uint32_t& a_out)
	{
		for (std::uint32_t t = 0; t <= static_cast<std::uint32_t>(HeadPartType::kEyebrows); ++t) {
			if (a_text == PartTypeName(t)) {
				a_out = t;
				return true;
			}
		}
		return false;
	}

	std::unordered_map<LibraryKey, std::vector<LibraryEntry>, LibraryKeyHash> g_library;
	std::unordered_map<RE::FormID, NpcEntry>                                 g_entries;

	// A physique is the user-facing profile selected by every NPC/faction rule.
	// The DLL knows no built-in physique names: Heavy, Muscular, Thin, Old and
	// third-party profiles are all data supplied by [Physique.<name>] sections.
	struct PhysiqueDefinition
	{
		std::string name;
		std::string faceMorph;
		std::string bodyMorph;
		int         priority{ 0 };
		std::string sourceFile;
		std::string sourcePath;
	};

	// Lower-cased physique name -> definition. Names are case-insensitive in the
	// editable lists while morph names retain their exact case.
	std::unordered_map<std::string, PhysiqueDefinition> g_physiques;
	std::unordered_map<RE::FormID, std::vector<NpcEntry>> g_entryCandidates;

	struct SourceInfo
	{
		std::filesystem::path path;
		std::string           name;
		std::string           author;
		int                   priority{ 0 };
		std::size_t           directEntries{ 0 };
		std::size_t           factionRules{ 0 };
		std::size_t           conflictsWon{ 0 };
	};
	std::unordered_map<std::string, SourceInfo> g_sources;
	SKEE::IBodyMorphInterface*                       g_bodyMorphAPI{ nullptr };
	std::atomic_bool                                 g_dataLoaded{ false };
	std::atomic_uint64_t                             g_loadGeneration{ 0 };
	constexpr std::string_view                       kBodyMorphKey = "MPF";
	// Body state is tracked per reference, not as one process-wide "already done"
	// bit.  Apart from making reloads idempotent, this lets us distinguish a
	// target that is already correct from a target that changed while the same
	// 3D instance was alive.
	struct BodyApplyState
	{
		RE::FormID baseFormID{ 0 };
		std::string morph;
		float       strength{ 0.0f };
		bool        compatible{ false };
		std::uint64_t generation{ 0 };
	};
	std::unordered_map<RE::FormID, BodyApplyState>   g_bodyApplied;
	// A user-configurable seed changes random faction assignments intentionally,
	// while the actor/form identity keeps them stable across reloads and saves.
	std::uint32_t                                    g_randomSeed{ 0x4D504631u };
	// Guard against writing into the same buffer twice. It is deliberately keyed
	// on the buffer, not on the actor: two references of the same base NPC (two
	// console-spawned copies, say) can share one head, and a per-actor guard
	// would let the second one apply the morph on top of the first, leaving that
	// NPC visibly more extreme than the rest.
	//
	// The stored value is a fingerprint of what we left in the buffer. If the
	// address comes back holding something else, the head was rebuilt or the
	// allocator recycled the memory, and applying again is correct.
	std::unordered_map<const void*, double>                                  g_applied;
	// Vanilla outfit -> ours, so an NPC the user adds to the list gets dressed
	// without being told what to wear. Keyed on the outfit the NPC ALREADY has,
	// which beats guessing from a faction: it is the very garment being replaced,
	// so a farmer can never come out dressed as a jarl, and it works on NPCs from
	// mods too. Generated, and written as 0xID~plugin rather than EditorIDs, so
	// it resolves with no dependency on po3's Tweaks.
	std::unordered_map<RE::FormID, RE::FormID>                               g_outfitFamily;
	// An actor that already existed when the record changed keeps the clothes the
	// save handed it: setting defaultOutfit fixes who is dressed NEXT time, not
	// the man standing in front of you. Measured on Corpulus Vinius - record
	// patched, inventory empty. So the actor is dressed too, once, and this
	// records who has been.
	std::unordered_set<RE::FormID>                                           g_dressed;
	// Diagnostics only: which actors the sweep has already described, so a list
	// that repeats every three seconds is written once.
	std::unordered_set<RE::FormID>                                           g_counted;
	std::unordered_set<RE::FormID>                                           g_announced;

	struct CachedBodyTri
	{
		bool              loaded{ false };
		rfm::BodyTriIndex index;
		std::string       error;
	};

	// Populated only when the user presses Shift+F7. Normal application never opens or
	// indexes a body TRI, and each distinct asset is read at most once per game.
	std::unordered_map<std::string, CachedBodyTri> g_bodyTriDiagnostics;

	// Whole groups, for the NPCs nobody can name.
	//
	// A bandit or a random farmer is spawned from a levelled list with a base the
	// game invents, so there is no editor ID to write in [NPCs]. What their record
	// DOES carry is faction membership, so that is what a rule matches.
	//
	// The share is decided per RECORD, not per spawn, and deliberately: weight, skin
	// and outfit are written to the base, which every bandit of that archetype
	// shares. Rolling per spawn would fatten one man's face and every man's body.
	// Deciding per archetype means some kinds of bandit are heavy and others are
	// not, which reads as variety and stays put.
	struct GroupRule
	{
		std::string text;          // what the file said, for the log
		RE::FormID  factionID{ 0 };
		float       strength{ 100.0f };
		bool        automaticStrength{ false };
		bool        randomStrength{ false };
		int         percent{ 100 };
		std::string physique;
		bool        randomPhysique{ false };
		std::string sourceFile;
		std::string sourcePath;
		int         priority{ 0 };
	};

	std::vector<GroupRule> g_groups;

	// Faction name -> form id, shipped beside the morphs.
	//
	// The plugin can resolve an editor ID on its own, but only when po3's Tweaks is
	// installed, and this mod treats po3 as optional. A file people are meant to
	// edit should not work for some of them and not others, so the mapping travels
	// with the mod and a name resolves either way.
	std::unordered_map<std::string, RE::FormID> g_factionNames;

	std::string Lowered(std::string a_text)
	{
		std::transform(a_text.begin(), a_text.end(), a_text.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return a_text;
	}

	std::string                                                              g_skinMaleText;
	std::string                                                              g_skinFemaleText;
	int                                                                      g_skinMalePriority = (std::numeric_limits<int>::min)();
	int                                                                      g_skinFemalePriority = (std::numeric_limits<int>::min)();
	std::string                                                              g_skinMaleSource;
	std::string                                                              g_skinFemaleSource;
	bool                                                                     g_verbose = false;
	bool                                                                     g_diagnostics = false;
	bool                                                                     g_userLoggingExplicit = false;
	std::string                                                              g_menuStatus;

	struct OriginalNpcRecord
	{
		RE::TESObjectARMO* skin{ nullptr };
		RE::BGSOutfit*     outfit{ nullptr };
	};
	std::unordered_map<RE::FormID, OriginalNpcRecord>                        g_originalNpcRecords;

	// How far the rest-pose vertices of a morph set may sit from the geometry's
	// base buffer, once both are centred, and still be judged the same mesh.
	// Measured: the same mesh carrying an NPC's own sculpt lands at 0.5-0.6, a
	// different mesh of equal vertex count lands at 8.6-8.7. 3.0 sits in the gap.
	constexpr float kSameMeshLimit = 3.0f;

	std::filesystem::path DataPath(std::string_view a_relative)
	{
		return std::filesystem::path("Data/SKSE/Plugins/MPF") / a_relative;
	}

	std::filesystem::path ConfigPath(std::string_view a_relative = {})
	{
		return std::filesystem::path("Data/ModernPhysiqueFramework") / a_relative;
	}

	std::filesystem::path RuntimeSettingsPath()
	{
		return DataPath("settings.ini");
	}

	std::string SourceKey(const std::filesystem::path& a_path)
	{
		return Lowered(a_path.lexically_normal().generic_string());
	}

	void Trim(std::string& a_text);

	void LoadRuntimeSettings()
	{
		g_userLoggingExplicit = false;
		g_verbose = false;
		g_diagnostics = false;
		g_randomSeed = 0x4D504631u;
		const auto path = RuntimeSettingsPath();
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			return;
		}
		CSimpleIniA ini(false, true, false);
		ini.SetUnicode();
		if (ini.LoadFile(path.string().c_str()) < 0) {
			spdlog::warn("{}: cannot be read; using normal logging", path.string());
			return;
		}
		std::string level = ini.GetValue("Runtime", "Logging", "");
		Trim(level);
		level = Lowered(level);
		if (level == "normal" || level == "verbose" || level == "diagnostic") {
			g_userLoggingExplicit = true;
			g_verbose = level == "verbose" || level == "diagnostic";
			g_diagnostics = level == "diagnostic";
		}
		const auto configuredSeed = ini.GetLongValue(
			"Runtime",
			"RandomSeed",
			static_cast<long>(g_randomSeed));
		if (configuredSeed >= 0) {
			g_randomSeed = static_cast<std::uint32_t>(
				static_cast<unsigned long>(configuredSeed) & 0xFFFFFFFFul);
		} else {
			spdlog::warn("{}: Runtime.RandomSeed must be a non-negative integer; using default 0x{:08X}",
				path.string(), g_randomSeed);
		}
		spdlog::info("runtime settings: logging={} randomSeed=0x{:08X}",
			level.empty() ? "normal" : level, g_randomSeed);
	}

	void SetupLog()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}
		*path /= "MPF.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] %v");
	}

	// -----------------------------------------------------------------------
	// loading
	// -----------------------------------------------------------------------
	// The manifest declares what each TRI is for. Deriving sex and part from the
	// file name would be guessing, and a wrong guess here puts an eyebrow morph
	// on a mouth without anything looking broken until you see the face.
	// One manifest, read into the library. A file that does not parse is reported
	// and skipped rather than ending the whole load: with several mods shipping
	// their own, one broken file must not silence everybody else's morphs.
	int LoadManifest(const std::filesystem::path& a_path, const std::filesystem::path& a_dir,
		std::unordered_set<std::string>& a_seen)
	{
		const auto name = a_path.filename().string();
		toml::table manifest;
		try {
			manifest = toml::parse_file(a_path.string());
		} catch (const toml::parse_error& err) {
			spdlog::error("{}: {} (line {}) - this manifest is skipped",
				name, err.description(), err.source().begin.line);
			return 0;
		}

		const auto* sets = manifest["sets"].as_array();
		if (!sets) {
			spdlog::warn("{} has no [[sets]] entries", name);
			return 0;
		}

		int loaded = 0;
		for (const auto& node : *sets) {
			const auto* set = node.as_table();
			if (!set) {
				continue;
			}
			const auto file = (*set)["file"].value_or(std::string{});
			const auto sex = (*set)["sex"].value_or(std::string{});
			const auto part = (*set)["part"].value_or(std::string{});
			if (file.empty() || (sex != "male" && sex != "female")) {
				spdlog::error("{}: entry needs 'file' and sex=male|female, got file='{}' sex='{}'",
					name, file, sex);
				continue;
			}
			std::uint32_t partType = 0;
			if (!ParsePartType(part, partType)) {
				spdlog::error("{}: entry '{}' has unknown part='{}'", name, file, part);
				continue;
			}

			// Two manifests naming the same TRI would load the same geometry
			// twice, and PickSet would then weigh a duplicate against itself.
			// The first one named wins, and the log says which.
			const std::filesystem::path relative(file);
			const bool escapesFolder = relative.is_absolute() ||
				std::ranges::any_of(relative, [](const auto& component) { return component == ".."; });
			if (escapesFolder) {
				spdlog::error("{}: morph file '{}' must be a relative path inside the morph folder",
					name, file);
				continue;
			}
			const auto triPath = (a_path.parent_path() / relative).lexically_normal();
			auto displayPath = triPath.lexically_relative(a_dir).generic_string();
			if (displayPath.empty()) {
				displayPath = file;
			}
			const auto key = Lowered(displayPath);
			if (a_seen.contains(key)) {
				spdlog::info("{}: '{}' was already declared by another manifest - skipped",
					name, displayPath);
				continue;
			}

			rfm::TriFile tri;
			std::string  error;
			if (!tri.Load(triPath, error)) {
				spdlog::error("{}: morph file '{}' refused: {}", name, displayPath, error);
				continue;
			}
			// Do not reserve a name until it loaded successfully. Otherwise a broken
			// declaration in one content mod can prevent a later valid one from loading.
			a_seen.insert(key);
			const LibraryKey libKey{ sex == "female", tri.VertexCount() };
			spdlog::info("morphs '{}' ({}): sex={} part={} verts={} morphs={}",
				displayPath, name, sex, PartTypeName(partType), tri.VertexCount(), tri.MorphNames().size());
			g_library[libKey].push_back(LibraryEntry{ displayPath, partType, std::move(tri) });
			++loaded;
		}
		return loaded;
	}

	// Every .toml in the morph folder, not one fixed name. The INI side has worked
	// this way from the start; the morphs did not, so a second content mod's
	// manifest replaced the first one's file and its morphs went quiet with
	// nothing but a log line to say so. Now each mod ships its own.
	void LoadLibrary()
	{
		const auto      dir = DataPath("morphs");
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) {
			spdlog::error("no morph folder at {} - nothing can be applied", dir.string());
			return;
		}

		std::vector<std::filesystem::path> manifests;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(
				dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
			if (entry.is_regular_file() && Lowered(entry.path().extension().string()) == ".toml") {
				manifests.push_back(entry.path());
			}
		}
		if (ec) {
			spdlog::warn("could not inspect every entry under {}: {}", dir.string(), ec.message());
		}
		// Sorted so the "first one named wins" rule above means the same thing on
		// every machine, rather than whatever order the file system hands back.
		std::sort(manifests.begin(), manifests.end());

		if (manifests.empty()) {
			spdlog::error("no .toml manifest under {} - nothing can be applied", dir.string());
			return;
		}

		std::unordered_set<std::string> seen;
		int                             sets = 0;
		for (const auto& path : manifests) {
			sets += LoadManifest(path, dir, seen);
		}
		spdlog::info("{} morph set(s) loaded from {} manifest(s)", sets, manifests.size());
	}

	void Trim(std::string& a_text)
	{
		while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.front()))) {
			a_text.erase(a_text.begin());
		}
		while (!a_text.empty() && std::isspace(static_cast<unsigned char>(a_text.back()))) {
			a_text.pop_back();
		}
	}

	std::string CleanValue(std::string a_text)
	{
		if (const auto comment = a_text.find(';'); comment != std::string::npos) {
			a_text.erase(comment);
		}
		Trim(a_text);
		return a_text;
	}

	void AssignGlobalSetting(const char* a_name, std::string a_value, int a_priority,
		const std::string& a_source, std::string& a_current, int& a_currentPriority,
		std::string& a_currentSource)
	{
		a_value = CleanValue(std::move(a_value));
		if (a_value.empty()) {
			return;
		}
		const bool replace = a_priority > a_currentPriority ||
			(a_priority == a_currentPriority &&
				(a_currentSource.empty() || a_source < a_currentSource));
		if (!a_current.empty()) {
			spdlog::info("setting {} conflict: '{}' (p{}) vs '{}' (p{}) -> {} wins",
				a_name, a_source, a_priority, a_currentSource, a_currentPriority,
				replace ? a_source : a_currentSource);
		}
		if (replace) {
			a_current = std::move(a_value);
			a_currentPriority = a_priority;
			a_currentSource = a_source;
		}
	}

	bool ParseFloatStrict(std::string a_text, float& a_value)
	{
		Trim(a_text);
		if (a_text.empty()) {
			return false;
		}
		try {
			std::size_t consumed = 0;
			a_value = std::stof(a_text, &consumed);
			return consumed == a_text.size() && std::isfinite(a_value);
		} catch (const std::exception&) {
			return false;
		}
	}

	bool ParseIntStrict(std::string a_text, int& a_value)
	{
		Trim(a_text);
		if (a_text.empty()) {
			return false;
		}
		try {
			std::size_t consumed = 0;
			a_value = std::stoi(a_text, &consumed);
			return consumed == a_text.size();
		} catch (const std::exception&) {
			return false;
		}
	}

	// "0x00013479~Skyrim.esm", or an EditorID when po3's Tweaks is installed.
	RE::FormID ResolveTarget(const std::string& a_text)
	{
		std::string text = a_text;
		Trim(text);
		if (text.empty()) {
			return 0;
		}
		const auto tilde = text.find('~');
		if (tilde != std::string::npos) {
			std::string idText = text.substr(0, tilde);
			std::string plugin = text.substr(tilde + 1);
			Trim(idText);
			Trim(plugin);
			if (idText.empty() || plugin.empty()) {
				return 0;
			}
			auto*      handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return 0;
			}
			if (idText.front() == '+' || idText.front() == '-') {
				return 0;
			}
			errno = 0;
			char* end = nullptr;
			const auto parsed = std::strtoul(idText.c_str(), &end, 16);
			if (errno == ERANGE || end == idText.c_str() || *end != '\0' ||
				parsed > (std::numeric_limits<RE::FormID>::max)()) {
				return 0;
			}
			const auto localID = static_cast<RE::FormID>(parsed);
			auto*      form = handler->LookupForm(localID, plugin);
			return form ? form->GetFormID() : 0;
		}
		auto* form = RE::TESForm::LookupByEditorID(text);
		return form ? form->GetFormID() : 0;
	}

	void LoadFactionNames()
	{
		const auto path = DataPath("factions.ini");
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			return;
		}
		CSimpleIniA ini(false, true, false);
		ini.SetUnicode();
		if (ini.LoadFile(path.string().c_str()) < 0) {
			spdlog::error("{}: cannot be read", path.string());
			return;
		}
		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("Factions", keys);
		for (const auto& key : keys) {
			const auto id = ResolveTarget(CleanValue(ini.GetValue("Factions", key.pItem, "")));
			if (id != 0 && key.pItem) {
				g_factionNames[Lowered(key.pItem)] = id;
			}
		}
		spdlog::info("{} faction names available, so [Factions] can be written by name",
			g_factionNames.size());
	}

	// A name from the shipped table, an editor ID through po3, or a form id.
	RE::FormID ResolveFaction(const std::string& a_text)
	{
		if (const auto it = g_factionNames.find(Lowered(a_text)); it != g_factionNames.end()) {
			return it->second;
		}
		return ResolveTarget(a_text);
	}

	// [Factions]
	//
	//     BanditFaction    = 100, Muscular          every record in the faction
	//     ForswornFaction  = 100, Heavy, 25%        a stable quarter of them
	//     JobFarmerFaction = auto, Thin
	//     BanditFaction    = random, random          random strength and physique
	//
	// Same value shape as [NPCs] - a morph strength and required physique - plus a
	// percentage. 'random' is deterministic per faction record: it distributes
	// records without reshuffling them every time the game starts.
	void LoadGroupRules(CSimpleIniA& a_ini, const std::string& a_file,
		const std::string& a_sourcePath, int a_priority)
	{
		CSimpleIniA::TNamesDepend keys;
		a_ini.GetAllKeys("Factions", keys);
		for (const auto& key : keys) {
			GroupRule rule;
			rule.text = key.pItem ? key.pItem : "";
			rule.sourceFile = a_file;
			rule.sourcePath = a_sourcePath;
			rule.priority = a_priority;
			if (rule.text.empty()) {
				continue;
			}
			std::string value = a_ini.GetValue("Factions", key.pItem, "");
			if (const auto comment = value.find(';'); comment != std::string::npos) {
				value.erase(comment);
			}
			std::vector<std::string> fields;
			std::stringstream parts(value);
			std::string token;
			while (std::getline(parts, token, ',')) {
				Trim(token);
				fields.push_back(token);
			}
			bool bad = fields.size() < 2 || fields.size() > 3 ||
				std::ranges::any_of(fields, [](const auto& field) { return field.empty(); });
			if (!bad) {
				const auto strengthText = Lowered(fields[0]);
				if (strengthText == "auto") {
					rule.automaticStrength = true;
					rule.strength = -1.0f;
				} else if (strengthText == "random") {
					rule.randomStrength = true;
					rule.strength = 0.0f;
				} else {
					bad = !ParseFloatStrict(fields[0], rule.strength);
				}
				rule.physique = fields[1];
				rule.randomPhysique = Lowered(rule.physique) == "random";
				if (fields.size() == 3) {
					if (!fields[2].ends_with('%')) {
						bad = true;
					} else {
						fields[2].pop_back();
						bad = !ParseIntStrict(fields[2], rule.percent);
					}
				}
			}
			if (bad || rule.physique.empty() ||
				(!rule.randomStrength && !rule.automaticStrength && rule.strength < 0.0f) ||
				(!rule.randomStrength && !rule.automaticStrength && rule.strength > 100.0f) ||
				rule.percent < 0 || rule.percent > 100) {
				spdlog::error("{}: [Factions] '{}' = '{}' must be strength, physique and "
					"an optional percentage", a_file, rule.text, value);
				continue;
			}
			// Resolved later, in ExpandGroupRules: the name table is loaded after
			// the lists are read, and tying the two together by load order is the
			// kind of dependency that breaks quietly.
			const auto strengthLabel = rule.randomStrength ? std::string("random") :
				rule.automaticStrength ? std::string("auto") : std::format("{:g}", rule.strength);
			const auto physiqueLabel = rule.randomPhysique ? std::string("random") : rule.physique;
			spdlog::info("   faction {:<28} strength {}, physique '{}', {}%", rule.text,
				strengthLabel, physiqueLabel, rule.percent);
			g_groups.push_back(std::move(rule));
		}
	}

	// [Physique.Heavy]
	// FaceMorph = RLY_Heavy
	// BodyMorph = RM_Heavy
	//
	// Section suffixes are user-defined and case-insensitive. BodyMorph is the
	// RaceMenu body-morph slider name in the TRI; it is optional for face-only
	// profiles. FaceMorph remains required.
	void LoadPhysiques(CSimpleIniA& a_ini, const std::string& a_file,
		const std::string& a_sourcePath, int a_priority)
	{
		CSimpleIniA::TNamesDepend sections;
		a_ini.GetAllSections(sections);
		constexpr std::string_view prefix = "Physique.";
		for (const auto& section : sections) {
			std::string sectionName = section.pItem ? section.pItem : "";
			if (!Lowered(sectionName).starts_with(Lowered(std::string(prefix)))) {
				continue;
			}

			PhysiqueDefinition candidate;
			candidate.name = sectionName.substr(prefix.size());
			Trim(candidate.name);
			candidate.priority = a_priority;
			candidate.sourceFile = a_file;
			candidate.sourcePath = a_sourcePath;
			candidate.faceMorph = a_ini.GetValue(sectionName.c_str(), "FaceMorph", "");
			if (const auto comment = candidate.faceMorph.find(';'); comment != std::string::npos) {
				candidate.faceMorph.erase(comment);
			}
			Trim(candidate.faceMorph);
			candidate.bodyMorph = a_ini.GetValue(sectionName.c_str(), "BodyMorph", "");
			if (const auto comment = candidate.bodyMorph.find(';'); comment != std::string::npos) {
				candidate.bodyMorph.erase(comment);
			}
			Trim(candidate.bodyMorph);

			if (candidate.name.empty() || candidate.faceMorph.empty()) {
				spdlog::error("{}: [{}] requires a physique name and FaceMorph", a_file, sectionName);
				continue;
			}

			const auto key = Lowered(candidate.name);
			if (const auto existing = g_physiques.find(key); existing != g_physiques.end()) {
				const auto& old = existing->second;
				const bool replace = candidate.priority > old.priority ||
					(candidate.priority == old.priority && candidate.sourceFile < old.sourceFile);
				spdlog::info("physique '{}' conflict: '{}' (p{}) vs '{}' (p{}) -> {} wins",
					candidate.name, candidate.sourceFile, candidate.priority, old.sourceFile,
					old.priority, replace ? candidate.sourceFile : old.sourceFile);
				if (!replace) {
					continue;
				}
			}
			g_physiques.insert_or_assign(key, std::move(candidate));
		}
	}

	// One line per NPC, and the line is the whole story:
	//
	//     [NPCs]
	//     Alvor = 75, Heavy
	//     Sven  = 70, Muscular
	//     0x0001347F~Skyrim.esm = 80, Thin
	//     Gerdur = auto, Heavy
	//
	// The number is the physique percentage on a 0-100 scale and drives the face
	// and body morphs one to one. "auto" reads the original record weight and
	// uses that value as morph strength without changing the weight.
	void LoadPatchFile(const std::filesystem::path& a_path)
	{
		CSimpleIniA ini(false, true, false);
		ini.SetUnicode();
		const auto fileName = a_path.filename().string();
		if (ini.LoadFile(a_path.string().c_str()) < 0) {
			spdlog::error("{}: cannot be read", fileName);
			return;
		}

		const int priority = static_cast<int>(ini.GetLongValue("Settings", "Priority", 0));
		const auto sourcePath = a_path.lexically_normal().generic_string();
		auto displayName = CleanValue(ini.GetValue("Settings", "Name", ""));
		if (displayName.empty()) {
			displayName = a_path.stem().string();
		}
		SourceInfo source;
		source.path = a_path.lexically_normal();
		source.name = std::move(displayName);
		source.author = CleanValue(ini.GetValue("Settings", "Author", ""));
		source.priority = priority;
		g_sources.insert_or_assign(SourceKey(a_path), std::move(source));

		if (!g_userLoggingExplicit && ini.GetBoolValue("Settings", "Diagnostics", false)) {
			g_diagnostics = true;
		}
		AssignGlobalSetting("SkinMale", ini.GetValue("Settings", "SkinMale", ""),
			priority, fileName, g_skinMaleText, g_skinMalePriority, g_skinMaleSource);
		AssignGlobalSetting("SkinFemale", ini.GetValue("Settings", "SkinFemale", ""),
			priority, fileName, g_skinFemaleText, g_skinFemalePriority, g_skinFemaleSource);

		LoadPhysiques(ini, fileName, sourcePath, priority);
		LoadGroupRules(ini, fileName, sourcePath, priority);
		if (auto found = g_sources.find(SourceKey(a_path)); found != g_sources.end()) {
			CSimpleIniA::TNamesDepend groupKeys;
			ini.GetAllKeys("Factions", groupKeys);
			found->second.factionRules = groupKeys.size();
		}

		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("NPCs", keys);
		if (keys.empty()) {
			spdlog::info("{}: no direct [NPCs] entries", fileName);
			return;
		}

		for (const auto& key : keys) {
			NpcEntry entry;
			entry.sourceFile = fileName;
			entry.sourcePath = sourcePath;
			entry.priority = priority;
			entry.targetText = key.pItem ? key.pItem : "";
			Trim(entry.targetText);
			if (entry.targetText.empty()) {
				continue;
			}

			// SimpleIni does not strip trailing comments from values, so cut one
			// before splitting the required "strength, physique" pair.
			const auto uncomment = [](std::string t) {
				if (const auto c = t.find(';'); c != std::string::npos) {
					t.erase(c);
				}
				return t;
			};

			// "<strength>, <physique>" -- both fields are required.
			std::string value = uncomment(ini.GetValue("NPCs", key.pItem, ""));
			const auto comma = value.find(',');
			if (comma == std::string::npos || value.find(',', comma + 1) != std::string::npos) {
				spdlog::error("{}: '{}' must be written as strength, physique", fileName,
					entry.targetText);
				continue;
			}
			entry.physique = value.substr(comma + 1);
			value = value.substr(0, comma);
			Trim(value);
			Trim(entry.physique);

			if (Lowered(value) == "auto") {
				entry.strength = -1.0f;
				entry.strengthFromRecord = true;
			} else {
				if (!ParseFloatStrict(value, entry.strength)) {
					spdlog::error("{}: '{}' has value '{}', which is neither a strength nor 'auto'",
						fileName, entry.targetText, value);
					continue;
				}
				if (entry.strength < 0.0f || entry.strength > 100.0f) {
					spdlog::error("{}: '{}' has strength {:.1f}; the scale is 0 to 100", fileName,
						entry.targetText, entry.strength);
					continue;
				}
			}
			if (entry.physique.empty()) {
				spdlog::error("{}: '{}' names no physique", fileName, entry.targetText);
				continue;
			}

			// Same key in the other two sections, when the author named one. Both
			// are optional: without them the NPC still gets the family outfit and
			// the default skin, which is what makes adding a name to [NPCs] enough.
			entry.outfitText = uncomment(ini.GetValue("Outfits", key.pItem, ""));
			entry.skinText = uncomment(ini.GetValue("Skins", key.pItem, ""));
			Trim(entry.outfitText);
			Trim(entry.skinText);

			entry.baseFormID = ResolveTarget(entry.targetText);
			if (entry.baseFormID == 0) {
				spdlog::warn("{}: target '{}' did not resolve (EditorIDs need po3's Tweaks)",
					fileName, entry.targetText);
				continue;
			}
			if (!RE::TESForm::LookupByID<RE::TESNPC>(entry.baseFormID)) {
				spdlog::warn("{}: target '{}' resolved to 0x{:08X}, but it is not an NPC",
					fileName, entry.targetText, entry.baseFormID);
				continue;
			}
			// What a name resolved to is worth writing down. A name always
			// resolves to something, so "57 loaded" says nothing about whether
			// they are the NPCs the author meant - and an entry that resolved to
			// the wrong form looks exactly like an entry that works.
			spdlog::info("   {:<26} -> 0x{:08X}  strength {}, physique '{}'", entry.targetText,
				entry.baseFormID, entry.strength < 0.0f ? std::string("from the record") :
				std::format("{:g}", entry.strength), entry.physique);
			g_entryCandidates[entry.baseFormID].push_back(entry);
			if (auto found = g_sources.find(SourceKey(a_path)); found != g_sources.end()) {
				++found->second.directEntries;
			}

			// Highest priority wins; ties break on file name so the outcome does
			// not depend on directory order.
			auto existing = g_entries.find(entry.baseFormID);
			if (existing != g_entries.end()) {
				const auto& old = existing->second;
				const bool  replace = entry.priority > old.priority ||
									 (entry.priority == old.priority && entry.sourceFile < old.sourceFile);
				spdlog::info("conflict on 0x{:08X}: '{}' (p{}) vs '{}' (p{}) -> {} wins",
					entry.baseFormID, entry.sourceFile, entry.priority,
					old.sourceFile, old.priority, replace ? entry.sourceFile : old.sourceFile);
				if (!replace) {
					continue;
				}
			}
			g_entries.insert_or_assign(entry.baseFormID, std::move(entry));
		}
	}

	// Each mod may ship its own INI in Data/ModernPhysiqueFramework. Every file
	// uses the same schema and can define physiques, NPCs, and factions.
	void LoadPatches()
	{
		std::error_code ec;
		int             files = 0;
		const auto dir = ConfigPath();
		std::vector<std::filesystem::path> paths;
		if (std::filesystem::is_directory(dir, ec)) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator(
					dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
				if (entry.is_regular_file() && Lowered(entry.path().extension().string()) == ".ini") {
					paths.push_back(entry.path());
				}
			}
			if (ec) {
				spdlog::warn("could not inspect every entry under {}: {}", dir.string(), ec.message());
			}
		}
		std::sort(paths.begin(), paths.end());
		for (const auto& path : paths) {
			LoadPatchFile(path);
			++files;
		}

		if (files == 0) {
			spdlog::error("no INI found under {}", dir.string());
			return;
		}
		spdlog::info("{} NPC entr(ies) loaded from {} file(s); {} physique(s) defined",
			g_entries.size(), files, g_physiques.size());
	}

	// Nothing else in this mod says "adult". A set is chosen by sex, vertex count
	// and shape, and childhead.tri is 996 vertices sitting 0.247 from the vanilla
	// female head - measured - so the head set added today would match a child and
	// fatten her face. The list is the only thing standing in the way, and a list
	// is a thing people edit. So it is refused here instead, whatever the list says.
	bool IsChildNPC(RE::TESNPC* a_base)
	{
		return a_base && a_base->race && a_base->race->IsChildRace();
	}

	// Stable across load-order changes: full FormIDs include the plugin's current
	// compile index, so hashing them would reshuffle a faction after unrelated mods
	// are enabled. Local ID + source filename identifies the record without that.
	std::uint32_t StableRecordHash(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return 0;
		}
		// The record identity is the stable part; the runtime seed is the explicit
		// content-author/user knob for changing faction shares and random choices.
		std::uint32_t h = 2166136261u ^ g_randomSeed;
		const auto* file = a_form->GetFile(0);
		const std::uint32_t id = file ? a_form->GetLocalFormID() : a_form->GetFormID();
		for (int shift = 0; shift < 32; shift += 8) {
			h ^= (id >> shift) & 0xFFu;
			h *= 16777619u;
		}
		if (file) {
			for (const unsigned char c : std::string_view(file->fileName)) {
				h ^= static_cast<std::uint32_t>(std::tolower(c));
				h *= 16777619u;
			}
		}
		h ^= h >> 15;
		return h;
	}

	// Stable per-record salt for faction-level randomization. The faction rule is
	// included so that two random rules affecting the same record do not make the
	// same choice by accident, while the result remains unchanged between runs.
	std::uint32_t StableGroupHash(const RE::TESForm* a_form, const GroupRule& a_rule,
		std::uint32_t a_salt)
	{
		std::uint32_t h = StableRecordHash(a_form) ^ a_salt;
		auto mix = [&h](std::string_view a_text) {
			for (const unsigned char c : a_text) {
				h ^= static_cast<std::uint32_t>(std::tolower(c));
				h *= 16777619u;
			}
		};
		mix(a_rule.sourceFile);
		mix(a_rule.text);
		h ^= h >> 15;
		h *= 2246822519u;
		h ^= h >> 13;
		return h;
	}

	std::vector<std::string> SortedPhysiqueNames()
	{
		std::vector<std::string> names;
		names.reserve(g_physiques.size());
		for (const auto& [key, definition] : g_physiques) {
			static_cast<void>(key);
			names.push_back(definition.name);
		}
		std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) {
			return Lowered(a) < Lowered(b);
		});
		return names;
	}

	// A generated table, one line per vanilla outfit:
	//
	//     0x0001BCC0~Skyrim.esm = 0x000804~Modern Physique Framework.esp
	//
	// It exists so that adding a name to [NPCs] is genuinely enough. Before this,
	// weight, skin and outfit all came from SkyPatcher files written before the
	// user ever saw the mod, so an NPC they added got a fat face on a thin body in
	// vanilla clothes - worse than not being on the list at all.
	void LoadOutfitTable()
	{
		const auto path = DataPath("outfits.ini");
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) {
			spdlog::warn("no outfit table at {} - NPCs without an explicit [Outfits] line keep "
				"their vanilla clothes", path.string());
			return;
		}
		CSimpleIniA ini(false, true, false);
		ini.SetUnicode();
		if (ini.LoadFile(path.string().c_str()) < 0) {
			spdlog::error("{}: cannot be read", path.string());
			return;
		}
		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("Outfits", keys);
		int unresolved = 0;
		for (const auto& key : keys) {
			const auto from = ResolveTarget(key.pItem ? key.pItem : "");
			const auto to = ResolveTarget(CleanValue(ini.GetValue("Outfits", key.pItem, "")));
			if (from == 0 || to == 0) {
				++unresolved;
				continue;
			}
			g_outfitFamily[from] = to;
		}
		spdlog::info("outfit table: {} vanilla outfits mapped, {} unresolved",
			g_outfitFamily.size(), unresolved);
	}

	// Auto-strength capture, skin and outfit changes, applied at kDataLoaded.
	//
	// This is the same thing SkyPatcher does, and deliberately at the same moment:
	// no actor has been built yet, so nothing needs a 3D refresh and nothing pops.
	// The difference is the source - the file the user actually edits - which is
	// the whole point of moving it here.
	// Turn each faction rule into list entries, once, before anything is patched.
	//
	// Walking every NPC record sounds heavy and is not: it happens once at data
	// load, before an actor exists, and the game has already built the array.
	void ExpandGroupRules()
	{
		if (g_groups.empty()) {
			return;
		}
		// Explicit [NPCs] entries always win. Between overlapping faction rules,
		// expand the same winner policy used everywhere else first: higher priority,
		// then lower filename. The first matching group can then claim the record.
		std::stable_sort(g_groups.begin(), g_groups.end(), [](const auto& a, const auto& b) {
			if (a.priority != b.priority) {
				return a.priority > b.priority;
			}
			if (a.sourceFile != b.sourceFile) {
				return a.sourceFile < b.sourceFile;
			}
			return a.text < b.text;
		});
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return;
		}
		const auto physiqueNames = SortedPhysiqueNames();
		int added = 0, skippedChildren = 0, alreadyListed = 0, notChosen = 0;
		for (auto& rule : g_groups) {
			rule.factionID = ResolveFaction(rule.text);
			auto* faction = rule.factionID
				? RE::TESForm::LookupByID<RE::TESFaction>(rule.factionID)
				: nullptr;
			if (!faction) {
				spdlog::warn("faction '{}' did not resolve - check the spelling against "
					"factions.ini, or write it as 0xID~Plugin", rule.text);
				continue;
			}
			int matched = 0;
			for (auto* npc : handler->GetFormArray<RE::TESNPC>()) {
				if (!npc || !npc->IsInFaction(faction)) {
					continue;
				}
				++matched;
				if (IsChildNPC(npc)) {
					++skippedChildren;
					continue;
				}
				const bool chosen = rule.percent >= 100 ||
					static_cast<int>(StableRecordHash(npc) % 100u) < rule.percent;
				NpcEntry groupCandidate;
				groupCandidate.targetText = rule.text;
				groupCandidate.sourceFile = rule.sourceFile;
				groupCandidate.sourcePath = rule.sourcePath;
				groupCandidate.priority = rule.priority;
				groupCandidate.strength = rule.strength;
				groupCandidate.baseFormID = npc->GetFormID();
				groupCandidate.physique = rule.physique;
				groupCandidate.fromFaction = true;
				if (chosen && rule.randomPhysique) {
					if (physiqueNames.empty()) {
						spdlog::error("faction '{}' requested random physique, but no physiques are defined",
							rule.text);
						continue;
					}
					const auto hash = StableGroupHash(npc, rule, 0x9E3779B9u);
					groupCandidate.physique = physiqueNames[hash % physiqueNames.size()];
				}
				if (chosen && rule.randomStrength) {
					const auto hash = StableGroupHash(npc, rule, 0x85EBCA6Bu);
					groupCandidate.strength = static_cast<float>(hash % 10001u) / 100.0f;
				}
				if (chosen) {
					g_entryCandidates[groupCandidate.baseFormID].push_back(groupCandidate);
				}
				// A valid name written by hand always wins over a group. A typo in its
				// physique must not silently block an otherwise valid faction rule.
				if (const auto existing = g_entries.find(npc->GetFormID());
					existing != g_entries.end()) {
					if (g_physiques.contains(Lowered(existing->second.physique))) {
						++alreadyListed;
						continue;
					}
					spdlog::error("{}: '{}' uses undefined physique '{}' and no longer blocks "
						"the matching faction rule from '{}'", existing->second.sourceFile,
						existing->second.targetText, existing->second.physique, rule.sourceFile);
					g_entries.erase(existing);
				}
				// Stable share: the same records are chosen every time, because the
				// choice comes from the form id and nothing else.
				if (!chosen) {
					++notChosen;
					continue;
				}
				g_entries.insert_or_assign(groupCandidate.baseFormID, std::move(groupCandidate));
				++added;
			}
			spdlog::info("faction '{}': {} records in it", rule.text, matched);
		}
		spdlog::info("group rules added {} NPCs ({} already named by hand, {} not in "
			"the chosen share, {} children refused)",
			added, alreadyListed, notChosen, skippedChildren);
	}

	// Resolve after every list file and faction rule has been loaded, so a content
	// pack may define a physique in a different INI from the NPCs that use it.
	// A missing physique rejects the whole entry: silently falling back would make
	// a mandatory user choice meaningless and could mismatch face and body.
	void ResolvePhysiques()
	{
		int rejected = 0;
		for (auto it = g_entries.begin(); it != g_entries.end();) {
			const auto definition = g_physiques.find(Lowered(it->second.physique));
			if (definition == g_physiques.end()) {
				spdlog::error("{}: '{}' uses undefined physique '{}' - entry rejected",
					it->second.sourceFile, it->second.targetText, it->second.physique);
				it = g_entries.erase(it);
				++rejected;
				continue;
			}
			it->second.physique = definition->second.name;
			it->second.bodyMorph = definition->second.bodyMorph;
			it->second.morphs.clear();
			it->second.morphs.push_back({ definition->second.faceMorph, 1.0f });
			++it;
		}
		spdlog::info("physiques resolved for {} NPC entr(ies), {} rejected", g_entries.size(), rejected);
	}

	void ApplyRecordChanges()
	{
		auto* skinMale = g_skinMaleText.empty() ? nullptr
			: RE::TESForm::LookupByID<RE::TESObjectARMO>(ResolveTarget(g_skinMaleText));
		auto* skinFemale = g_skinFemaleText.empty() ? nullptr
			: RE::TESForm::LookupByID<RE::TESObjectARMO>(ResolveTarget(g_skinFemaleText));

		int autoStrengths = 0, skins = 0, outfits = 0, byFamily = 0,
			dressless = 0, children = 0;
		for (auto& [formID, entry] : g_entries) {
			auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID);
			if (!npc) {
				continue;
			}
			if (IsChildNPC(npc)) {
				spdlog::warn("'{}' (0x{:08X}) is a child and was skipped - skin and outfit "
					"were not changed", entry.targetText, formID);
				++children;
				continue;
			}
			g_originalNpcRecords.try_emplace(formID,
				OriginalNpcRecord{ npc->skin, npc->defaultOutfit });

			if (entry.strength < 0.0f) {
				entry.strength = std::clamp(npc->weight, 0.0f, 100.0f);
				entry.strengthFromRecord = true;
				++autoStrengths;
			}
			auto* skin = entry.skinText.empty() ? nullptr
				: RE::TESForm::LookupByID<RE::TESObjectARMO>(ResolveTarget(entry.skinText));
			if (!skin) {
				if (!entry.skinText.empty()) {
					spdlog::warn("'{}': skin '{}' did not resolve (EditorIDs need po3's Tweaks) "
						"- falling back to the default", entry.targetText, entry.skinText);
				}
				skin = npc->IsFemale() ? skinFemale : skinMale;
			}
			if (skin) {
				npc->skin = skin;
				++skins;
			}

			// Named outfit first; otherwise whatever the family table says for the
			// clothes this NPC is already wearing. Falling through both leaves the
			// vanilla outfit alone, which is the honest outcome for an NPC whose
			// garment nobody has rebuilt - a thin torso is better than none.
			auto* outfit = entry.outfitText.empty() ? nullptr
				: RE::TESForm::LookupByID<RE::BGSOutfit>(ResolveTarget(entry.outfitText));
			bool fromFamily = false;
			if (!outfit && npc->defaultOutfit) {
				if (const auto it = g_outfitFamily.find(npc->defaultOutfit->GetFormID());
					it != g_outfitFamily.end()) {
					outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(it->second);
					fromFamily = outfit != nullptr;
				}
			}
			if (outfit) {
				// SetDefaultOutfit, not a raw assignment to defaultOutfit: the
				// function exists to do the bookkeeping the game needs for the
				// change to be noticed.
				npc->SetDefaultOutfit(outfit);
				entry.outfitFormID = outfit->GetFormID();
				++outfits;
				byFamily += fromFamily ? 1 : 0;
			} else {
				++dressless;
				if (g_diagnostics) {
					spdlog::info("'{}' keeps vanilla clothes: outfit 0x{:08X} is in no family",
						entry.targetText,
						npc->defaultOutfit ? npc->defaultOutfit->GetFormID() : 0);
				}
			}
		}
		spdlog::info("records patched: {} strengths read from original weights, "
			"{} skins, {} outfits ({} chosen by family), {} left in vanilla clothes, {} children "
			"refused; NPC weights were preserved", autoStrengths, skins, outfits, byFamily,
			dressless, children);
	}

	std::string StableFormText(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		if (const auto* file = a_form->GetFile(0); file && file->fileName && *file->fileName) {
			return std::format("0x{:08X}~{}", a_form->GetLocalFormID(), file->fileName);
		}
		return std::format("0x{:08X}", a_form->GetFormID());
	}

	std::string SourceDisplayName(const std::string& a_path, const std::string& a_fallback)
	{
		const auto found = g_sources.find(SourceKey(a_path));
		return found == g_sources.end() ? a_fallback : found->second.name;
	}

	void PublishMenuSnapshot()
	{
		static std::atomic_uint64_t nextRevision{ 1 };
		auto snapshot = std::make_shared<mpf::menu::Snapshot>();
		snapshot->revision = nextRevision.fetch_add(1, std::memory_order_relaxed);
		snapshot->status = g_menuStatus;
		snapshot->buildID = MPF_BUILD_ID;
		snapshot->backend = MPF_BODY_BACKEND;
		snapshot->skeeAvailable = g_bodyMorphAPI != nullptr;
		snapshot->menuApiVersion = mpf::menu::FrameworkApiVersion();
		snapshot->loggingLevel = g_diagnostics ? 2 : (g_verbose ? 1 : 0);

		for (auto& [key, source] : g_sources) {
			source.conflictsWon = 0;
		}
		for (const auto& [formID, candidates] : g_entryCandidates) {
			const auto effective = g_entries.find(formID);
			if (effective == g_entries.end()) {
				continue;
			}
			auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID);
			mpf::menu::NpcRow row;
			row.formID = formID;
			row.stableID = StableFormText(npc);
			row.name = npc && npc->GetName() && *npc->GetName() ? npc->GetName() : effective->second.targetText;
			row.physique = effective->second.physique;
			row.strength = effective->second.strength;
			row.automaticStrength = effective->second.strengthFromRecord;
			row.sourceName = SourceDisplayName(effective->second.sourcePath,
				effective->second.sourceFile);

			for (const auto& candidate : candidates) {
				mpf::menu::Candidate item;
				item.sourcePath = candidate.sourcePath;
				item.sourceName = SourceDisplayName(candidate.sourcePath, candidate.sourceFile);
				item.targetKey = candidate.targetText;
				item.physique = candidate.physique;
				item.automaticStrength = candidate.strengthFromRecord || candidate.strength < 0.0f;
				item.strength = item.automaticStrength ?
					(npc ? std::clamp(npc->weight, 0.0f, 100.0f) : 0.0f) : candidate.strength;
				item.priority = candidate.priority;
				item.kind = candidate.fromFaction ? mpf::menu::OriginKind::faction :
					mpf::menu::OriginKind::direct;
				item.winner = candidate.sourcePath == effective->second.sourcePath &&
					candidate.targetText == effective->second.targetText &&
					candidate.fromFaction == effective->second.fromFaction;
				row.candidates.push_back(std::move(item));
			}
			{
				std::unordered_set<std::string> uniqueSources;
				for (const auto& candidate : row.candidates) {
					uniqueSources.insert(SourceKey(candidate.sourcePath));
				}
				row.sourceCount = uniqueSources.size();
			}
			std::stable_sort(row.candidates.begin(), row.candidates.end(), [](const auto& a, const auto& b) {
				if (a.winner != b.winner) {
					return a.winner;
				}
				if (a.priority != b.priority) {
					return a.priority > b.priority;
				}
				return a.sourceName < b.sourceName;
			});
			if (row.sourceCount > 1) {
				++snapshot->conflictCount;
				if (auto source = g_sources.find(SourceKey(effective->second.sourcePath));
					source != g_sources.end()) {
					++source->second.conflictsWon;
				}
			}
			snapshot->npcs.push_back(std::move(row));
		}
		std::sort(snapshot->npcs.begin(), snapshot->npcs.end(), [](const auto& a, const auto& b) {
			const auto left = Lowered(a.name);
			const auto right = Lowered(b.name);
			return left == right ? a.formID < b.formID : left < right;
		});

		// The edited-NPC table above deliberately remains small.  The add panel,
		// however, needs a searchable index of the whole load order so users can
		// type a display name instead of knowing an EditorID or FormID.
		if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
			for (auto* npc : dataHandler->GetFormArray<RE::TESNPC>()) {
				if (!npc || IsChildNPC(npc)) {
					continue;
				}
				const auto* name = npc->GetName();
				if (!name || !*name) {
					continue;
				}
				auto stableID = StableFormText(npc);
				if (stableID.empty()) {
					continue;
				}
				snapshot->npcChoices.push_back({ npc->GetFormID(), std::move(stableID), name });
			}
			std::sort(snapshot->npcChoices.begin(), snapshot->npcChoices.end(), [](const auto& a, const auto& b) {
				const auto left = Lowered(a.name);
				const auto right = Lowered(b.name);
				return left == right ? a.formID < b.formID : left < right;
			});
			std::unordered_map<RE::FormID, std::string> factionAliases;
			for (const auto& [alias, formID] : g_factionNames) {
				factionAliases.try_emplace(formID, alias);
			}
			for (auto* faction : dataHandler->GetFormArray<RE::TESFaction>()) {
				if (!faction) {
					continue;
				}
				const auto* name = faction->GetName();
				std::string displayName = name && *name ? name : "";
				if (const auto alias = factionAliases.find(faction->GetFormID()); alias != factionAliases.end()) {
					if (displayName.empty()) {
						displayName = alias->second;
					} else if (Lowered(displayName) != alias->second) {
						displayName += std::format(" ({})", alias->second);
					}
				}
				if (displayName.empty()) {
					continue;
				}
				auto stableID = StableFormText(faction);
				if (stableID.empty()) {
					continue;
				}
				snapshot->factionChoices.push_back({ faction->GetFormID(), std::move(stableID), std::move(displayName) });
			}
			std::sort(snapshot->factionChoices.begin(), snapshot->factionChoices.end(), [](const auto& a, const auto& b) {
				const auto left = Lowered(a.name);
				const auto right = Lowered(b.name);
				return left == right ? a.formID < b.formID : left < right;
			});
		}

		for (const auto& rule : g_groups) {
			auto* faction = RE::TESForm::LookupByID<RE::TESFaction>(rule.factionID);
			const auto* factionName = faction ? faction->GetName() : nullptr;
			mpf::menu::FactionRow row;
			row.formID = rule.factionID;
			row.stableID = StableFormText(faction);
			row.name = factionName && *factionName ? factionName : rule.text;
			row.targetKey = rule.text;
			row.physique = rule.physique;
			row.strength = rule.strength;
			row.percent = rule.percent;
			row.automaticStrength = rule.automaticStrength;
			row.randomStrength = rule.randomStrength;
			row.randomPhysique = rule.randomPhysique;
			row.sourceName = SourceDisplayName(rule.sourcePath, rule.sourceFile);
			row.sourcePath = rule.sourcePath;
			row.priority = rule.priority;
			snapshot->factions.push_back(std::move(row));
		}
		std::stable_sort(snapshot->factions.begin(), snapshot->factions.end(), [](const auto& a, const auto& b) {
			if (a.priority != b.priority) {
				return a.priority > b.priority;
			}
			const auto left = Lowered(a.name);
			const auto right = Lowered(b.name);
			return left == right ? a.sourceName < b.sourceName : left < right;
		});

		for (const auto& [key, source] : g_sources) {
			snapshot->sources.push_back({ source.path.generic_string(), source.name, source.author,
				source.priority, source.directEntries, source.factionRules, source.conflictsWon });
		}
		std::sort(snapshot->sources.begin(), snapshot->sources.end(), [](const auto& a, const auto& b) {
			return Lowered(a.name) < Lowered(b.name);
		});

		for (const auto& [key, physique] : g_physiques) {
			snapshot->physiques.push_back({ physique.name, physique.faceMorph, physique.bodyMorph,
				SourceDisplayName(physique.sourcePath, physique.sourceFile), physique.priority });
			snapshot->physiqueNames.push_back(physique.name);
		}
		std::sort(snapshot->physiques.begin(), snapshot->physiques.end(), [](const auto& a, const auto& b) {
			return Lowered(a.name) < Lowered(b.name);
		});
		std::sort(snapshot->physiqueNames.begin(), snapshot->physiqueNames.end(), [](const auto& a, const auto& b) {
			return Lowered(a) < Lowered(b);
		});
		mpf::menu::Publish(std::move(snapshot));
	}

	void RestoreOriginalRecords()
	{
		for (const auto& [formID, original] : g_originalNpcRecords) {
			if (auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID)) {
				npc->skin = original.skin;
				if (original.outfit) {
					npc->SetDefaultOutfit(original.outfit);
				} else {
					npc->defaultOutfit = nullptr;
				}
			}
		}
	}

	void ClearConfigState()
	{
		g_entries.clear();
		g_entryCandidates.clear();
		g_sources.clear();
		g_physiques.clear();
		g_groups.clear();
		g_skinMaleText.clear();
		g_skinFemaleText.clear();
		g_skinMalePriority = (std::numeric_limits<int>::min)();
		g_skinFemalePriority = (std::numeric_limits<int>::min)();
		g_skinMaleSource.clear();
		g_skinFemaleSource.clear();
		LoadRuntimeSettings();
	}

	// -----------------------------------------------------------------------
	// applying
	// -----------------------------------------------------------------------
	void VisitGeometries(RE::NiAVObject* a_obj, const std::function<void(RE::BSGeometry*)>& a_fn)
	{
		if (!a_obj) {
			return;
		}
		if (auto* geom = a_obj->AsGeometry()) {
			a_fn(geom);
			return;
		}
		if (auto* node = a_obj->AsNode()) {
			for (auto& child : node->GetChildren()) {
				VisitGeometries(child.get(), a_fn);
			}
		}
	}

	// Geometry names in a FaceGeom NIF are the head part's FULL name, which is
	// what RaceMenu matches on too. Only the actor's own parts are searched
	// (plus their extra parts), instead of every head part in the load order.
	RE::BGSHeadPart* FindHeadPart(RE::BGSHeadPart* a_part, const char* a_name, int a_depth = 0)
	{
		if (!a_part || a_depth > 3) {
			return nullptr;
		}
		const char* full = a_part->GetFullName();
		if (full && std::strcmp(full, a_name) == 0) {
			return a_part;
		}
		for (auto* extra : a_part->extraParts) {
			if (auto* found = FindHeadPart(extra, a_name, a_depth + 1)) {
				return found;
			}
		}
		return nullptr;
	}

	// Every head part in the load order, by FULL name. The NPC's own list is not
	// enough: measured on Gerdur, whose head geometry is KWA_FemaleHead while no
	// part of that name appears in her headParts, and on beard pieces like
	// HumanBeard44_C0tR. RaceMenu scans the whole catalogue for the same reason.
	std::unordered_map<std::string, RE::BGSHeadPart*> g_headPartsByName;

	void IndexHeadPart(RE::BGSHeadPart* a_part, int a_depth = 0)
	{
		if (!a_part || a_depth > 3) {
			return;
		}
		if (const char* full = a_part->GetFullName(); full && *full) {
			g_headPartsByName.emplace(full, a_part);
		}
		for (auto* extra : a_part->extraParts) {
			IndexHeadPart(extra, a_depth + 1);
		}
	}

	void BuildHeadPartIndex()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return;
		}
		for (auto* part : handler->GetFormArray<RE::BGSHeadPart>()) {
			IndexHeadPart(part);
		}
		spdlog::info("head part index: {} name(s)", g_headPartsByName.size());
	}

	RE::BGSHeadPart* FindHeadPartForGeometry(RE::TESNPC* a_base, const char* a_name)
	{
		if (!a_name) {
			return nullptr;
		}
		// The NPC's own parts first: if two mods share a FULL name, the one this
		// NPC actually wears is the right answer.
		if (a_base && a_base->headParts) {
			for (std::int8_t i = 0; i < a_base->numHeadParts; ++i) {
				if (auto* found = FindHeadPart(a_base->headParts[i], a_name)) {
					return found;
				}
			}
		}
		const auto it = g_headPartsByName.find(a_name);
		return it == g_headPartsByName.end() ? nullptr : it->second;
	}

	// Printed once per actor that fails to resolve a geometry, so the next
	// failure arrives as evidence instead of as another guess.
	void LogHeadPartsOf(RE::TESNPC* a_base)
	{
		if (!a_base || !a_base->headParts) {
			spdlog::info("      (this NPC has no head part list at all)");
			return;
		}
		std::string names;
		for (std::int8_t i = 0; i < a_base->numHeadParts; ++i) {
			auto* p = a_base->headParts[i];
			if (!p) {
				continue;
			}
			const char* full = p->GetFullName();
			names += std::format("{}{}[{}]", names.empty() ? "" : ", ",
				full && *full ? full : "<no FULL>", PartTypeName(static_cast<std::uint32_t>(p->type.get())));
		}
		spdlog::info("      NPC's own head parts: {}", names);
	}

	BSFaceGenBaseMorphExtraData* GetFOD(RE::BSGeometry* a_geom)
	{
		static const RE::BSFixedString key{ "FOD" };
		return static_cast<BSFaceGenBaseMorphExtraData*>(a_geom->GetExtraData(key));
	}

	// Some head parts carry a "FOD" whose vertices are all but coincident -
	// measured on Orgnar's BoP beards, where 727 vertices sat within 0.007 of
	// each other. Those buffers are not geometry and must not be written.
	bool BufferLooksLikeGeometry(const BSFaceGenBaseMorphExtraData* a_fod)
	{
		if (!a_fod->vertexData || a_fod->vertexCount < 3) {
			return false;
		}
		auto lo = a_fod->vertexData[0];
		auto hi = lo;
		for (std::uint32_t i = 1; i < a_fod->vertexCount; ++i) {
			const auto& v = a_fod->vertexData[i];
			lo.x = (std::min)(lo.x, v.x);
			lo.y = (std::min)(lo.y, v.y);
			lo.z = (std::min)(lo.z, v.z);
			hi.x = (std::max)(hi.x, v.x);
			hi.y = (std::max)(hi.y, v.y);
			hi.z = (std::max)(hi.z, v.z);
		}
		const float extent = (std::max)({ hi.x - lo.x, hi.y - lo.y, hi.z - lo.z });
		return extent > 0.5f;  // a real head part spans tens of units
	}

	// Mean distance between two point sets of equal size, each translated to its
	// own centroid first. Centring is not a nicety: the base buffer sits in body
	// space for some NPCs and head-local space for others - Orgnar's head is
	// centred near z=121, Delphine's near z=0 - so raw positions are not
	// comparable, while shape is.
	float CentredMeanDistance(const RE::NiPoint3* a_live, const std::vector<rfm::Vec3>& a_rest)
	{
		const auto n = a_rest.size();
		if (n == 0) {
			return 1.0e9f;
		}
		double lx = 0, ly = 0, lz = 0, rx = 0, ry = 0, rz = 0;
		for (std::size_t i = 0; i < n; ++i) {
			lx += a_live[i].x; ly += a_live[i].y; lz += a_live[i].z;
			rx += a_rest[i].x; ry += a_rest[i].y; rz += a_rest[i].z;
		}
		const auto inv = 1.0 / static_cast<double>(n);
		lx *= inv; ly *= inv; lz *= inv;
		rx *= inv; ry *= inv; rz *= inv;

		double sum = 0.0;
		for (std::size_t i = 0; i < n; ++i) {
			const double dx = (a_live[i].x - lx) - (a_rest[i].x - rx);
			const double dy = (a_live[i].y - ly) - (a_rest[i].y - ry);
			const double dz = (a_live[i].z - lz) - (a_rest[i].z - rz);
			sum += std::sqrt(dx * dx + dy * dy + dz * dz);
		}
		return static_cast<float>(sum * inv);
	}

	// Among the sets authored for this sex and vertex count, the one whose rest
	// pose actually is this mesh. Returns null when none is close enough, which
	// is the correct answer for a head topology nobody has authored for.
	const LibraryEntry* PickSet(bool a_female, const BSFaceGenBaseMorphExtraData* a_fod, float& a_outDistance)
	{
		a_outDistance = 1.0e9f;
		const auto it = g_library.find(LibraryKey{ a_female, a_fod->vertexCount });
		if (it == g_library.end()) {
			return nullptr;
		}
		const LibraryEntry* best = nullptr;
		for (const auto& entry : it->second) {
			const float d = CentredMeanDistance(a_fod->vertexData, entry.tri.BaseVerts());
			if (d < a_outDistance) {
				a_outDistance = d;
				best = &entry;
			}
		}
		return (best && a_outDistance <= kSameMeshLimit) ? best : nullptr;
	}

	// Cheap, order-sensitive summary of a buffer's contents. Only needs to change
	// when the vertices change, not to be collision-proof.
	double Fingerprint(const RE::NiPoint3* a_verts, std::uint32_t a_count)
	{
		double sum = 0.0;
		const std::uint32_t step = (a_count / 64) + 1;
		for (std::uint32_t i = 0; i < a_count; i += step) {
			sum += (a_verts[i].x * 3.0 + a_verts[i].y * 5.0 + a_verts[i].z * 7.0) * (i + 1);
		}
		return sum;
	}

	struct ApplyStats
	{
		int  geometriesTouched{ 0 };
		int  geometriesSkipped{ 0 };
		// The head can be attached a frame or two after the actor's 3D is set, so
		// "not there yet" is a reason to come back rather than a failure to report.
		bool noFaceNode{ false };
	};

	ApplyStats ApplyToActor(RE::Actor* a_actor, const NpcEntry& a_entry, const char* a_tag, bool a_dryRun)
	{
		ApplyStats stats;
		// The sweep revisits the same actors forever, so anything it would repeat
		// every three seconds is said once by the event path, or not at all.
		const bool sweeping = std::string_view(a_tag) == "sweep";
		auto*      base = a_actor->GetActorBase();
		const bool female = base && base->GetSex() == RE::SEX::kFemale;
		bool       reportedParts = false;

		auto* faceNode = a_actor->GetFaceNodeSkinned();
		if (!faceNode) {
			faceNode = a_actor->GetFaceNode();
		}
		if (!faceNode) {
			stats.noFaceNode = true;
			return stats;
		}

		VisitGeometries(faceNode, [&](RE::BSGeometry* geom) {
			auto* fod = GetFOD(geom);
			if (!fod) {
				return;  // hair and other non-morphable parts
			}
			auto*              tri = geom->AsTriShape();
			const std::uint32_t vcount = tri ? tri->GetTrishapeRuntimeData().vertexCount : 0;
			if (vcount == 0 || fod->vertexCount != vcount) {
				spdlog::warn("[{}] '{}' refused: FOD count {} vs geometry {}",
					a_tag, geom->name.c_str(), fod->vertexCount, vcount);
				++stats.geometriesSkipped;
				return;
			}

			// Cheap first: a buffer we already wrote is skipped before any
			// geometry matching runs. The sweep revisits every listed actor every
			// few seconds, and PickSet walks up to 12206 vertices per candidate,
			// so this ordering is what keeps the sweep from costing anything.
			if (!a_dryRun) {
				const auto seen = g_applied.find(fod->vertexData);
				if (seen != g_applied.end() && seen->second == Fingerprint(fod->vertexData, vcount)) {
					if (!sweeping) {
						spdlog::info("[{}] '{}' already contains the MPF face delta; skipped buffer {}",
							a_tag, geom->name.c_str(), static_cast<const void*>(fod->vertexData));
					}
					return;
				}
			}

			if (!BufferLooksLikeGeometry(fod)) {
				if (!sweeping) {
					spdlog::warn("[{}] '{}' refused: FOD contents are collapsed, not geometry",
						a_tag, geom->name.c_str());
				}
				++stats.geometriesSkipped;
				return;
			}

			// Informative only. The name is unreliable - Ordinary People's head
			// geometry has no head part of the same name anywhere - so it is
			// logged, never used to decide.
			const char* partName = "unmatched";
			if (auto* headPart = FindHeadPartForGeometry(base, geom->name.c_str())) {
				partName = PartTypeName(static_cast<std::uint32_t>(headPart->type.get()));
			} else if (!reportedParts && !sweeping) {
				reportedParts = true;
				LogHeadPartsOf(base);
			}

			float       distance = 0.0f;
			const auto* set = PickSet(female, fod, distance);
			if (!set) {
				// Not verbose-gated: this only fires for NPCs on the list, and
				// without it a skipped NPC is indistinguishable from a silent
				// failure. The distance says why: a large one means no authored
				// set is this mesh, a missing one means no set of this size.
				if (!sweeping) {
					spdlog::info("[{}] '{}' {} part={} verts={} - no morph set is this mesh "
								 "(best distance {:.3f}), left alone",
						a_tag, geom->name.c_str(), female ? "female" : "male", partName, vcount,
						distance > 1.0e8f ? -1.0f : distance);
				}
				return;
			}

			for (const auto& request : a_entry.morphs) {
				const auto* deltas = set->tri.Find(request.name);
				if (!deltas) {
					spdlog::warn("[{}] morph '{}' is not in the {} set for {} verts",
						a_tag, request.name, female ? "female" : "male", vcount);
					continue;
				}
				// Face and body morphs share one percentage. Engine weight is preserved.
				const float effective = (a_entry.strength / 100.0f) * request.scale;
				if (effective == 0.0f) {
					spdlog::info("[{}] '{}' '{}' is at strength 0, nothing to apply",
						a_tag, geom->name.c_str(), request.name);
					continue;
				}

				float maxMove = 0.0f;
				for (std::uint32_t i = 0; i < vcount; ++i) {
					const auto& d = (*deltas)[i];
					const RE::NiPoint3 move{ d.x * effective, d.y * effective, d.z * effective };
					if (!a_dryRun) {
						fod->vertexData[i] += move;
					}
					maxMove = (std::max)(maxMove, move.Length());
				}
				// The buffer address is logged because a shared head between two
				// references of the same base NPC shows up here as the same
				// address twice, and nothing else in the log would reveal it.
				spdlog::info("[{}] '{}' {} '{}' x{:.3f} (strength {:.1f} from the {}) over {} verts "
							 "from '{}' (mesh distance {:.3f}), maxMove={:.4f}, buffer {}",
					a_tag, geom->name.c_str(), a_dryRun ? "would apply" : "applied",
					request.name, effective, a_entry.strength,
					a_entry.strengthFromRecord ? "original record" : "list", vcount,
					set->file, distance, maxMove,
					static_cast<const void*>(fod->vertexData));
				++stats.geometriesTouched;
			}
			if (!a_dryRun) {
				g_applied[fod->vertexData] = Fingerprint(fod->vertexData, vcount);
			}
		});
		return stats;
	}

	// Is this actor on the list? Asked in three places - the event, the sweep and
	// HandleActor - and they must agree, or an actor gets past one gate and is
	// turned away at the next. Both bases count: GetActorBase resolves templates
	// and answers with the inherited record, while the reference's own base is the
	// form the list names.
	// A form the game invented at runtime rather than one a plugin defines.
	bool IsDynamic(RE::FormID a_id) { return (a_id >> 24) == 0xFF; }

	// The record the list actually names, for an actor whose own base is dynamic.
	//
	// Measured on 'Skjorni the Reveler': placeatme on a templated NPC does not use
	// WERJ06Drunk03 as the base at all - it fabricates a new TESNPC in FF space, and
	// GetActorBase and GetBaseObject both answer with THAT. The form the list names
	// is only reachable by following the template the copy was made from.
	RE::TESNPC* ListedThroughTemplate(RE::TESNPC* a_base, int a_depth = 0)
	{
		if (!a_base || a_depth > 4) {
			return nullptr;
		}
		if (!IsDynamic(a_base->GetFormID()) && g_entries.contains(a_base->GetFormID())) {
			return a_base;
		}
		auto* next = a_base->baseTemplateForm ? a_base->baseTemplateForm->As<RE::TESNPC>() : nullptr;
		return next == a_base ? nullptr : ListedThroughTemplate(next, a_depth + 1);
	}

	// Where the game itself records what a runtime copy was made from.
	//
	// Three earlier attempts on 'Skjorni the Reveler' failed because they asked the
	// BASE: GetActorBase, GetBaseObject and the template chain all answer with the
	// dynamic form the copy IS - measured, the chain is one link long and stops at
	// 0xFF001ADE. The origin is not kept on the base at all; it is on the
	// REFERENCE, in ExtraLeveledCreature, which holds both the form that was placed
	// and the template it resolved through.
	RE::TESNPC* ListedThroughLeveledExtra(RE::Actor* a_actor)
	{
		auto* extra = a_actor->extraList.GetByType<RE::ExtraLeveledCreature>();
		if (!extra) {
			return nullptr;
		}
		for (auto* candidate : { extra->originalBase, extra->templateBase }) {
			if (candidate && g_entries.contains(candidate->GetFormID())) {
				return candidate->As<RE::TESNPC>();
			}
		}
		return nullptr;
	}

	RE::TESNPC* ListedBase(RE::Actor* a_actor)
	{
		if (auto* base = a_actor->GetActorBase(); base && g_entries.contains(base->GetFormID())) {
			return base;
		}
		if (auto* placed = a_actor->GetBaseObject(); placed && g_entries.contains(placed->GetFormID())) {
			return placed->As<RE::TESNPC>();
		}
		if (auto* fromExtra = ListedThroughLeveledExtra(a_actor)) {
			return fromExtra;
		}
		return ListedThroughTemplate(a_actor->GetActorBase());
	}

	bool HasBodyMorphMappings()
	{
		return std::ranges::any_of(g_entries, [](const auto& item) {
			return !item.second.bodyMorph.empty();
		});
	}

	void VisitObjects(RE::NiAVObject* a_obj, const std::function<void(RE::NiAVObject*)>& a_fn)
	{
		if (!a_obj) {
			return;
		}
		a_fn(a_obj);
		if (auto* node = a_obj->AsNode()) {
			for (auto& child : node->GetChildren()) {
				VisitObjects(child.get(), a_fn);
			}
		}
	}

	class BodyMorphLogVisitor final : public SKEE::IBodyMorphInterface::MorphValueVisitor
	{
	public:
		BodyMorphLogVisitor(const char* a_tag, const char* a_phase) :
			tag(a_tag), phase(a_phase)
		{}

		void Visit(RE::TESObjectREFR*, const char* a_morph, const char* a_key,
			float a_value) override
		{
			if (std::abs(a_value) <= 0.0001f) {
				return;
			}
			++count;
			spdlog::info("[{}] body-state {}: morph '{}' key '{}' x{:.3f}", tag,
				phase, a_morph ? a_morph : "<null>", a_key ? a_key : "<null>",
				a_value);
		}

		std::size_t count{ 0 };

	private:
		const char* tag;
		const char* phase;
	};

	void LogBodyMorphState(RE::Actor* a_actor, const char* a_tag, const char* a_phase)
	{
		if (!a_actor || !g_bodyMorphAPI) {
			return;
		}
		BodyMorphLogVisitor visitor(a_tag, a_phase);
		g_bodyMorphAPI->VisitMorphValues(a_actor, visitor);
		if (visitor.count == 0) {
			spdlog::info("[{}] body-state {}: no active RaceMenu morph values", a_tag,
				a_phase);
		}
	}

	std::optional<std::filesystem::path> ResolveLiveBodyTri(std::string a_value,
		std::string& a_error)
	{
		a_error.clear();
		Trim(a_value);
		if (a_value.empty()) {
			a_error = "empty BODYTRI value";
			return std::nullopt;
		}

		std::filesystem::path relative(a_value);
		const bool escapesMeshes = relative.is_absolute() || relative.has_root_path() ||
			std::ranges::any_of(relative, [](const auto& component) { return component == ".."; });
		if (escapesMeshes) {
			a_error = "BODYTRI is not a relative meshes path";
			return std::nullopt;
		}
		relative = relative.lexically_normal();

		std::filesystem::path path("Data");
		auto first = relative.begin();
		if (first != relative.end() && Lowered(first->string()) == "meshes") {
			path /= relative;
		} else {
			path /= "meshes";
			path /= relative;
		}
		return path.lexically_normal();
	}

	std::string JoinShapeNames(const std::vector<std::string>& a_names)
	{
		constexpr std::size_t kMaxNames = 8;
		std::string result;
		for (std::size_t i = 0; i < a_names.size() && i < kMaxNames; ++i) {
			result += std::format("{}'{}'", result.empty() ? "" : ", ", a_names[i]);
		}
		if (a_names.size() > kMaxNames) {
			result += std::format(", and {} more", a_names.size() - kMaxNames);
		}
		return result;
	}

	void ReportBodyDiagnostics(RE::Actor* a_actor, const NpcEntry& a_entry, const char* a_tag)
	{
		if (!a_actor) {
			return;
		}

		const float expected = std::clamp(a_entry.strength / 100.0f, 0.0f, 1.0f);
		const auto* runtimeBase = a_actor->GetActorBase();
		spdlog::info("[{}] body diagnostic: physique='{}' strength={:.1f} ({}) "
			"engineWeight={:.1f} morph='{}' expected=x{:.3f}",
			a_tag, a_entry.physique, a_entry.strength,
			a_entry.strengthFromRecord ? "original record" : "list",
			runtimeBase ? runtimeBase->weight : -1.0f,
			a_entry.bodyMorph.empty() ? "<face-only>" : a_entry.bodyMorph, expected);

		if (a_entry.bodyMorph.empty()) {
			spdlog::info("[{}] body diagnostic result: face-only physique; no body asset check needed",
				a_tag);
			return;
		}

		if (!g_bodyMorphAPI) {
			spdlog::warn("[{}] body diagnostic result: RaceMenu/SKEE BodyMorph interface is unavailable",
				a_tag);
		} else {
			const bool hasPair = g_bodyMorphAPI->HasBodyMorph(a_actor,
				a_entry.bodyMorph.c_str(), kBodyMorphKey.data());
			const bool hasName = g_bodyMorphAPI->HasBodyMorphName(a_actor,
				a_entry.bodyMorph.c_str());
			const bool hasKey = g_bodyMorphAPI->HasBodyMorphKey(a_actor, kBodyMorphKey.data());
			const float owned = g_bodyMorphAPI->GetMorph(a_actor,
				a_entry.bodyMorph.c_str(), kBodyMorphKey.data());
			const float combined = g_bodyMorphAPI->GetBodyMorphs(a_actor,
				a_entry.bodyMorph.c_str());
			spdlog::info("[{}] SKEE state: MPF pair={} MPF key={} morph name={} "
				"owned=x{:.3f} combined-all-keys=x{:.3f} submitted-this-3D={}",
				a_tag, hasPair ? "yes" : "no", hasKey ? "yes" : "no",
				hasName ? "yes" : "no", owned, combined,
				g_bodyApplied.contains(a_actor->GetFormID()) ? "yes" : "no");
			if (expected > 0.0001f && (!hasPair || std::abs(owned - expected) > 0.001f)) {
				spdlog::warn("[{}] SKEE state mismatch: expected MPF '{}' x{:.3f}, stored x{:.3f}",
					a_tag, a_entry.bodyMorph, expected, owned);
			}
		}

		std::vector<std::string> wornBodyPieces;
		for (const auto& [item, data] : a_actor->GetInventory([](RE::TESBoundObject& a_object) {
				 return a_object.IsArmor();
			 })) {
			auto* armor = item->As<RE::TESObjectARMO>();
			if (!armor || data.first <= 0 || !data.second || !data.second->IsWorn() ||
				!armor->bipedModelData.bipedObjectSlots.any(
					RE::BIPED_MODEL::BipedObjectSlot::kBody)) {
				continue;
			}
			const auto* armorName = armor->GetName();
			wornBodyPieces.push_back(std::format("0x{:08X} '{}'", armor->GetFormID(),
				armorName && *armorName ? armorName : "<unnamed>"));
		}
		spdlog::info("[{}] worn slot-32 pieces: {}{}", a_tag, wornBodyPieces.size(),
			wornBodyPieces.empty() ? "" : std::format(" ({})", JoinShapeNames(wornBodyPieces)));

		std::unordered_set<std::string> bodyTriValues;
		std::size_t geometryCount = 0;
		std::size_t shapeDataCount = 0;
		static const RE::BSFixedString bodyTriKey{ "BODYTRI" };
		static const RE::BSFixedString shapeDataKey{ "SHAPEDATA" };
		VisitObjects(a_actor->Get3D(false), [&](RE::NiAVObject* a_object) {
			if (auto* bodyTri = a_object->GetExtraData<RE::NiStringExtraData>(bodyTriKey);
				bodyTri && bodyTri->value && *bodyTri->value) {
				bodyTriValues.insert(bodyTri->value);
			}
			if (auto* geometry = a_object->AsGeometry()) {
				++geometryCount;
				if (geometry->GetExtraData(shapeDataKey)) {
					++shapeDataCount;
				}
			}
		});
		spdlog::info("[{}] live 3D: geometries={} BODYTRI assets={} SHAPEDATA geometries={}",
			a_tag, geometryCount, bodyTriValues.size(), shapeDataCount);

		if (bodyTriValues.empty()) {
			spdlog::warn("[{}] body diagnostic result: live body/outfit exposes no BODYTRI; "
				"RaceMenu has no compatible morph asset to inspect", a_tag);
			return;
		}

		bool anyAssetContainsMorph = false;
		for (const auto& bodyTriValue : bodyTriValues) {
			std::string pathError;
			const auto path = ResolveLiveBodyTri(bodyTriValue, pathError);
			if (!path) {
				spdlog::warn("[{}] BODYTRI '{}' refused: {}", a_tag, bodyTriValue, pathError);
				continue;
			}
			const auto cacheKey = Lowered(path->generic_string());
			auto [cached, inserted] = g_bodyTriDiagnostics.try_emplace(cacheKey);
			if (inserted) {
				cached->second.loaded = cached->second.index.Load(*path, cached->second.error);
			}

			if (!cached->second.loaded) {
				spdlog::warn("[{}] BODYTRI '{}' could not be indexed: {}", a_tag,
					path->generic_string(), cached->second.error);
				continue;
			}

			const auto& shapes = cached->second.index.Shapes();
			const auto withMorph = cached->second.index.CountShapesWith(a_entry.bodyMorph);
			anyAssetContainsMorph = anyAssetContainsMorph || withMorph > 0;
			std::vector<std::string> missing;
			for (const auto& shape : shapes) {
				if (!shape.morphs.contains(a_entry.bodyMorph)) {
					missing.push_back(shape.name);
				}
			}

			if (withMorph == 0) {
				spdlog::warn("[{}] BODYTRI '{}' has morph '{}' on 0/{} shapes",
					a_tag, path->generic_string(), a_entry.bodyMorph, shapes.size());
			} else {
				spdlog::info("[{}] BODYTRI '{}' has morph '{}' on {}/{} shapes{}",
					a_tag, path->generic_string(), a_entry.bodyMorph, withMorph, shapes.size(),
					missing.empty() ? "" : std::format("; missing: {}", JoinShapeNames(missing)));
			}
		}

		if (!anyAssetContainsMorph) {
			spdlog::warn("[{}] body diagnostic result: '{}' is stored on the actor but absent "
				"from every readable live BODYTRI", a_tag, a_entry.bodyMorph);
		} else {
			spdlog::info("[{}] body diagnostic result: at least one live BODYTRI contains '{}'",
				a_tag, a_entry.bodyMorph);
		}
	}

	bool ClearOwnedBodyMorph(RE::Actor* a_actor, const char* a_tag)
	{
		if (!a_actor) {
			return false;
		}
		const auto referenceID = a_actor->GetFormID();
		const bool tracked = g_bodyApplied.erase(referenceID) != 0;
		const bool hasKey = g_bodyMorphAPI &&
			g_bodyMorphAPI->HasBodyMorphKey(a_actor, kBodyMorphKey.data());
		if (!hasKey) {
			return tracked;
		}
		g_bodyMorphAPI->ClearBodyMorphKeys(a_actor, kBodyMorphKey.data());
		g_bodyMorphAPI->ApplyBodyMorphs(a_actor, true);
		spdlog::info("[{}] '{}' ref=0x{:08X} cleared stale body morph key '{}'",
			a_tag, a_actor->GetName(), referenceID, kBodyMorphKey);
		return true;
	}

	void ApplyBodyMorph(RE::Actor* a_actor, const NpcEntry& a_entry, const char* a_tag,
		bool a_dryRun)
	{
		if (!a_actor) {
			return;
		}

		const float relative = std::clamp(a_entry.strength / 100.0f, 0.0f, 1.0f);
		const auto referenceID = a_actor->GetFormID();
		const auto generation = g_loadGeneration.load(std::memory_order_acquire);
		if (const auto state = g_bodyApplied.find(referenceID); state != g_bodyApplied.end() &&
			state->second.baseFormID == a_entry.baseFormID &&
			state->second.morph == a_entry.bodyMorph &&
			std::abs(state->second.strength - relative) <= 0.0001f &&
			state->second.generation == generation) {
			return;
		}
		if (a_dryRun) {
			ReportBodyDiagnostics(a_actor, a_entry, a_tag);
			return;
		}
		if (!g_bodyMorphAPI) {
			return;
		}
		if (a_entry.bodyMorph.empty()) {
			ClearOwnedBodyMorph(a_actor, a_tag);
			g_bodyApplied[referenceID] = BodyApplyState{
				a_entry.baseFormID, {}, relative, true, generation };
			return;
		}

		// Validate while the current RaceMenu state is still visible. Clearing the
		// MPF key first would make HasBodyMorph/HasBodyMorphName report false for a
		// morph that was already known to this actor, turning a valid reapplication
		// into a false compatibility failure.
		const bool hasMorph = g_bodyMorphAPI->HasBodyMorph(
			a_actor, a_entry.bodyMorph.c_str(), kBodyMorphKey.data()) ||
			g_bodyMorphAPI->HasBodyMorphName(a_actor, a_entry.bodyMorph.c_str());
		// MPF owns only its own key. Other RaceMenu morph consumers are never
		// inspected or modified. ApplyBodyMorphs rebuilds each shape from RaceMenu's
		// saved SHAPEDATA before evaluating all active keys, so there is no cumulative
		// deformation. Clear our old target before every target change so a reload or
		// an edited INI cannot leave two MPF values active at once.
		LogBodyMorphState(a_actor, a_tag, "before MPF update");
		g_bodyMorphAPI->ClearBodyMorphKeys(a_actor, kBodyMorphKey.data());
		if (!hasMorph) {
			// A missing body TRI/morph is a compatibility failure, not a reason to
			// call SetMorph with an invented name. The cleared key removes stale MPF
			// data while leaving other body-morph owners untouched.
			g_bodyMorphAPI->ApplyBodyMorphs(a_actor, true);
			g_bodyApplied[referenceID] = BodyApplyState{
				a_entry.baseFormID, a_entry.bodyMorph, relative, false, generation };
			spdlog::warn("[{}] '{}' ref=0x{:08X} body morph '{}' is unavailable on the "
				"current body/outfit; MPF body application skipped",
				a_tag, a_actor->GetName(), referenceID, a_entry.bodyMorph);
			return;
		}
		if (relative > 0.0f) {
			g_bodyMorphAPI->SetMorph(a_actor, a_entry.bodyMorph.c_str(),
				kBodyMorphKey.data(), relative);
		}
		LogBodyMorphState(a_actor, a_tag, "after MPF update");
		g_bodyMorphAPI->ApplyBodyMorphs(a_actor, true);
		g_bodyApplied[referenceID] = BodyApplyState{
			a_entry.baseFormID, a_entry.bodyMorph, relative, true, generation };
		const auto* runtimeBase = a_actor->GetActorBase();
		spdlog::info("[{}] '{}' ref=0x{:08X} preserved weight {:.1f}; body morph '{}' "
			"x{:.3f} under key '{}'", a_tag, a_actor->GetName(), referenceID,
			runtimeBase ? runtimeBase->weight : -1.0f, a_entry.bodyMorph, relative,
			kBodyMorphKey);
	}

	void QueueForHead(RE::Actor* a_actor, bool a_resetBody = true);

	void ScheduleBodyMorphsForLoadedActors(bool a_reset)
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddTask([a_reset]() {
			if (a_reset) {
				g_bodyApplied.clear();
			}
			if (!g_dataLoaded.load(std::memory_order_acquire) || !g_bodyMorphAPI) {
				return;
			}
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			for (auto& handle : lists->highActorHandles) {
				auto actor = handle.get();
				if (!actor) {
					continue;
				}
				auto* listed = ListedBase(actor.get());
				if (!listed) {
					ClearOwnedBodyMorph(actor.get(), "body-ready");
					continue;
				}
				if (const auto entry = g_entries.find(listed->GetFormID()); entry != g_entries.end()) {
					QueueForHead(actor.get());
				} else {
					ClearOwnedBodyMorph(actor.get(), "body-ready");
				}
			}
		});
	}

	ApplyStats HandleActor(RE::Actor* a_actor, const char* a_tag, bool a_dryRun)
	{
		ApplyStats stats;
		auto* base = a_actor->GetActorBase();
		if (!base) {
			return stats;
		}
		if (auto* listed = ListedBase(a_actor); listed && listed != base) {
			spdlog::info("[{}] '{}' ref=0x{:08X} is 0x{:08X} on the list; its own base "
				"0x{:08X} was made at runtime from a template",
				a_tag, a_actor->GetName(), a_actor->GetFormID(), listed->GetFormID(),
				base->GetFormID());
			base = listed;
		}
		if (IsChildNPC(base)) {
			if (!a_dryRun) {
				ClearOwnedBodyMorph(a_actor, a_tag);
			}
			if (a_dryRun || g_diagnostics) {
				spdlog::info("[{}] '{}' base=0x{:08X} is a child - refused", a_tag,
					 a_actor->GetName(), base->GetFormID());
			}
			return stats;
		}
		const auto it = g_entries.find(base->GetFormID());
		if (it == g_entries.end()) {
			if (!a_dryRun) {
				ClearOwnedBodyMorph(a_actor, a_tag);
			}
			if (a_dryRun || g_verbose) {
				spdlog::info("[{}] '{}' base=0x{:08X} is not on the list", a_tag, a_actor->GetName(), base->GetFormID());
			}
			return stats;
		}
		// The sweep runs constantly, so it only announces an actor when it
		// actually did something; the event path always announces, because there
		// the arrival itself is the information.
		// The queue revisits an actor until the head exists, so the arrival is
		// announced once by the path that finally did the work, not per attempt.
		const bool quiet = std::string_view(a_tag) == "sweep" ||
						   g_announced.contains(a_actor->GetFormID());
		g_announced.insert(a_actor->GetFormID());
		if (!quiet) {
			spdlog::info("[{}] '{}' base=0x{:08X} from '{}'", a_tag, a_actor->GetName(),
				base->GetFormID(), it->second.sourceFile);
		}

		ApplyBodyMorph(a_actor, it->second, a_tag, a_dryRun);
		// Dress the actor itself, once. The base record was patched at
		// kDataLoaded, which settles anybody the game builds afterwards; an actor
		// the save already contained needs telling directly.
		if (!a_dryRun && it->second.outfitFormID != 0 && !g_dressed.contains(a_actor->GetFormID())) {
			if (auto* outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(it->second.outfitFormID)) {
				const bool changed = a_actor->SetDefaultOutfit(outfit, true);
				g_dressed.insert(a_actor->GetFormID());

				// "The body looks past 100%" has been reported three times, and the
				// files rule out the obvious causes: body morph strength is clamped to
				// 100%; nothing but heads, beards and brows receives a face morph; and
				// the body baked into each outfit matches the one the skin applies to
				// within 2%. What no file can show is a second torso equipped on top
				// of the first, which would thicken the silhouette without anything
				// exceeding a slider. So it is counted here.

				spdlog::info("[{}] '{}' ref=0x{:08X} outfit 0x{:08X} - {}", a_tag,
					a_actor->GetName(), a_actor->GetFormID(), it->second.outfitFormID,
					changed ? "changed on the actor" : "already worn, record was enough");
			}
		}

		// Counted on a LATER visit, never in the same breath as the dressing: the
		// game equips after the call returns, so counting immediately reported zero
		// for Corpulus Vinius - the one actor whose outfit had actually changed.
		if (!a_dryRun && g_dressed.contains(a_actor->GetFormID()) &&
			g_counted.insert(a_actor->GetFormID()).second) {
			int torsos = 0;
			std::string worn;
			for (const auto& [item, data] : a_actor->GetInventory([](RE::TESBoundObject& o) {
					 return o.IsArmor();
				 })) {
				auto* armor = item->As<RE::TESObjectARMO>();
				if (!armor || data.first <= 0 || !data.second || !data.second->IsWorn()) {
					continue;
				}
				if (armor->bipedModelData.bipedObjectSlots.any(
						RE::BIPED_MODEL::BipedObjectSlot::kBody)) {
					++torsos;
					worn += std::format(" 0x{:08X}", armor->GetFormID());
				}
			}
			if (torsos > 1) {
				spdlog::warn("[{}] '{}' is wearing {} body pieces at once:{}", a_tag,
					a_actor->GetName(), torsos, worn);
			} else if (g_diagnostics) {
				spdlog::info("[{}] '{}' body pieces worn: {}{}", a_tag, a_actor->GetName(),
					torsos, worn);
			}
		}

		stats = ApplyToActor(a_actor, it->second, a_tag, a_dryRun);
		if (quiet && stats.geometriesTouched > 0) {
			// The reference id matters, not just the base: a console-spawned copy
			// starts with FF, the one placed in the world does not. Writing to the
			// original while a copy stands in front of you looks exactly like the
			// morph having no effect.
			spdlog::info("[{}] '{}' ref=0x{:08X} ({}) base=0x{:08X} - the 3D-load event never arrived",
				a_tag, a_actor->GetName(), a_actor->GetFormID(),
				(a_actor->GetFormID() >> 24) == 0xFF ? "spawned copy" : "placed in the world",
				base->GetFormID());
		}
		return stats;
	}

	RE::Actor* NearestActor()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!player || !lists) {
			return nullptr;
		}
		const auto origin = player->GetPosition();
		RE::Actor* best = nullptr;
		float      bestDist = 1.0e9f;
		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			if (!actor || actor.get() == player) {
				continue;
			}
			const float d = actor->GetPosition().GetDistance(origin);
			if (d < bestDist) {
				bestDist = d;
				best = actor.get();
			}
		}
		return best;
	}

	// -----------------------------------------------------------------------
	// events
	// -----------------------------------------------------------------------
	class InputSink : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static InputSink* GetSingleton()
		{
			static InputSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event,
			RE::BSTEventSource<RE::InputEvent*>*) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			for (auto* e = *a_event; e; e = e->next) {
				auto* button = e->AsButtonEvent();
				if (!button) {
					continue;
				}
				const auto code = button->GetIDCode();
				if (button->device == RE::INPUT_DEVICE::kKeyboard &&
					code == RE::BSKeyboardDevice::Key::kLeftShift) {
					_leftShiftDown = button->IsPressed();
					continue;
				}
				if (button->device == RE::INPUT_DEVICE::kKeyboard &&
					code == RE::BSKeyboardDevice::Key::kRightShift) {
					_rightShiftDown = button->IsPressed();
					continue;
				}
				if (!button->IsDown()) {
					continue;
				}
				if (code != kKeyReport && code != kKeyApply) {
					continue;
				}
				if (code == kKeyReport &&
					(button->device != RE::INPUT_DEVICE::kKeyboard ||
						(!_leftShiftDown && !_rightShiftDown))) {
					continue;
				}
				auto* actor = NearestActor();
				if (!actor) {
					spdlog::warn("no actor nearby");
					continue;
				}
				HandleActor(actor, code == kKeyReport ? "Shift+F7-report" : "F6-apply", code == kKeyReport);
			}
			return RE::BSEventNotifyControl::kContinue;
		}

	private:
		bool _leftShiftDown{ false };
		bool _rightShiftDown{ false };
	};

	// Where the morph is applied from, and why it is a hook rather than a listener.
	//
	// The face lives in a vertex buffer we write into, and that write does not
	// survive the head being rebuilt - so the question is not "has this NPC appeared
	// in this save" (body morph values are stored by SKEE), but "has this head just
	// been built", every time. Character::Load3D is
	// the game's own answer: it is the call that builds an actor's 3D, so it runs on
	// the engine's code path rather than as a notification the engine may not send.
	//
	// Measured, not assumed: Set3D was hooked first and fired ZERO times in a run
	// where the old listener fired 49. Both were then hooked and instrumented, and
	// the log named Load3D. The listener it replaced was caught lying - 16 events
	// for 9 spawned NPCs, two producing none - and the three-second sweep existed
	// only to cover for that.
	//
	// The vtable is resolved per runtime by CommonLibSSE-NG, so this needs no
	// address from the Address Library, which is the fragile part of hooking Skyrim.

	// The head is attached a moment AFTER Load3D returns, so an actor usually has to
	// be revisited. The first attempt at that used the task interface to "try again
	// next frame", and the log showed all twelve retries landing in the same
	// millisecond: a task queued from inside a task is drained in the same pass, so
	// the retries burned instantly and gave up before the head existed. Hence a
	// queue with a real wait.
	//
	// It sleeps when empty. Nothing runs while no head is pending, which is the
	// whole reason the permanent sweep went away.
	struct Pending
	{
		RE::ActorHandle handle;
		RE::FormID      referenceID{ 0 };
		int             attempts{ 0 };
		bool            resetBody{ true };
		std::uint64_t   generation{ 0 };
	};

	std::mutex               g_pendingLock;
	std::condition_variable  g_pendingCv;
	// One pending item per reference. Load3D can fire more than once while a
	// head is being attached; keeping a vector here used to schedule duplicate
	// retries for the same actor and made the body reset/apply race harder to
	// reason about.
	std::unordered_map<RE::FormID, Pending> g_pending;
	std::atomic_bool         g_waiterRunning{ false };

	constexpr int kMaxHeadWaitTries = 20;      // 20 x 250ms = five seconds, then give up
	constexpr auto kHeadWaitStep = std::chrono::milliseconds(250);

	void StartHeadWaiter()
	{
		if (g_waiterRunning.exchange(true)) {
			return;
		}
		std::thread([]() {
			while (true) {
				std::vector<Pending> batch;
				{
					std::unique_lock lock(g_pendingLock);
					g_pendingCv.wait(lock, [] { return !g_pending.empty(); });
					batch.reserve(g_pending.size());
					for (auto& [referenceID, item] : g_pending) {
						static_cast<void>(referenceID);
						batch.push_back(std::move(item));
					}
					g_pending.clear();
				}
				std::this_thread::sleep_for(kHeadWaitStep);
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					std::lock_guard lock(g_pendingLock);
					for (auto& item : batch) {
						auto& slot = g_pending[item.referenceID];
						if (slot.referenceID == 0 || item.generation >= slot.generation) {
							slot = std::move(item);
						} else {
							slot.resetBody = slot.resetBody || item.resetBody;
						}
					}
					g_pendingCv.notify_one();
					continue;
				}
				task->AddTask([batch]() mutable {
					std::vector<Pending> again;
					for (auto& item : batch) {
						if (item.generation != g_loadGeneration.load(std::memory_order_acquire)) {
							continue;
						}
						auto actor = item.handle.get();
						if (!actor) {
							continue;
						}
						if (item.resetBody) {
							g_bodyApplied.erase(actor->GetFormID());
							item.resetBody = false;
						}
						const auto stats = HandleActor(actor.get(), "3d", false);
						if (!stats.noFaceNode) {
							continue;
						}
						if (++item.attempts < kMaxHeadWaitTries) {
							again.push_back(item);
						} else if (g_diagnostics) {
							spdlog::info("[3d] '{}' never grew a face node - gave up after {}",
								actor->GetName(), kMaxHeadWaitTries);
						}
					}
					if (!again.empty()) {
						std::lock_guard lock(g_pendingLock);
						for (auto& item : again) {
							auto& slot = g_pending[item.referenceID];
							if (slot.referenceID == 0 || item.generation >= slot.generation) {
								slot = std::move(item);
							} else {
								slot.resetBody = slot.resetBody || item.resetBody;
							}
						}
						g_pendingCv.notify_one();
					}
				});
			}
		}).detach();
	}

	void QueueForHead(RE::Actor* a_actor, bool a_resetBody)
	{
		if (!a_actor) {
			return;
		}
		const auto referenceID = a_actor->GetFormID();
		{
			std::lock_guard lock(g_pendingLock);
			auto& item = g_pending[referenceID];
			if (item.referenceID == 0) {
				item = Pending{
					a_actor->CreateRefHandle(), referenceID, 0, a_resetBody,
					g_loadGeneration.load(std::memory_order_acquire) };
			} else {
				item.handle = a_actor->CreateRefHandle();
				item.resetBody = item.resetBody || a_resetBody;
				item.generation = g_loadGeneration.load(std::memory_order_acquire);
				item.attempts = 0;
			}
		}
		g_pendingCv.notify_one();
	}

	void QueueLoadedActorsForHeads()
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddTask([]() {
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists) {
				return;
			}
			for (auto& handle : lists->highActorHandles) {
				if (auto actor = handle.get()) {
					QueueForHead(actor.get());
				}
			}
		});
	}

	struct Load3DHook
	{
		static RE::NiAVObject* thunk(RE::TESObjectREFR* a_this, bool a_background)
		{
			auto* result = func(a_this, a_background);
			auto* actor = a_this ? a_this->As<RE::Actor>() : nullptr;
			// Queue every actor. This is a one-shot check per 3D build, not polling,
			// and it is necessary to remove an MPF key persisted in the RaceMenu
			// co-save after its NPC was deleted from the INI. It also avoids calling
			// the SKEE interface from a background Load3D invocation. Listed actors
			// whose bases were invented at runtime are resolved once the task runs.
			if (actor) {
				QueueForHead(actor);
			}
			return result;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallHook()
	{
		REL::Relocation<std::uintptr_t> vtbl{ RE::Character::VTABLE[0] };
		Load3DHook::func = vtbl.write_vfunc(0x6A, Load3DHook::thunk);
		StartHeadWaiter();
		spdlog::info("hooked Character::Load3D - applied when the game builds a head, "
			"with no polling at rest");
	}

	void RequestBodyMorphAPI()
	{
		SKEE::InterfaceExchangeMessage message{};
		SKSE::GetMessagingInterface()->Dispatch(
			SKEE::InterfaceExchangeMessage::kExchangeInterface,
			&message, sizeof(message), "skee");
		if (message.interfaceMap) {
			g_bodyMorphAPI = static_cast<SKEE::IBodyMorphInterface*>(
				message.interfaceMap->QueryInterface("BodyMorph"));
		}
		if (g_bodyMorphAPI) {
			spdlog::info("RaceMenu/SKEE BodyMorph interface found (version {})",
				g_bodyMorphAPI->GetVersion());
			ScheduleBodyMorphsForLoadedActors(true);
		} else {
			spdlog::warn("RaceMenu/SKEE BodyMorph interface not found; body morphing is unavailable");
		}
		PublishMenuSnapshot();
	}

	void ResetRuntimeTracking(bool a_invalidatePending, bool a_clearFaceBuffers = true)
	{
		if (a_invalidatePending) {
			g_loadGeneration.fetch_add(1, std::memory_order_acq_rel);
			std::lock_guard lock(g_pendingLock);
			g_pending.clear();
		}
		// A save transition does not guarantee that FaceGen rebuilt its FOD buffers.
		// In practice Skyrim can keep the same, already-morphed allocation alive
		// across PreLoadGame/PostLoadGame (and even NewGame). Forgetting it here made
		// the additive face delta run again on the same vertices. Keep the pointer +
		// fingerprint entries for those transitions: a genuinely rebuilt or recycled
		// buffer has different contents and will still be applied normally.
		if (a_clearFaceBuffers) {
			g_applied.clear();
		} else {
			spdlog::info("preserved {} applied face-buffer fingerprint(s) across save transition",
				g_applied.size());
		}
		g_dressed.clear();
		g_counted.clear();
		g_announced.clear();
		g_bodyApplied.clear();
	}

	bool IsEditableConfigPath(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto root = std::filesystem::absolute(ConfigPath(), ec).lexically_normal();
		if (ec) {
			return false;
		}
		const auto target = std::filesystem::absolute(a_path, ec).lexically_normal();
		if (ec) {
			return false;
		}
		auto rootIt = root.begin();
		auto targetIt = target.begin();
		for (; rootIt != root.end(); ++rootIt, ++targetIt) {
			if (targetIt == target.end() || Lowered(rootIt->string()) != Lowered(targetIt->string())) {
				return false;
			}
		}
		return true;
	}

	std::size_t RefreshLoadedActors(RE::FormID a_onlyBase = 0)
	{
		auto* lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return 0;
		}
		std::size_t refreshed = 0;
		for (auto& handle : lists->highActorHandles) {
			auto actor = handle.get();
			if (!actor) {
				continue;
			}
			if (a_onlyBase != 0) {
				const auto* base = actor->GetActorBase();
				if (!base || base->GetFormID() != a_onlyBase) {
					continue;
				}
			}
			ClearOwnedBodyMorph(actor.get(), "menu-refresh");
			if (auto* base = actor->GetActorBase(); base &&
				g_originalNpcRecords.contains(base->GetFormID())) {
				actor->SetDefaultOutfit(base->defaultOutfit, true);
			}
			actor->DoReset3D(false);
			++refreshed;
		}
		return refreshed;
	}

	void ReloadConfigurationFromDisk(bool a_refreshActors, RE::FormID a_onlyBase = 0)
	{
		RestoreOriginalRecords();
		ClearConfigState();
		LoadPatches();
		ExpandGroupRules();
		ResolvePhysiques();
		ApplyRecordChanges();
		ResetRuntimeTracking(true);
		const auto refreshed = a_refreshActors ? RefreshLoadedActors(a_onlyBase) : 0;
		g_menuStatus = std::format("Reloaded {} effective NPCs from {} INI files; refreshed {} loaded actors",
			g_entries.size(), g_sources.size(), refreshed);
		spdlog::info("[menu] {}", g_menuStatus);
		PublishMenuSnapshot();
	}

	bool WriteNpcEntry(const mpf::menu::Action& a_action, bool a_remove)
	{
		const std::filesystem::path path(a_action.sourcePath);
		if (!IsEditableConfigPath(path)) {
			g_menuStatus = "Refused an INI path outside Data/ModernPhysiqueFramework";
			return false;
		}
		std::string error;
		std::optional<std::string> value;
		if (!a_remove) {
			const auto strength = a_action.automaticStrength ? std::string("auto") :
				std::format("{:g}", std::clamp(a_action.strength, 0.0f, 100.0f));
			value = std::format("{}, {}", strength, a_action.physique);
		}
		if (!mpf::ini::SetValue(path, "NPCs", a_action.targetKey, value, error)) {
			g_menuStatus = std::format("Could not edit '{}': {}", path.filename().string(), error);
			return false;
		}
		return true;
	}

	bool WriteFactionEntry(const mpf::menu::Action& a_action, bool a_remove)
	{
		const std::filesystem::path path(a_action.sourcePath);
		if (!IsEditableConfigPath(path)) {
			g_menuStatus = "Refused an INI path outside Data/ModernPhysiqueFramework";
			return false;
		}
		std::string error;
		std::optional<std::string> value;
		if (!a_remove) {
			const auto strength = a_action.randomStrength ? std::string("random") :
				a_action.automaticStrength ? std::string("auto") :
				std::format("{:g}", std::clamp(a_action.strength, 0.0f, 100.0f));
			const auto physique = a_action.randomPhysique ? std::string("random") : a_action.physique;
			value = std::format("{}, {}, {}%", strength, physique,
				std::clamp(a_action.integerValue, 0, 100));
		}
		if (!mpf::ini::SetValue(path, "Factions", a_action.targetKey, value, error)) {
			g_menuStatus = std::format("Could not edit '{}': {}", path.filename().string(), error);
			return false;
		}
		return true;
	}

	bool EnsureUserIni(const std::filesystem::path& a_path)
	{
		if (std::filesystem::exists(a_path)) {
			return true;
		}
		std::string error;
		if (!mpf::ini::SetValue(a_path, "Settings", "Name", "MPF User", error) ||
			!mpf::ini::SetValue(a_path, "Settings", "Priority", "0", error)) {
			g_menuStatus = "Could not create MPF_User.ini: " + error;
			return false;
		}
		return true;
	}

	void ProcessMenuAction(mpf::menu::Action a_action)
	{
		switch (a_action.kind) {
		case mpf::menu::ActionKind::editNpc:
			if (WriteNpcEntry(a_action, false)) {
				ReloadConfigurationFromDisk(true, a_action.formID);
			} else {
				PublishMenuSnapshot();
			}
			break;
		case mpf::menu::ActionKind::deleteNpc:
			if (WriteNpcEntry(a_action, true)) {
				ReloadConfigurationFromDisk(true, a_action.formID);
			} else {
				PublishMenuSnapshot();
			}
			break;
		case mpf::menu::ActionKind::addNpc:
		{
			const auto targetID = a_action.formID != 0 ? a_action.formID : ResolveTarget(a_action.stableID);
			if (targetID == 0 || !RE::TESForm::LookupByID<RE::TESNPC>(targetID)) {
				g_menuStatus = std::format("NPC '{}' did not resolve", a_action.stableID);
				PublishMenuSnapshot();
				break;
			}
			if (a_action.formID == 0 && g_entryCandidates.contains(targetID)) {
				g_menuStatus = "That NPC already has an INI entry; open its row to edit it";
				PublishMenuSnapshot();
				break;
			}
			const auto userPath = ConfigPath("MPF_User.ini");
			if (!EnsureUserIni(userPath)) {
				PublishMenuSnapshot();
				break;
			}
			a_action.sourcePath = userPath.generic_string();
			a_action.targetKey = a_action.formID != 0 ? StableFormText(
				RE::TESForm::LookupByID<RE::TESNPC>(targetID)) : a_action.stableID;
			if (WriteNpcEntry(a_action, false)) {
				ReloadConfigurationFromDisk(true, targetID);
			} else {
				PublishMenuSnapshot();
			}
			break;
		}
		case mpf::menu::ActionKind::editFaction:
			if (WriteFactionEntry(a_action, false)) {
				ReloadConfigurationFromDisk(true);
			} else {
				PublishMenuSnapshot();
			}
			break;
		case mpf::menu::ActionKind::deleteFaction:
			if (WriteFactionEntry(a_action, true)) {
				ReloadConfigurationFromDisk(true);
			} else {
				PublishMenuSnapshot();
			}
			break;
		case mpf::menu::ActionKind::addFaction:
		{
			const auto targetID = a_action.formID != 0 ? a_action.formID : ResolveFaction(a_action.stableID);
			auto* faction = targetID != 0 ? RE::TESForm::LookupByID<RE::TESFaction>(targetID) : nullptr;
			if (!faction) {
				g_menuStatus = std::format("Faction '{}' did not resolve", a_action.stableID);
				PublishMenuSnapshot();
				break;
			}
			if (a_action.formID == 0 && std::ranges::any_of(g_groups, [&](const auto& rule) {
					return rule.factionID == targetID;
				})) {
				g_menuStatus = "That faction already has an INI entry; open it in Factions to edit";
				PublishMenuSnapshot();
				break;
			}
			const auto userPath = ConfigPath("MPF_User.ini");
			if (!EnsureUserIni(userPath)) {
				PublishMenuSnapshot();
				break;
			}
			a_action.sourcePath = userPath.generic_string();
			a_action.targetKey = StableFormText(faction);
			if (WriteFactionEntry(a_action, false)) {
				ReloadConfigurationFromDisk(true);
			} else {
				PublishMenuSnapshot();
			}
			break;
		}
		case mpf::menu::ActionKind::setSourcePriority:
		{
			const std::filesystem::path path(a_action.sourcePath);
			std::string error;
			if (!IsEditableConfigPath(path) || !mpf::ini::SetValue(path, "Settings", "Priority",
					std::to_string(a_action.integerValue), error)) {
				g_menuStatus = error.empty() ? "Refused an INI path outside the MPF config folder" : error;
				PublishMenuSnapshot();
			} else {
				ReloadConfigurationFromDisk(true);
			}
			break;
		}
		case mpf::menu::ActionKind::setLogging:
		{
			const char* levels[] = { "normal", "verbose", "diagnostic" };
			const int level = std::clamp(a_action.integerValue, 0, 2);
			std::string error;
			if (!mpf::ini::SetValue(RuntimeSettingsPath(), "Runtime", "Logging", levels[level], error)) {
				g_menuStatus = "Could not save runtime settings: " + error;
			} else {
				LoadRuntimeSettings();
				g_menuStatus = std::format("Logging changed to {}", levels[level]);
			}
			PublishMenuSnapshot();
			break;
		}
		case mpf::menu::ActionKind::reloadAndRefresh:
			ReloadConfigurationFromDisk(true);
			break;
		case mpf::menu::ActionKind::refreshNearest:
			if (auto* actor = NearestActor()) {
				ClearOwnedBodyMorph(actor, "menu-refresh");
				g_bodyApplied.erase(actor->GetFormID());
				actor->DoReset3D(false);
				g_menuStatus = std::format("Refreshed nearest actor '{}'", actor->GetName());
			} else {
				g_menuStatus = "No nearby actor was found";
			}
			PublishMenuSnapshot();
			break;
		case mpf::menu::ActionKind::refreshAll:
		{
			ResetRuntimeTracking(true);
			const auto refreshed = RefreshLoadedActors();
			g_menuStatus = std::format("Refreshed {} loaded actors", refreshed);
			PublishMenuSnapshot();
			break;
		}
		}
	}

	void ReceiveMenuAction(mpf::menu::Action a_action)
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([action = std::move(a_action)]() mutable {
				ProcessMenuAction(std::move(action));
			});
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			if (mpf::menu::Register()) {
				spdlog::info("SKSE Menu Framework page registered (version {})",
					mpf::menu::FrameworkApiVersion());
			}
			RequestBodyMorphAPI();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			BuildHeadPartIndex();
			LoadLibrary();
			LoadRuntimeSettings();
			LoadPatches();
			LoadOutfitTable();
			LoadFactionNames();
			ExpandGroupRules();
			ResolvePhysiques();
			ApplyRecordChanges();
			g_dataLoaded.store(true, std::memory_order_release);
			if (HasBodyMorphMappings() && !g_bodyMorphAPI) {
				spdlog::warn("body morph mappings are configured, but RaceMenu/SKEE is unavailable");
			}
			ScheduleBodyMorphsForLoadedActors(false);
			if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
				input->AddEventSink<RE::InputEvent*>(InputSink::GetSingleton());
			}
			InstallHook();
			spdlog::info("ready: applies when a head is built | F6 apply to nearest | Shift+F7 report nearest");
			g_menuStatus = std::format("Loaded {} effective NPCs from {} INI files",
				g_entries.size(), g_sources.size());
			PublishMenuSnapshot();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			// A delayed task from the previous save must never resolve a recycled
			// handle in the next one. Invalidate it before the loading screen starts,
			// but retain face-buffer fingerprints because FaceGen may reuse those
			// already-morphed buffers during the load.
			ResetRuntimeTracking(true, false);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			// Load3D calls queued during the load belong to the new generation and
			// are intentionally retained so their faces can be applied.
			ScheduleBodyMorphsForLoadedActors(false);
			break;
		case SKSE::MessagingInterface::kNewGame:
			// NewGame can also reuse FaceGen allocations from the previous world when
			// entered without restarting the process.
			ResetRuntimeTracking(true, false);
			ScheduleBodyMorphsForLoadedActors(false);
			QueueLoadedActorsForHeads();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLog();
	spdlog::info("Modern Physique Framework (MPF) {} loaded | backend={} | build={} | compiled={} {}",
		MPF_VERSION, MPF_BODY_BACKEND, MPF_BUILD_ID, __DATE__, __TIME__);
	mpf::menu::SetActionHandler(ReceiveMenuAction);
	mpf::menu::Register();
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
