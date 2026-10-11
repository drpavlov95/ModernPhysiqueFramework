// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mpf::menu
{
	enum class OriginKind
	{
		direct,
		faction
	};

	struct Candidate
	{
		std::string sourcePath;
		std::string sourceName;
		std::string targetKey;
		std::string physique;
		float       strength{ 0.0f };
		bool        automaticStrength{ false };
		int         priority{ 0 };
		OriginKind  kind{ OriginKind::direct };
		bool        winner{ false };
	};

	struct NpcRow
	{
		std::uint32_t         formID{ 0 };
		std::string           stableID;
		std::string           name;
		std::string           physique;
		float                 strength{ 0.0f };
		bool                  automaticStrength{ false };
		std::string           sourceName;
		std::size_t           sourceCount{ 0 };
		std::vector<Candidate> candidates;
	};

	struct NpcChoice
	{
		std::uint32_t formID{ 0 };
		std::string   stableID;
		std::string   name;
	};

	struct SourceRow
	{
		std::string path;
		std::string name;
		std::string author;
		int         priority{ 0 };
		std::size_t directEntries{ 0 };
		std::size_t factionRules{ 0 };
		std::size_t conflictsWon{ 0 };
	};

	struct FactionRow
	{
		std::uint32_t formID{ 0 };
		std::string   stableID;
		std::string   name;
		std::string   targetKey;
		std::string   physique;
		float         strength{ 0.0f };
		int           percent{ 100 };
		std::string   sourceName;
		std::string   sourcePath;
		int           priority{ 0 };
		bool          automaticStrength{ false };
		bool          randomStrength{ false };
		bool          randomPhysique{ false };
	};

	struct FactionChoice
	{
		std::uint32_t formID{ 0 };
		std::string   stableID;
		std::string   name;
	};

	struct PhysiqueRow
	{
		std::string name;
		std::string faceMorph;
		std::string bodyMorph;
		std::string sourceName;
		int         priority{ 0 };
	};

	struct Snapshot
	{
		std::uint64_t             revision{ 0 };
		std::vector<NpcRow>       npcs;
		std::vector<NpcChoice>    npcChoices;
		std::vector<FactionRow>   factions;
		std::vector<FactionChoice> factionChoices;
		std::vector<SourceRow>    sources;
		std::vector<PhysiqueRow>  physiques;
		std::vector<std::string>  physiqueNames;
		std::string               status;
		std::string               buildID;
		std::string               backend;
		std::size_t               conflictCount{ 0 };
		float                     menuApiVersion{ 0.0f };
		bool                      skeeAvailable{ false };
		int                       loggingLevel{ 0 };  // 0 normal, 1 verbose, 2 diagnostic
	};

	enum class ActionKind
	{
		editNpc,
		deleteNpc,
		addNpc,
		editFaction,
		deleteFaction,
		addFaction,
		setSourcePriority,
		setLogging,
		reloadAndRefresh,
		refreshNearest,
		refreshAll
	};

	struct Action
	{
		ActionKind   kind{ ActionKind::reloadAndRefresh };
		std::uint32_t formID{ 0 };
		std::string  stableID;
		std::string  sourcePath;
		std::string  targetKey;
		std::string  physique;
		float        strength{ 0.0f };
		bool         automaticStrength{ false };
		int          integerValue{ 0 };
		bool         randomStrength{ false };
		bool         randomPhysique{ false };
	};

	using ActionHandler = void (*)(Action);

	void SetActionHandler(ActionHandler a_handler);
	void Publish(std::shared_ptr<const Snapshot> a_snapshot);
	bool Register();
	float FrameworkApiVersion();
}
