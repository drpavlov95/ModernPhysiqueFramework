// SPDX-License-Identifier: GPL-3.0-only
// Modern Physique Framework - applies named TRI morphs to an NPC's head at runtime,
// on top of whatever FaceGeom NIF the load order produced.
//
// It never regenerates FaceGen, never touches an NPC record, never touches the
// face tint, and writes nothing to the save. That is the whole point: the two
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
#include <chrono>
#include <thread>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SimpleIni.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.hpp>

#include "tri.h"

// OBody's public plugin API deliberately leaves the game types undefined so
// consumers can supply the aliases from whichever SKSE/CommonLib they use.
using Actor = RE::Actor;
using TESForm = RE::TESForm;
#include "third_party/obody/API.h"

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

	// F6/F7 deliberately: the throwaway FaceProbe diagnostic sits on F8-F12, and
	// the two need to be usable in the same session.
	constexpr std::uint32_t kKeyApply = 0x40;   // F6 - apply now, for testing
	constexpr std::uint32_t kKeyReport = 0x41;  // F7 - say what would be applied

	struct MorphRequest
	{
		std::string name;
		float       weight{ 0.0f };
	};

	struct NpcEntry
	{
		std::string               targetText;
		RE::FormID                baseFormID{ 0 };
		std::string               physique;
		std::vector<MorphRequest> morphs;
		int                       priority{ 0 };
		std::string               sourceFile;
		// The weight from the list, 0-100, on the same scale as the vanilla
		// slider: it is the whole strength of the morph, one to one. A negative
		// value means "auto", which reads the NPC's own record weight instead,
		// for setups that drive weight from somewhere else.
		float                     weight{ -1.0f };
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
		std::string              name;
		std::string              faceMorph;
		std::vector<std::string> malePresets;
		std::vector<std::string> femalePresets;
		int                      priority{ 0 };
		std::string              sourceFile;
	};

	// Lower-cased physique name -> definition. Names are case-insensitive in the
	// editable lists while preset and morph names retain their exact case.
	std::unordered_map<std::string, PhysiqueDefinition> g_physiques;
	OBody::API::IPluginInterface*                    g_obodyAPI{ nullptr };
	std::atomic_bool                                 g_obodyReady{ false };
	std::atomic_bool                                 g_dataLoaded{ false };
	// Reference -> preset. Accessed only from game-thread tasks.
	std::unordered_map<RE::FormID, std::string>      g_obodySynced;
	std::unordered_set<RE::FormID>                   g_obodyFailed;
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
		float       weight{ 100.0f };
		int         percent{ 100 };
		std::string physique;
		std::string sourceFile;
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
	bool                                                                     g_verbose = false;
	bool                                                                     g_diagnostics = false;

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
			const auto key = Lowered(file);
			if (const auto [it, fresh] = a_seen.insert(key); !fresh) {
				spdlog::info("{}: '{}' was already declared by another manifest - skipped", name, file);
				continue;
			}

			rfm::TriFile tri;
			std::string  error;
			if (!tri.Load(a_dir / file, error)) {
				spdlog::error("{}: morph file '{}' refused: {}", name, file, error);
				continue;
			}
			const LibraryKey libKey{ sex == "female", tri.VertexCount() };
			spdlog::info("morphs '{}' ({}): sex={} part={} verts={} morphs={}",
				file, name, sex, PartTypeName(partType), tri.VertexCount(), tri.MorphNames().size());
			g_library[libKey].push_back(LibraryEntry{ file, partType, std::move(tri) });
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
		for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
			if (entry.is_regular_file() && entry.path().extension() == ".toml") {
				manifests.push_back(entry.path());
			}
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

	// "0x00013479~Skyrim.esm", or an EditorID when po3's Tweaks is installed.
	RE::FormID ResolveTarget(const std::string& a_text)
	{
		const auto tilde = a_text.find('~');
		if (tilde != std::string::npos) {
			const auto idText = a_text.substr(0, tilde);
			const auto plugin = a_text.substr(tilde + 1);
			auto*      handler = RE::TESDataHandler::GetSingleton();
			if (!handler) {
				return 0;
			}
			const auto localID = static_cast<RE::FormID>(std::strtoul(idText.c_str(), nullptr, 16));
			auto*      form = handler->LookupForm(localID, plugin);
			return form ? form->GetFormID() : 0;
		}
		auto* form = RE::TESForm::LookupByEditorID(a_text);
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
			const auto id = ResolveTarget(ini.GetValue("Factions", key.pItem, ""));
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
	//     JobFarmerFaction = 80, Thin
	//
	// Same value shape as [NPCs] - a weight and required physique - plus a
	// percentage, which is what makes "random bandits" possible without the result
	// changing every time the game starts.
	void LoadGroupRules(CSimpleIniA& a_ini, const std::string& a_file, int a_priority)
	{
		CSimpleIniA::TNamesDepend keys;
		a_ini.GetAllKeys("Factions", keys);
		for (const auto& key : keys) {
			GroupRule rule;
			rule.text = key.pItem ? key.pItem : "";
			rule.sourceFile = a_file;
			rule.priority = a_priority;
			if (rule.text.empty()) {
				continue;
			}
			std::string value = a_ini.GetValue("Factions", key.pItem, "");
			if (const auto comment = value.find(';'); comment != std::string::npos) {
				value.erase(comment);
			}
			std::stringstream parts(value);
			std::string       token;
			bool              first = true;
			bool              bad = false;
			bool              hasPhysique = false;
			while (std::getline(parts, token, ',')) {
				while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) token.erase(token.begin());
				while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
				if (token.empty()) {
					continue;
				}
				if (first) {
					try {
						rule.weight = std::stof(token);
					} catch (const std::exception&) {
						bad = true;
					}
					first = false;
				} else if (token.back() == '%') {
					try {
						rule.percent = std::stoi(token.substr(0, token.size() - 1));
					} catch (const std::exception&) {
						bad = true;
					}
				} else if (!hasPhysique) {
					rule.physique = token;
					hasPhysique = true;
				} else {
					bad = true;
				}
			}
			if (bad || !hasPhysique || rule.physique.empty() ||
				rule.weight < 0.0f || rule.weight > 100.0f ||
				rule.percent < 0 || rule.percent > 100) {
				spdlog::error("{}: [Factions] '{}' = '{}' must be weight, physique and "
					"an optional percentage", a_file, rule.text, value);
				continue;
			}
			// Resolved later, in ExpandGroupRules: the name table is loaded after
			// the lists are read, and tying the two together by load order is the
			// kind of dependency that breaks quietly.
			spdlog::info("   faction {:<28} weight {:g}, physique '{}', {}%", rule.text,
				rule.weight, rule.physique, rule.percent);
			g_groups.push_back(std::move(rule));
		}
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

	std::vector<std::string> ParsePresetPool(std::string a_value)
	{
		if (const auto comment = a_value.find(';'); comment != std::string::npos) {
			a_value.erase(comment);
		}
		std::vector<std::string> result;
		std::stringstream values(a_value);
		std::string preset;
		while (std::getline(values, preset, '|')) {
			Trim(preset);
			if (!preset.empty()) {
				result.push_back(std::move(preset));
			}
		}
		return result;
	}

	// [Physique.Heavy]
	// FaceMorph = RLY_Heavy
	// MalePresets = Runtime Morphs Heavy Male 01 | Runtime Morphs Heavy Male 02
	// FemalePresets = Runtime Morphs Heavy Female 01
	//
	// Section suffixes are user-defined and case-insensitive. Preset pools are
	// optional (the standalone build has none), but FaceMorph is required.
	void LoadPhysiques(CSimpleIniA& a_ini, const std::string& a_file, int a_priority)
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
			candidate.faceMorph = a_ini.GetValue(sectionName.c_str(), "FaceMorph", "");
			if (const auto comment = candidate.faceMorph.find(';'); comment != std::string::npos) {
				candidate.faceMorph.erase(comment);
			}
			Trim(candidate.faceMorph);
			candidate.malePresets = ParsePresetPool(
				a_ini.GetValue(sectionName.c_str(), "MalePresets", ""));
			candidate.femalePresets = ParsePresetPool(
				a_ini.GetValue(sectionName.c_str(), "FemalePresets", ""));

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
	// The number is the NPC's weight on the vanilla 0-100 scale and it is the
	// entire strength of the morph, one to one. Adding somebody is a line;
	// removing them is deleting the line. "auto" reads the record's own weight,
	// for setups that already drive weight from somewhere else.
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
		if (ini.GetBoolValue("Settings", "Diagnostics", false)) {
			g_diagnostics = true;
		}
		if (const auto* v = ini.GetValue("Settings", "SkinMale", ""); v && *v) {
			g_skinMaleText = v;
		}
		if (const auto* v = ini.GetValue("Settings", "SkinFemale", ""); v && *v) {
			g_skinFemaleText = v;
		}

		LoadPhysiques(ini, fileName, priority);
		LoadGroupRules(ini, fileName, priority);

		CSimpleIniA::TNamesDepend keys;
		ini.GetAllKeys("NPCs", keys);
		if (keys.empty()) {
			spdlog::warn("{}: no [NPCs] section, or it is empty", fileName);
			return;
		}

		for (const auto& key : keys) {
			NpcEntry entry;
			entry.sourceFile = fileName;
			entry.priority = priority;
			entry.targetText = key.pItem ? key.pItem : "";
			if (entry.targetText.empty()) {
				continue;
			}

			// SimpleIni does not strip trailing comments from values, so cut one
			// before splitting the required "weight, physique" pair.
			const auto uncomment = [](std::string t) {
				if (const auto c = t.find(';'); c != std::string::npos) {
					t.erase(c);
				}
				return t;
			};

			// "<weight>, <physique>" -- both fields are required.
			std::string value = uncomment(ini.GetValue("NPCs", key.pItem, ""));
			const auto comma = value.find(',');
			if (comma == std::string::npos || value.find(',', comma + 1) != std::string::npos) {
				spdlog::error("{}: '{}' must be written as weight, physique", fileName,
					entry.targetText);
				continue;
			}
			entry.physique = value.substr(comma + 1);
			value = value.substr(0, comma);
			Trim(value);
			Trim(entry.physique);

			if (value == "auto" || value == "AUTO") {
				entry.weight = -1.0f;
			} else {
				try {
					entry.weight = std::stof(value);
				} catch (const std::exception&) {
					spdlog::error("{}: '{}' has value '{}', which is neither a weight nor 'auto'",
						fileName, entry.targetText, value);
					continue;
				}
				if (entry.weight < 0.0f || entry.weight > 100.0f) {
					spdlog::error("{}: '{}' has weight {:.1f}; the scale is 0 to 100", fileName,
						entry.targetText, entry.weight);
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

			entry.baseFormID = ResolveTarget(entry.targetText);
			if (entry.baseFormID == 0) {
				spdlog::warn("{}: target '{}' did not resolve (EditorIDs need po3's Tweaks)",
					fileName, entry.targetText);
				continue;
			}
			// What a name resolved to is worth writing down. A name always
			// resolves to something, so "57 loaded" says nothing about whether
			// they are the NPCs the author meant - and an entry that resolved to
			// the wrong form looks exactly like an entry that works.
			spdlog::info("   {:<26} -> 0x{:08X}  weight {}, physique '{}'", entry.targetText,
				entry.baseFormID, entry.weight < 0.0f ? std::string("from the record") :
				std::format("{:g}", entry.weight), entry.physique);

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
			for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
				if (entry.is_regular_file() && entry.path().extension() == ".ini") {
					paths.push_back(entry.path());
				}
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
			const auto to = ResolveTarget(ini.GetValue("Outfits", key.pItem, ""));
			if (from == 0 || to == 0) {
				++unresolved;
				continue;
			}
			g_outfitFamily[from] = to;
		}
		spdlog::info("outfit table: {} vanilla outfits mapped, {} unresolved",
			g_outfitFamily.size(), unresolved);
	}

	// Weight, skin and outfit, applied to the base records at kDataLoaded.
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
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return;
		}
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
				// A name written by hand always wins over a group.
				if (g_entries.contains(npc->GetFormID())) {
					++alreadyListed;
					continue;
				}
				// Stable share: the same records are chosen every time, because the
				// choice comes from the form id and nothing else.
				if (rule.percent < 100) {
					const std::uint32_t id = npc->GetFormID();
					std::uint32_t h = id * 2654435761u;
					h ^= h >> 15;
					if (static_cast<int>(h % 100u) >= rule.percent) {
						++notChosen;
						continue;
					}
				}
				NpcEntry entry;
				entry.targetText = rule.text;
				entry.sourceFile = rule.sourceFile;
				entry.priority = rule.priority;
				entry.weight = rule.weight;
				entry.baseFormID = npc->GetFormID();
				entry.physique = rule.physique;
				g_entries.insert_or_assign(entry.baseFormID, std::move(entry));
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

		int weights = 0, skins = 0, outfits = 0, byFamily = 0, dressless = 0, children = 0;
		for (auto& [formID, entry] : g_entries) {
			auto* npc = RE::TESForm::LookupByID<RE::TESNPC>(formID);
			if (!npc) {
				continue;
			}
			if (IsChildNPC(npc)) {
				spdlog::warn("'{}' (0x{:08X}) is a child and was skipped - weight, skin and "
					"outfit were not changed", entry.targetText, formID);
				++children;
				continue;
			}

			if (entry.weight >= 0.0f) {
				npc->weight = entry.weight;
				++weights;
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
		spdlog::info("records patched: {} weights, {} skins, {} outfits ({} chosen by family), "
			"{} left in vanilla clothes, {} children refused", weights, skins, outfits,
			byFamily, dressless, children);
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
		const float npcWeight = base ? base->weight : 100.0f;
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
				// The face rides the same axis as the body: an NPC at weight 100
				// gets the declared strength, one at weight 0 gets none. That is
				// what keeps a heavy face on a heavy body without anyone having
				// to author a number per NPC.
				// The list's weight is the strength, one to one; "auto" falls back
				// to whatever the record says, for setups that drive weight
				// elsewhere.
				const float listedWeight = a_entry.weight < 0.0f ? npcWeight : a_entry.weight;
				const float effective = listedWeight / 100.0f;
				if (effective == 0.0f) {
					spdlog::info("[{}] '{}' '{}' is at weight 0, nothing to apply",
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
				spdlog::info("[{}] '{}' {} '{}' x{:.3f} (weight {:.1f} from the {}) over {} verts "
							 "from '{}' (mesh distance {:.3f}), maxMove={:.4f}, buffer {}",
					a_tag, geom->name.c_str(), a_dryRun ? "would apply" : "applied",
					request.name, effective, listedWeight, a_entry.weight < 0.0f ? "record" : "list", vcount, set->file, distance, maxMove,
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

	bool OnList(RE::Actor* a_actor) { return ListedBase(a_actor) != nullptr; }

	std::optional<std::string> PickOBodyPreset(const NpcEntry& a_entry, bool a_female)
	{
		const auto definition = g_physiques.find(Lowered(a_entry.physique));
		if (definition == g_physiques.end()) {
			return std::nullopt;
		}
		const auto& presets = a_female ? definition->second.femalePresets : definition->second.malePresets;
		if (presets.empty()) {
			return std::nullopt;
		}

		// Same avalanche used by faction percentages: cheap, stable, and based on
		// the listed base rather than the transient spawned reference.
		std::uint32_t h = a_entry.baseFormID * 2654435761u;
		h ^= h >> 15;
		return presets[h % presets.size()];
	}

	bool HasOBodyPresetMappings()
	{
		return std::ranges::any_of(g_physiques, [](const auto& item) {
			return !item.second.malePresets.empty() || !item.second.femalePresets.empty();
		});
	}

	void SyncOBodyPreset(RE::Actor* a_actor, const NpcEntry& a_entry, const char* a_tag)
	{
		if (!a_actor || !g_obodyReady.load(std::memory_order_acquire) || !g_obodyAPI) {
			return;
		}

		const auto referenceID = a_actor->GetFormID();
		if (g_obodySynced.contains(referenceID) || g_obodyFailed.contains(referenceID)) {
			return;
		}

		const auto* actorBase = a_actor->GetActorBase();
		const bool female = actorBase && actorBase->GetSex() == RE::SEX::kFemale;
		const auto desired = PickOBodyPreset(a_entry, female);
		if (!desired) {
			// Empty pools are valid for the standalone build and for face-only
			// physiques. OBody simply keeps control of body distribution.
			return;
		}

		OBody::API::PresetAssignmentInformation current{};
		g_obodyAPI->GetPresetAssignedToActor(a_actor, current);
		if (current.presetName == *desired) {
			g_obodySynced.insert_or_assign(referenceID, *desired);
			if (g_diagnostics) {
				spdlog::info("[{}] '{}' already has OBody preset '{}'", a_tag,
					a_actor->GetName(), *desired);
			}
			return;
		}

		OBody::API::AssignPresetPayload payload{};
		payload.presetName = *desired;
		if (g_obodyAPI->AssignPresetToActor(a_actor, payload)) {
			g_obodySynced.insert_or_assign(referenceID, *desired);
			spdlog::info("[{}] '{}' ref=0x{:08X} -> OBody preset '{}'", a_tag,
				a_actor->GetName(), referenceID, *desired);
		} else {
			g_obodyFailed.insert(referenceID);
			spdlog::error("[{}] '{}' could not receive OBody preset '{}' - check the internal "
				"name in the BodySlide XML", a_tag, a_actor->GetName(), *desired);
		}
	}

	void ScheduleOBodySyncForLoadedActors(bool a_reset)
	{
		auto* tasks = SKSE::GetTaskInterface();
		if (!tasks) {
			return;
		}
		tasks->AddTask([a_reset]() {
			if (a_reset) {
				g_obodySynced.clear();
				g_obodyFailed.clear();
			}
			if (!g_dataLoaded.load(std::memory_order_acquire) ||
				!g_obodyReady.load(std::memory_order_acquire) || !g_obodyAPI) {
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
					continue;
				}
				if (const auto entry = g_entries.find(listed->GetFormID()); entry != g_entries.end()) {
					SyncOBodyPreset(actor.get(), entry->second, "obody-ready");
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
			if (g_diagnostics) {
				spdlog::info("[{}] '{}' base=0x{:08X} is a child - refused", a_tag,
					a_actor->GetName(), base->GetFormID());
			}
			return stats;
		}
		const auto it = g_entries.find(base->GetFormID());
		if (it == g_entries.end()) {
			if (g_verbose) {
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

		if (!a_dryRun) {
			SyncOBodyPreset(a_actor, it->second, a_tag);
		} else if (const auto preset = PickOBodyPreset(it->second,
			base->GetSex() == RE::SEX::kFemale)) {
			spdlog::info("[{}] '{}' would receive OBody preset '{}'", a_tag,
				a_actor->GetName(), *preset);
		}
		// Dress the actor itself, once. The base record was patched at
		// kDataLoaded, which settles anybody the game builds afterwards; an actor
		// the save already contained needs telling directly.
		if (!a_dryRun && it->second.outfitFormID != 0 && !g_dressed.contains(a_actor->GetFormID())) {
			if (auto* outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(it->second.outfitFormID)) {
				const bool changed = a_actor->SetDefaultOutfit(outfit, true);
				g_dressed.insert(a_actor->GetFormID());

				// "The body looks past 100%" has been reported three times, and the
				// files rule out the obvious causes: the weight is 100, which is the
				// maximum; nothing but heads, beards and brows receives a morph; and
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
	class LoadSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
	{
	public:
		static LoadSink* GetSingleton()
		{
			static LoadSink s;
			return &s;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event,
			RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
		{
			if (!a_event) {
				return RE::BSEventNotifyControl::kContinue;
			}
			auto* form = RE::TESForm::LookupByID(a_event->formID);
			auto* actor = form ? form->As<RE::Actor>() : nullptr;
			auto* base = actor ? actor->GetActorBase() : nullptr;

			// Every event, unconditionally, when diagnostics are on. Two NPCs
			// appeared in neither the applied nor the unmatched list last time,
			// which the earlier logging could not distinguish from the event
			// never arriving - so now nothing is filtered before it is recorded.
			if (g_diagnostics) {
				spdlog::info("[event] ref=0x{:08X} loaded={} form={} actor={} base={} name='{}'",
					a_event->formID, a_event->loaded ? 1 : 0,
					form ? "yes" : "NULL", actor ? "yes" : "NULL",
					base ? std::format("0x{:08X}", base->GetFormID()) : std::string("NULL"),
					actor ? actor->GetName() : "");
			}

			if (!base || !OnList(actor)) {
				return RE::BSEventNotifyControl::kContinue;
			}
			if (!a_event->loaded) {
				// Deliberately NOT forgetting the buffers this actor used. A
				// recycled address is already handled by the fingerprint, while
				// forgetting would let a head shared between two references of
				// the same base NPC take the morph twice.
				return RE::BSEventNotifyControl::kContinue;
			}
			// The head is not built yet when this fires, so apply a beat later.
			SKSE::GetTaskInterface()->AddTask([formID = actor->GetFormID()]() {
				auto* f = RE::TESForm::LookupByID(formID);
				if (auto* a = f ? f->As<RE::Actor>() : nullptr) {
					HandleActor(a, "3d-loaded", false);
				}
			});
			return RE::BSEventNotifyControl::kContinue;
		}
	};

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
				if (!button || !button->IsDown()) {
					continue;
				}
				const auto code = button->GetIDCode();
				if (code != kKeyReport && code != kKeyApply) {
					continue;
				}
				auto* actor = NearestActor();
				if (!actor) {
					spdlog::warn("no actor nearby");
					continue;
				}
				HandleActor(actor, code == kKeyReport ? "F7-report" : "F8-apply", code == kKeyReport);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// The 3D-load event is not dependable: measured on a nine-NPC spawn, only
	// sixteen events arrived at all and two of the NPCs produced none, while
	// their bodies were visibly patched by other means. So the event is kept as
	// Where the morph is applied from, and why it is a hook rather than a listener.
	//
	// The face lives in a vertex buffer we write into, and that write does not
	// survive the head being rebuilt - so the question is not "has this NPC appeared
	// in this save", the way OBody asks (its morphs are stored by SKEE and reapplied
	// for it), but "has this head just been built", every time. Character::Load3D is
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
		int             attempts{ 0 };
	};

	std::mutex               g_pendingLock;
	std::condition_variable  g_pendingCv;
	std::vector<Pending>     g_pending;
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
					batch.swap(g_pending);
				}
				std::this_thread::sleep_for(kHeadWaitStep);
				auto* task = SKSE::GetTaskInterface();
				if (!task) {
					continue;
				}
				task->AddTask([batch]() mutable {
					std::vector<Pending> again;
					for (auto& item : batch) {
						auto actor = item.handle.get();
						if (!actor) {
							continue;
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
						g_pending.insert(g_pending.end(), again.begin(), again.end());
						g_pendingCv.notify_one();
					}
				});
			}
		}).detach();
	}

	void QueueForHead(RE::Actor* a_actor)
	{
		{
			std::lock_guard lock(g_pendingLock);
			g_pending.push_back({ a_actor->CreateRefHandle(), 0 });
		}
		g_pendingCv.notify_one();
	}

	struct Load3DHook
	{
		static RE::NiAVObject* thunk(RE::TESObjectREFR* a_this, bool a_background)
		{
			auto* result = func(a_this, a_background);
			if (g_entries.empty()) {
				return result;
			}
			auto* actor = a_this ? a_this->As<RE::Actor>() : nullptr;
			// A base the game invented at runtime does not carry its origin yet at
			// this point, so ListedBase can answer "no" for somebody who is on the
			// list. Those are queued anyway and the queue asks again once the head
			// is up - it is how Skjorni was reachable at all.
			if (actor && (OnList(actor) || (actor->GetActorBase() &&
					(actor->GetActorBase()->GetFormID() >> 24) == 0xFF))) {
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

	class OBodyReadinessListener final : public OBody::API::IOBodyReadinessEventListener
	{
	public:
		static OBodyReadinessListener* GetSingleton()
		{
			static OBodyReadinessListener singleton;
			return &singleton;
		}

		void OBodyIsReady() override
		{
			g_obodyReady.store(true, std::memory_order_release);
			spdlog::info("OBody API is ready");
			ScheduleOBodySyncForLoadedActors(true);
		}

		void OBodyIsNoLongerReady() override
		{
			g_obodyReady.store(false, std::memory_order_release);
			spdlog::info("OBody API is temporarily unavailable");
		}
	};

	void RequestOBodyAPI()
	{
		OBody::API::SKSEMessages::RequestPluginInterface request{};
		request.version = OBody::API::PluginAPIVersion::Latest;
		request.pluginInterface = &g_obodyAPI;
		request.readinessEventListener = OBodyReadinessListener::GetSingleton();

		SKSE::GetMessagingInterface()->Dispatch(
			decltype(request)::type, &request, sizeof(request), "OBody");
		if (g_obodyAPI) {
			spdlog::info("OBody plugin API found; preset assignment will use INI mappings");
		} else {
			spdlog::info("OBody plugin API not found; face morphing remains available");
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			RequestOBodyAPI();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			BuildHeadPartIndex();
			LoadLibrary();
			LoadPatches();
			LoadOutfitTable();
			LoadFactionNames();
			ExpandGroupRules();
			ResolvePhysiques();
			ApplyRecordChanges();
			g_dataLoaded.store(true, std::memory_order_release);
			if (HasOBodyPresetMappings() && !g_obodyAPI) {
				spdlog::warn("OBody preset mappings are configured, but a compatible OBody API was not found");
			}
			ScheduleOBodySyncForLoadedActors(false);
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESObjectLoadedEvent>(LoadSink::GetSingleton());
			}
			if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
				input->AddEventSink<RE::InputEvent*>(InputSink::GetSingleton());
			}
			InstallHook();
			spdlog::info("ready: applies when a head is built | F6 apply to nearest | F7 report nearest");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			// Heads are rebuilt across a load, so the record of what we already
			// wrote is stale.
			g_applied.clear();
			g_dressed.clear();
			g_counted.clear();
			g_announced.clear();
			g_obodySynced.clear();
			g_obodyFailed.clear();
			ScheduleOBodySyncForLoadedActors(false);
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
	spdlog::info("Modern Physique Framework (MPF) loaded");
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
