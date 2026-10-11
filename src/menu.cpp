// SPDX-License-Identifier: GPL-3.0-only
#include "menu.h"

#include "third_party/skse_menu_framework/API.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <mutex>

namespace mpf::menu
{
	namespace
	{
		std::shared_ptr<const Snapshot> g_snapshot;
		std::mutex                      g_snapshotLock;
		ActionHandler                   g_handler{ nullptr };
		std::atomic_bool                g_registered{ false };

		std::array<char, 128> g_filter{};
		std::array<char, 128> g_factionNpcFilter{};
		std::array<char, 128> g_factionFilter{};
		std::string           g_sourceFilterPath;
		std::string           g_factionNpcSourceFilterPath;
		bool                  g_conflictsOnly{ false };
		bool                  g_factionNpcConflictsOnly{ false };
		std::uint32_t         g_selectedForm{ 0 };
		std::size_t           g_selectedCandidate{ 0 };
		std::array<char, 128> g_addTarget{};
		std::array<char, 128> g_addPhysique{};
		std::uint32_t          g_addFormID{ 0 };
		float                 g_addStrength{ 100.0f };
		bool                  g_addAuto{ false };
		bool                  g_showAddNpc{ false };
		std::string           g_addMatchQuery;
		std::uint64_t         g_addMatchRevision{ 0 };
		std::vector<std::size_t> g_addMatches;
		bool                  g_showAddFaction{ false };
		std::array<char, 128> g_addFactionTarget{};
		std::array<char, 128> g_addFactionPhysique{};
		float                 g_addFactionStrength{ 100.0f };
		int                   g_addFactionPercent{ 100 };
		bool                  g_addFactionAuto{ false };
		bool                  g_addFactionRandomStrength{ false };
		bool                  g_addFactionRandomPhysique{ false };
		std::string           g_addFactionMatchQuery;
		std::uint64_t         g_addFactionMatchRevision{ 0 };
		std::vector<std::size_t> g_addFactionMatches;
		std::array<char, 128> g_editPhysique{};
		float                 g_editStrength{ 0.0f };
		bool                  g_editAuto{ false };
		bool                  g_confirmDelete{ false };
		bool                  g_scrollToEditor{ false };
		std::string           g_selectedFactionSourcePath;
		std::string           g_selectedFactionTargetKey;
		std::array<char, 128> g_editFactionPhysique{};
		float                 g_editFactionStrength{ 100.0f };
		int                   g_editFactionPercent{ 100 };
		bool                  g_editFactionAuto{ false };
		bool                  g_editFactionRandomStrength{ false };
		bool                  g_editFactionRandomPhysique{ false };
		bool                  g_confirmFactionDelete{ false };
		bool                  g_scrollToFactionEditor{ false };
		std::string           g_selectedSourcePath;
		std::string           g_selectedSourceName;
		int                   g_selectedSourcePriority{ 0 };

		// High-contrast semantic colours keep the important controls visible even
		// when the framework's global ImGui theme is very dark or very muted.
		constexpr SMF3::Vec4 kBlue{ 0.12f, 0.34f, 0.62f, 1.0f };
		constexpr SMF3::Vec4 kBlueHover{ 0.18f, 0.50f, 0.86f, 1.0f };
		constexpr SMF3::Vec4 kBlueActive{ 0.08f, 0.23f, 0.45f, 1.0f };
		constexpr SMF3::Vec4 kGreen{ 0.12f, 0.42f, 0.27f, 1.0f };
		constexpr SMF3::Vec4 kGreenHover{ 0.18f, 0.62f, 0.38f, 1.0f };
		constexpr SMF3::Vec4 kGreenActive{ 0.08f, 0.28f, 0.18f, 1.0f };
		constexpr SMF3::Vec4 kRed{ 0.52f, 0.16f, 0.18f, 1.0f };
		constexpr SMF3::Vec4 kRedHover{ 0.76f, 0.25f, 0.28f, 1.0f };
		constexpr SMF3::Vec4 kRedActive{ 0.35f, 0.08f, 0.10f, 1.0f };
		constexpr SMF3::Vec4 kAmber{ 0.52f, 0.34f, 0.08f, 1.0f };
		constexpr SMF3::Vec4 kAmberHover{ 0.76f, 0.52f, 0.12f, 1.0f };
		constexpr SMF3::Vec4 kAmberActive{ 0.34f, 0.20f, 0.04f, 1.0f };
		constexpr SMF3::Vec4 kNeutral{ 0.28f, 0.31f, 0.36f, 1.0f };
		constexpr SMF3::Vec4 kNeutralHover{ 0.42f, 0.46f, 0.53f, 1.0f };
		constexpr SMF3::Vec4 kNeutralActive{ 0.18f, 0.21f, 0.26f, 1.0f };

		bool PrimaryButton(const char* a_label)
		{
			return SMF3::UI::ColoredButton(a_label, kBlue, kBlueHover, kBlueActive);
		}

		bool SuccessButton(const char* a_label)
		{
			return SMF3::UI::ColoredButton(a_label, kGreen, kGreenHover, kGreenActive);
		}

		bool DangerButton(const char* a_label)
		{
			return SMF3::UI::ColoredButton(a_label, kRed, kRedHover, kRedActive);
		}

		bool EditButton(const char* a_label)
		{
			return SMF3::UI::ColoredSmallButton(a_label, kAmber, kAmberHover, kAmberActive);
		}

		bool NeutralButton(const char* a_label)
		{
			return SMF3::UI::ColoredButton(a_label, kNeutral, kNeutralHover, kNeutralActive);
		}

		enum class NpcSort
		{
			name,
			physique,
			strength,
			origin,
			conflict
		};

		NpcSort g_npcSort{ NpcSort::name };
		bool    g_npcSortAscending{ true };

		std::shared_ptr<const Snapshot> Current()
		{
			std::lock_guard lock(g_snapshotLock);
			return g_snapshot;
		}

		void Dispatch(Action a_action)
		{
			if (g_handler) {
				g_handler(std::move(a_action));
			}
		}

		std::string Lower(std::string a_text)
		{
			std::ranges::transform(a_text, a_text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_text;
		}

		int CompareText(const std::string& a_left, const std::string& a_right)
		{
			const auto left = Lower(a_left);
			const auto right = Lower(a_right);
			return left < right ? -1 : (right < left ? 1 : 0);
		}

		int CompareNpcRows(const NpcRow& a_left, const NpcRow& a_right)
		{
			int result = 0;
			switch (g_npcSort) {
			case NpcSort::name:
				result = CompareText(a_left.name, a_right.name);
				break;
			case NpcSort::physique:
				result = CompareText(a_left.physique, a_right.physique);
				break;
			case NpcSort::strength:
				if (a_left.automaticStrength != a_right.automaticStrength) {
					result = a_left.automaticStrength ? 1 : -1;
				} else if (!a_left.automaticStrength) {
					result = a_left.strength < a_right.strength ? -1 :
						(a_right.strength < a_left.strength ? 1 : 0);
				}
				break;
			case NpcSort::origin:
				result = CompareText(a_left.sourceName, a_right.sourceName);
				break;
			case NpcSort::conflict:
				result = a_left.sourceCount < a_right.sourceCount ? -1 :
					(a_right.sourceCount < a_left.sourceCount ? 1 : 0);
				break;
			}
			if (result == 0) {
				result = CompareText(a_left.name, a_right.name);
			}
			if (result == 0) {
				result = a_left.formID < a_right.formID ? -1 :
					(a_right.formID < a_left.formID ? 1 : 0);
			}
			return g_npcSortAscending ? result : -result;
		}

		void SortHeader(const char* a_label, NpcSort a_sort)
		{
			const auto label = g_npcSort == a_sort ?
				std::format("{} {}", a_label, g_npcSortAscending ? "^" : "v") :
				std::string(a_label);
			if (SMF3::UI::SmallButton(label.c_str())) {
				if (g_npcSort == a_sort) {
					g_npcSortAscending = !g_npcSortAscending;
				} else {
					g_npcSort = a_sort;
					g_npcSortAscending = true;
				}
			}
		}

		bool Matches(const NpcRow& a_row, const std::array<char, 128>& a_filter,
			const std::string& a_sourceFilterPath, bool a_conflictsOnly)
		{
			if (a_conflictsOnly && a_row.sourceCount < 2) {
				return false;
			}
			if (!a_sourceFilterPath.empty() &&
				!std::ranges::any_of(a_row.candidates, [&](const auto& a_candidate) {
					return a_candidate.sourcePath == a_sourceFilterPath;
				})) {
				return false;
			}
			const auto needle = Lower(a_filter.data());
			if (needle.empty()) {
				return true;
			}
			return Lower(a_row.name).contains(needle) || Lower(a_row.stableID).contains(needle) ||
				Lower(a_row.physique).contains(needle) || Lower(a_row.sourceName).contains(needle);
		}

		void SourceCombo(const Snapshot& a_snapshot, std::string& a_sourceFilterPath,
			const char* a_label)
		{
			std::string current = "All origins";
			if (!a_sourceFilterPath.empty()) {
				if (const auto found = std::ranges::find(a_snapshot.sources, a_sourceFilterPath,
					&SourceRow::path); found != a_snapshot.sources.end()) {
					current = std::format("{} (p{})", found->name, found->priority);
				} else {
					a_sourceFilterPath.clear();
				}
			}
			if (!SMF3::UI::BeginCombo(a_label, current.c_str())) {
				return;
			}
			if (SMF3::UI::Selectable("All origins", a_sourceFilterPath.empty())) {
				a_sourceFilterPath.clear();
			}
			for (const auto& source : a_snapshot.sources) {
				const auto label = std::format("{} (p{})##{}", source.name, source.priority, source.path);
				if (SMF3::UI::Selectable(label.c_str(), source.path == a_sourceFilterPath)) {
					a_sourceFilterPath = source.path;
				}
			}
			SMF3::UI::EndCombo();
		}

		void CopyText(std::array<char, 128>& a_target, const std::string& a_text)
		{
			a_target.fill('\0');
			const auto count = (std::min)(a_text.size(), a_target.size() - 1);
			std::memcpy(a_target.data(), a_text.data(), count);
		}

		void BeginCandidateEdit(const NpcRow& a_row, std::size_t a_index)
		{
			g_selectedForm = a_row.formID;
			g_selectedCandidate = a_index;
			const auto& candidate = a_row.candidates[a_index];
			CopyText(g_editPhysique, candidate.physique);
			g_editStrength = candidate.strength;
			g_editAuto = candidate.automaticStrength;
			g_confirmDelete = false;
			g_scrollToEditor = true;
		}

		void BeginFactionEdit(const FactionRow& a_row)
		{
			g_selectedFactionSourcePath = a_row.sourcePath;
			g_selectedFactionTargetKey = a_row.targetKey;
			CopyText(g_editFactionPhysique, a_row.physique);
			g_editFactionStrength = a_row.strength;
			g_editFactionPercent = a_row.percent;
			g_editFactionAuto = a_row.automaticStrength;
			g_editFactionRandomStrength = a_row.randomStrength;
			g_editFactionRandomPhysique = a_row.randomPhysique;
			g_confirmFactionDelete = false;
			g_scrollToFactionEditor = true;
		}

		void PhysiqueCombo(const Snapshot& a_snapshot, const char* a_label,
			std::array<char, 128>& a_buffer)
		{
			if (!SMF3::UI::BeginCombo(a_label, a_buffer.data())) {
				return;
			}
			for (const auto& physique : a_snapshot.physiqueNames) {
				if (SMF3::UI::Selectable(physique.c_str(), physique == a_buffer.data())) {
					CopyText(a_buffer, physique);
				}
			}
			SMF3::UI::EndCombo();
		}

		void RenderNpcEditor(const Snapshot& a_snapshot)
		{
			const auto rowIt = std::ranges::find(a_snapshot.npcs, g_selectedForm, &NpcRow::formID);
			if (rowIt == a_snapshot.npcs.end() || rowIt->candidates.empty()) {
				g_selectedForm = 0;
				return;
			}
			g_selectedCandidate = (std::min)(g_selectedCandidate, rowIt->candidates.size() - 1);
			const auto& candidate = rowIt->candidates[g_selectedCandidate];

			const auto editorTitle = std::format("Edit {}", rowIt->name);
			SMF3::UI::SeparatorText(editorTitle.c_str());
			if (g_scrollToEditor) {
				SMF3::UI::SetScrollHereY(0.0f);
				g_scrollToEditor = false;
			}
			SMF3::UI::TextDisabled("Mod / INI: %s", candidate.sourceName.c_str());

			PhysiqueCombo(a_snapshot, "Physique##edit", g_editPhysique);
			SMF3::UI::Checkbox("Use original record weight##edit", &g_editAuto);
			if (!g_editAuto) {
				SMF3::UI::InputFloat("Strength##edit", &g_editStrength);
				g_editStrength = std::clamp(g_editStrength, 0.0f, 100.0f);
			}
			if (candidate.kind == OriginKind::faction) {
				if (SuccessButton("Save NPC override")) {
					Dispatch({ ActionKind::addNpc, rowIt->formID, rowIt->stableID, {}, {},
						g_editPhysique.data(), g_editStrength, g_editAuto });
				}
			} else {
				if (SuccessButton("Save entry")) {
					Dispatch({ ActionKind::editNpc, rowIt->formID, rowIt->stableID,
						candidate.sourcePath, candidate.targetKey, g_editPhysique.data(),
						g_editStrength, g_editAuto });
				}
				if (!g_confirmDelete) {
					SMF3::UI::SameLine();
					if (DangerButton("Delete entry")) {
						g_confirmDelete = true;
					}
				} else {
					SMF3::UI::TextColored({ 0.95f, 0.45f, 0.3f, 1.0f },
						"Delete this NPC entry from %s?", candidate.sourceName.c_str());
					if (DangerButton("Confirm delete")) {
						Dispatch({ ActionKind::deleteNpc, rowIt->formID, rowIt->stableID,
							candidate.sourcePath, candidate.targetKey });
						g_confirmDelete = false;
						g_selectedForm = 0;
					}
					SMF3::UI::SameLine();
					if (NeutralButton("Cancel delete")) {
						g_confirmDelete = false;
					}
				}
			}
			SMF3::UI::SameLine();
			if (NeutralButton("Close editor")) {
				g_selectedForm = 0;
			}
		}

		void RefreshAddMatches(const Snapshot& a_snapshot)
		{
			const auto query = Lower(g_addTarget.data());
			if (g_addMatchRevision == a_snapshot.revision && query == g_addMatchQuery) {
				return;
			}
			g_addMatchRevision = a_snapshot.revision;
			g_addMatchQuery = query;
			g_addMatches.clear();
			if (query.empty()) {
				return;
			}

			for (std::size_t i = 0; i < a_snapshot.npcChoices.size() && g_addMatches.size() < 8; ++i) {
				const auto& choice = a_snapshot.npcChoices[i];
				if (Lower(choice.name).contains(query) || Lower(choice.stableID).contains(query)) {
					g_addMatches.push_back(i);
				}
			}
		}

		void RenderAddNpc(const Snapshot& a_snapshot)
		{
			SMF3::UI::SeparatorText("Add NPC to MPF_User.ini");
			if (!g_addPhysique[0] && !a_snapshot.physiqueNames.empty()) {
				CopyText(g_addPhysique, a_snapshot.physiqueNames.front());
			}
			if (SMF3::UI::InputTextWithHint("NPC##add", "Start typing an NPC name or stable FormID",
				g_addTarget.data(), g_addTarget.size())) {
				// A clicked suggestion stores its FormID.  Any subsequent edit means
				// that the old selection no longer describes the input field.
				g_addFormID = 0;
			}
			RefreshAddMatches(a_snapshot);
			bool selectedMatch = false;
			for (const auto index : g_addMatches) {
				const auto& choice = a_snapshot.npcChoices[index];
				const auto existing = std::ranges::find(a_snapshot.npcs, choice.formID, &NpcRow::formID);
				std::string state;
				std::size_t candidateIndex = 0;
				if (existing != a_snapshot.npcs.end() && !existing->candidates.empty()) {
					const auto winner = std::ranges::find(existing->candidates, true, &Candidate::winner);
					candidateIndex = winner == existing->candidates.end() ? 0 :
						static_cast<std::size_t>(std::distance(existing->candidates.begin(), winner));
					state = existing->candidates[candidateIndex].kind == OriginKind::faction ?
						"  [via faction]" : "  [configured NPC]";
				}
				const auto label = std::format("{}  ({}){}##add-match-{:08X}",
					choice.name, choice.stableID, state, choice.formID);
				if (SMF3::UI::Selectable(label.c_str())) {
					if (existing != a_snapshot.npcs.end() && !existing->candidates.empty()) {
						BeginCandidateEdit(*existing, candidateIndex);
						g_showAddNpc = false;
						g_addTarget.fill('\0');
						g_addFormID = 0;
					} else {
						CopyText(g_addTarget, choice.stableID);
						g_addFormID = choice.formID;
					}
					selectedMatch = true;
					break;
				}
			}
			if (selectedMatch) {
				g_addMatchQuery = Lower(g_addTarget.data());
				g_addMatches.clear();
			}
			PhysiqueCombo(a_snapshot, "Physique##add", g_addPhysique);
			SMF3::UI::Checkbox("Use original record weight##add", &g_addAuto);
			if (!g_addAuto) {
				SMF3::UI::InputFloat("Strength##add", &g_addStrength);
				g_addStrength = std::clamp(g_addStrength, 0.0f, 100.0f);
			}
			if (SuccessButton("Add and refresh")) {
				// Also accept an exact display-name match when the user types it and
				// presses the button without clicking the suggestion.  The old code
				// passed that display name to ResolveTarget(), which intentionally only
				// understands EditorIDs and stable FormIDs, so the action silently failed.
				std::uint32_t targetForm = g_addFormID;
				if (!targetForm && g_addTarget[0]) {
					const auto query = Lower(g_addTarget.data());
					for (const auto& choice : a_snapshot.npcChoices) {
						if (Lower(choice.name) == query || Lower(choice.stableID) == query) {
							targetForm = choice.formID;
							break;
						}
					}
				}
				if ((targetForm || g_addTarget[0]) && g_addPhysique[0]) {
					Dispatch({ ActionKind::addNpc, targetForm,
						targetForm ? std::string{} : std::string(g_addTarget.data()), {}, {},
						g_addPhysique.data(), g_addStrength, g_addAuto });
				} else {
					SMF3::UI::TextColored({ 1.0f, 0.75f, 0.25f, 1.0f },
						"Choose an NPC and a physique before adding.");
				}
			}
		}

		enum class NpcListMode
		{
			named,
			factionAffected
		};

		bool BelongsToList(const NpcRow& a_row, NpcListMode a_mode)
		{
			if (a_mode == NpcListMode::named) {
				return std::ranges::any_of(a_row.candidates, [](const auto& candidate) {
					return candidate.kind == OriginKind::direct;
				});
			}
			const auto winner = std::ranges::find(a_row.candidates, true, &Candidate::winner);
			return winner != a_row.candidates.end() && winner->kind == OriginKind::faction;
		}

		void RenderNpcList(const Snapshot& a_snapshot, NpcListMode a_mode,
			std::array<char, 128>& a_filter, std::string& a_sourceFilterPath,
			bool& a_conflictsOnly, const char* a_id)
		{
			const auto searchLabel = std::format("Search##{}-search", a_id);
			const auto sourceLabel = std::format("Origin / mod##{}-source", a_id);
			const auto conflictLabel = std::format("Conflicts only##{}-conflicts", a_id);
			SMF3::UI::InputTextWithHint(searchLabel.c_str(), "NPC, FormID, physique, mod or INI",
				a_filter.data(), a_filter.size());
			SMF3::UI::SameLine();
			SourceCombo(a_snapshot, a_sourceFilterPath, sourceLabel.c_str());
			SMF3::UI::SameLine();
			SMF3::UI::Checkbox(conflictLabel.c_str(), &a_conflictsOnly);
			SMF3::UI::TextDisabled(a_mode == NpcListMode::named ?
				"NPCs explicitly named in an INI [NPCs] section, including losing conflict entries." :
				"NPCs whose effective assignment comes from an INI [Factions] rule.");

			std::vector<const NpcRow*> rows;
			rows.reserve(a_snapshot.npcs.size());
			for (const auto& row : a_snapshot.npcs) {
				if (BelongsToList(row, a_mode) && Matches(row, a_filter, a_sourceFilterPath, a_conflictsOnly)) {
					rows.push_back(&row);
				}
			}
			std::stable_sort(rows.begin(), rows.end(), [](const auto* a_left, const auto* a_right) {
				return CompareNpcRows(*a_left, *a_right) < 0;
			});

			const auto tableID = std::format("{}-table", a_id);
			if (SMF3::UI::BeginTable(tableID.c_str(), 6)) {
				for (const char* column : { "NPC", "Physique", "Strength", "Origin", "Conflict", "Action" }) {
					SMF3::UI::TableSetupColumn(column);
				}
				SMF3::UI::TableNextRow();
				SMF3::UI::TableNextColumn(); SortHeader("NPC", NpcSort::name);
				SMF3::UI::TableNextColumn(); SortHeader("Physique", NpcSort::physique);
				SMF3::UI::TableNextColumn(); SortHeader("Strength", NpcSort::strength);
				SMF3::UI::TableNextColumn(); SortHeader("Origin", NpcSort::origin);
				SMF3::UI::TableNextColumn(); SortHeader("Conflict", NpcSort::conflict);
				SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted("Action");
				std::size_t rendered = 0;
				for (const auto* rowPointer : rows) {
					if (rendered >= 500) {
						break;
					}
					const auto& row = *rowPointer;
					++rendered;
					SMF3::UI::PushID(static_cast<int>(row.formID));
					SMF3::UI::TableNextRow();
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(row.name.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(row.physique.c_str());
					SMF3::UI::TableNextColumn();
					row.automaticStrength ? SMF3::UI::TextUnformatted("auto") : SMF3::UI::Text("%.0f", row.strength);
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(row.sourceName.c_str());
					SMF3::UI::TableNextColumn();
					if (row.sourceCount > 1) {
						SMF3::UI::TextColored({ 0.95f, 0.72f, 0.2f, 1.0f }, "%zu INIs", row.sourceCount);
					} else {
						SMF3::UI::TextDisabled("-");
					}
					SMF3::UI::TableNextColumn();
					if (EditButton("Edit")) {
						const auto winner = std::ranges::find(row.candidates, true, &Candidate::winner);
						BeginCandidateEdit(row, winner == row.candidates.end() ? 0 :
							static_cast<std::size_t>(std::distance(row.candidates.begin(), winner)));
					}
					SMF3::UI::PopID();
				}
				SMF3::UI::EndTable();
			}
			SMF3::UI::TextDisabled("The table displays at most 500 NPCs; use search or Origin / mod to narrow large lists.");
		}

		void RenderNpcs(const Snapshot& a_snapshot)
		{
			if (!a_snapshot.status.empty()) {
				const bool failed = a_snapshot.status.contains("Could") ||
					a_snapshot.status.contains("did not") ||
					a_snapshot.status.contains("Refused");
				SMF3::UI::TextColored(failed ? SMF3::Vec4{ 1.0f, 0.45f, 0.35f, 1.0f } :
					SMF3::Vec4{ 0.45f, 0.88f, 0.58f, 1.0f }, "Status: %s", a_snapshot.status.c_str());
			}
			const bool toggleAddNpc = g_showAddNpc ? NeutralButton("Hide Add NPC") :
				PrimaryButton("Add NPC");
			if (toggleAddNpc) {
				g_showAddNpc = !g_showAddNpc;
			}
			if (g_showAddNpc) {
				RenderAddNpc(a_snapshot);
				SMF3::UI::Separator();
			}
			RenderNpcEditor(a_snapshot);
			RenderNpcList(a_snapshot, NpcListMode::named, g_filter, g_sourceFilterPath,
				g_conflictsOnly, "named-npcs");
		}

		void RenderFactionNpcs(const Snapshot& a_snapshot)
		{
			RenderNpcEditor(a_snapshot);
			RenderNpcList(a_snapshot, NpcListMode::factionAffected, g_factionNpcFilter,
				g_factionNpcSourceFilterPath, g_factionNpcConflictsOnly, "faction-npcs");
		}

		void RefreshAddFactionMatches(const Snapshot& a_snapshot)
		{
			const auto query = Lower(g_addFactionTarget.data());
			if (g_addFactionMatchRevision == a_snapshot.revision && query == g_addFactionMatchQuery) {
				return;
			}
			g_addFactionMatchRevision = a_snapshot.revision;
			g_addFactionMatchQuery = query;
			g_addFactionMatches.clear();
			if (query.empty()) {
				return;
			}
			for (std::size_t i = 0; i < a_snapshot.factionChoices.size() && g_addFactionMatches.size() < 8; ++i) {
				const auto& choice = a_snapshot.factionChoices[i];
				if (Lower(choice.name).contains(query) || Lower(choice.stableID).contains(query)) {
					g_addFactionMatches.push_back(i);
				}
			}
		}

		void RenderAddFaction(const Snapshot& a_snapshot)
		{
			SMF3::UI::SeparatorText("Add Faction to MPF_User.ini");
			SMF3::UI::InputTextWithHint("Faction##add", "Start typing a faction name or stable FormID",
				g_addFactionTarget.data(), g_addFactionTarget.size());
			RefreshAddFactionMatches(a_snapshot);
			bool selectedMatch = false;
			for (const auto index : g_addFactionMatches) {
				const auto& choice = a_snapshot.factionChoices[index];
				const auto existing = std::ranges::find(a_snapshot.factions, choice.formID, &FactionRow::formID);
				const auto label = std::format("{}  ({}){}##add-faction-{:08X}",
					choice.name, choice.stableID,
					existing == a_snapshot.factions.end() ? "" : "  [configured]", choice.formID);
				if (SMF3::UI::Selectable(label.c_str())) {
					if (existing != a_snapshot.factions.end()) {
						BeginFactionEdit(*existing);
						g_showAddFaction = false;
						g_addFactionTarget.fill('\0');
					} else {
						CopyText(g_addFactionTarget, choice.stableID);
					}
					selectedMatch = true;
					break;
				}
			}
			if (selectedMatch) {
				g_addFactionMatchQuery = Lower(g_addFactionTarget.data());
				g_addFactionMatches.clear();
			}
			SMF3::UI::Checkbox("Random physique##add-faction", &g_addFactionRandomPhysique);
			if (g_addFactionRandomPhysique) {
				SMF3::UI::TextDisabled("Chooses one loaded physique deterministically for each NPC.");
			} else {
				PhysiqueCombo(a_snapshot, "Physique##add-faction", g_addFactionPhysique);
			}
			if (SMF3::UI::Checkbox("Random strength##add-faction", &g_addFactionRandomStrength) &&
				g_addFactionRandomStrength) {
				g_addFactionAuto = false;
			}
			if (SMF3::UI::Checkbox("Use original record weight##add-faction", &g_addFactionAuto) &&
				g_addFactionAuto) {
				g_addFactionRandomStrength = false;
			}
			if (!g_addFactionRandomStrength && !g_addFactionAuto) {
				SMF3::UI::InputFloat("Strength##add-faction", &g_addFactionStrength);
				g_addFactionStrength = std::clamp(g_addFactionStrength, 0.0f, 100.0f);
			} else if (g_addFactionRandomStrength) {
				SMF3::UI::TextDisabled("Chooses a deterministic strength from 0 to 100 for each NPC.");
			}
			SMF3::UI::InputInt("Coverage %##add-faction", &g_addFactionPercent);
			g_addFactionPercent = std::clamp(g_addFactionPercent, 0, 100);
			if (SuccessButton("Add faction and refresh") &&
				g_addFactionTarget[0] && (g_addFactionRandomPhysique || g_addFactionPhysique[0])) {
				Dispatch({ ActionKind::addFaction, 0, g_addFactionTarget.data(), {}, {},
					g_addFactionPhysique.data(), g_addFactionStrength, g_addFactionAuto,
					g_addFactionPercent, g_addFactionRandomStrength, g_addFactionRandomPhysique });
			}
		}

		void RenderFactionEditor(const Snapshot& a_snapshot)
		{
			if (g_selectedFactionSourcePath.empty() || g_selectedFactionTargetKey.empty()) {
				return;
			}
			const auto row = std::ranges::find_if(a_snapshot.factions, [](const auto& candidate) {
				return candidate.sourcePath == g_selectedFactionSourcePath &&
					candidate.targetKey == g_selectedFactionTargetKey;
			});
			if (row == a_snapshot.factions.end()) {
				g_selectedFactionSourcePath.clear();
				g_selectedFactionTargetKey.clear();
				return;
			}

			SMF3::UI::SeparatorText("Edit Faction");
			if (g_scrollToFactionEditor) {
				SMF3::UI::SetScrollHereY(0.0f);
				g_scrollToFactionEditor = false;
			}
			SMF3::UI::Text("%s  (%s)", row->name.c_str(), row->stableID.c_str());
			SMF3::UI::Text("Mod / INI: %s", row->sourceName.c_str());
			SMF3::UI::TextDisabled("%s", row->sourcePath.c_str());
			SMF3::UI::Checkbox("Random physique##edit-faction", &g_editFactionRandomPhysique);
			if (g_editFactionRandomPhysique) {
				SMF3::UI::TextDisabled("Chooses one loaded physique deterministically for each NPC.");
			} else {
				PhysiqueCombo(a_snapshot, "Physique##edit-faction", g_editFactionPhysique);
			}
			if (SMF3::UI::Checkbox("Random strength##edit-faction", &g_editFactionRandomStrength) &&
				g_editFactionRandomStrength) {
				g_editFactionAuto = false;
			}
			if (SMF3::UI::Checkbox("Use original record weight##edit-faction", &g_editFactionAuto) &&
				g_editFactionAuto) {
				g_editFactionRandomStrength = false;
			}
			if (!g_editFactionRandomStrength && !g_editFactionAuto) {
				SMF3::UI::InputFloat("Strength##edit-faction", &g_editFactionStrength);
				g_editFactionStrength = std::clamp(g_editFactionStrength, 0.0f, 100.0f);
			} else if (g_editFactionRandomStrength) {
				SMF3::UI::TextDisabled("Chooses a deterministic strength from 0 to 100 for each NPC.");
			}
			SMF3::UI::InputInt("Coverage %##edit-faction", &g_editFactionPercent);
			g_editFactionPercent = std::clamp(g_editFactionPercent, 0, 100);
			if (SuccessButton("Save faction entry")) {
				Dispatch({ ActionKind::editFaction, row->formID, row->stableID, row->sourcePath,
					row->targetKey, g_editFactionPhysique.data(), g_editFactionStrength,
					g_editFactionAuto, g_editFactionPercent, g_editFactionRandomStrength,
					g_editFactionRandomPhysique });
			}
			if (!g_confirmFactionDelete) {
				SMF3::UI::SameLine();
				if (DangerButton("Delete faction entry")) {
					g_confirmFactionDelete = true;
				}
			} else {
				SMF3::UI::TextColored({ 0.95f, 0.45f, 0.3f, 1.0f },
					"Delete this faction rule from %s?", row->sourceName.c_str());
				if (DangerButton("Confirm faction delete")) {
					Dispatch({ ActionKind::deleteFaction, row->formID, row->stableID, row->sourcePath,
						row->targetKey });
					g_selectedFactionSourcePath.clear();
					g_selectedFactionTargetKey.clear();
					g_confirmFactionDelete = false;
				}
				SMF3::UI::SameLine();
				if (NeutralButton("Cancel faction delete")) {
					g_confirmFactionDelete = false;
				}
			}
			SMF3::UI::SameLine();
			if (NeutralButton("Close faction editor")) {
				g_selectedFactionSourcePath.clear();
				g_selectedFactionTargetKey.clear();
			}
		}

		void RenderFactions(const Snapshot& a_snapshot)
		{
			if (!a_snapshot.status.empty()) {
				const bool failed = a_snapshot.status.contains("Could") ||
					a_snapshot.status.contains("did not") ||
					a_snapshot.status.contains("Refused");
				SMF3::UI::TextColored(failed ? SMF3::Vec4{ 1.0f, 0.45f, 0.35f, 1.0f } :
					SMF3::Vec4{ 0.45f, 0.88f, 0.58f, 1.0f }, "Status: %s", a_snapshot.status.c_str());
			}
			const bool toggleAddFaction = g_showAddFaction ? NeutralButton("Hide Add Faction") :
				PrimaryButton("Add Faction");
			if (toggleAddFaction) {
				g_showAddFaction = !g_showAddFaction;
			}
			if (g_showAddFaction) {
				RenderAddFaction(a_snapshot);
				SMF3::UI::Separator();
			}
			RenderFactionEditor(a_snapshot);
			SMF3::UI::InputTextWithHint("Search##factions", "Faction, FormID, physique, mod or INI",
				g_factionFilter.data(), g_factionFilter.size());
			const auto needle = Lower(g_factionFilter.data());
			SMF3::UI::TextDisabled("Each row is one faction rule from one INI file; priority decides which rules are considered first.");
			if (SMF3::UI::BeginTable("faction-table", 7)) {
				for (const char* column : { "Faction", "Physique", "Strength", "Coverage", "Mod / INI", "Priority", "Action" }) {
					SMF3::UI::TableSetupColumn(column);
				}
				SMF3::UI::TableHeadersRow();
				for (std::size_t i = 0; i < a_snapshot.factions.size(); ++i) {
					const auto& row = a_snapshot.factions[i];
					if (!needle.empty() && !Lower(row.name).contains(needle) &&
						!Lower(row.stableID).contains(needle) && !Lower(row.targetKey).contains(needle) &&
						!Lower(row.physique).contains(needle) && !Lower(row.sourceName).contains(needle)) {
						continue;
					}
					SMF3::UI::PushID(static_cast<int>(i));
					SMF3::UI::TableNextRow();
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(row.name.c_str());
					SMF3::UI::TableNextColumn();
					SMF3::UI::TextUnformatted(row.randomPhysique ? "random" : row.physique.c_str());
					SMF3::UI::TableNextColumn();
					if (row.randomStrength) {
						SMF3::UI::TextUnformatted("random");
					} else if (row.automaticStrength) {
						SMF3::UI::TextUnformatted("auto");
					} else {
						SMF3::UI::Text("%.0f", row.strength);
					}
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%d%%", row.percent);
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(row.sourceName.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%d", row.priority);
					SMF3::UI::TableNextColumn();
					if (EditButton("Edit")) {
						BeginFactionEdit(row);
					}
					SMF3::UI::PopID();
				}
				SMF3::UI::EndTable();
			}
		}

		void RenderContent(const Snapshot& a_snapshot)
		{
			SMF3::UI::SeparatorText("INI files");
			if (SMF3::UI::BeginTable("source-table", 7)) {
				for (const char* column : { "Mod / INI", "Priority", "NPCs", "Factions", "Conflicts won", "Path", "Action" }) {
					SMF3::UI::TableSetupColumn(column);
				}
				SMF3::UI::TableHeadersRow();
				for (std::size_t i = 0; i < a_snapshot.sources.size(); ++i) {
					const auto& source = a_snapshot.sources[i];
					SMF3::UI::PushID(static_cast<int>(i));
					SMF3::UI::TableNextRow();
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(source.name.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%d", source.priority);
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%zu", source.directEntries);
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%zu", source.factionRules);
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%zu", source.conflictsWon);
					SMF3::UI::TableNextColumn(); SMF3::UI::TextDisabled("%s", source.path.c_str());
					SMF3::UI::TableNextColumn();
					if (EditButton("Edit priority")) {
						g_selectedSourcePath = source.path;
						g_selectedSourceName = source.name;
						g_selectedSourcePriority = source.priority;
					}
					SMF3::UI::PopID();
				}
				SMF3::UI::EndTable();
			}
			SMF3::UI::TextColored({ 0.95f, 0.72f, 0.2f, 1.0f },
				"Priority belongs to the whole INI file. Changing it can alter multiple winners.");
			if (!g_selectedSourcePath.empty()) {
				SMF3::UI::SeparatorText("Edit INI priority");
				SMF3::UI::Text("Mod / INI: %s", g_selectedSourceName.c_str());
				SMF3::UI::InputInt("Priority##source", &g_selectedSourcePriority);
				if (SuccessButton("Save priority")) {
					Dispatch({ ActionKind::setSourcePriority, 0, {}, g_selectedSourcePath, {}, {},
						0.0f, false, g_selectedSourcePriority });
					g_selectedSourcePath.clear();
					g_selectedSourceName.clear();
				}
				SMF3::UI::SameLine();
				if (NeutralButton("Cancel##priority")) {
					g_selectedSourcePath.clear();
					g_selectedSourceName.clear();
				}
			}

			SMF3::UI::SeparatorText("Effective physiques");
			if (SMF3::UI::BeginTable("physique-table", 5)) {
				for (const char* column : { "Name", "Face morph", "Body morph", "Mod / INI", "Priority" }) {
					SMF3::UI::TableSetupColumn(column);
				}
				SMF3::UI::TableHeadersRow();
				for (const auto& physique : a_snapshot.physiques) {
					SMF3::UI::TableNextRow();
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(physique.name.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(physique.faceMorph.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(physique.bodyMorph.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::TextUnformatted(physique.sourceName.c_str());
					SMF3::UI::TableNextColumn(); SMF3::UI::Text("%d", physique.priority);
				}
				SMF3::UI::EndTable();
			}
		}

		void RenderSettings(const Snapshot& a_snapshot)
		{
			SMF3::UI::SeparatorText("Runtime status");
			SMF3::UI::Text("Build: %s", a_snapshot.buildID.c_str());
			SMF3::UI::Text("Body backend: %s", a_snapshot.backend.c_str());
			SMF3::UI::Text("RaceMenu/SKEE: %s", a_snapshot.skeeAvailable ? "available" : "missing");
			SMF3::UI::Text("Menu Framework: %.2f", a_snapshot.menuApiVersion);
			SMF3::UI::Text("NPCs: %zu · INI files: %zu · Conflicts: %zu", a_snapshot.npcs.size(),
				a_snapshot.sources.size(), a_snapshot.conflictCount);
			if (!a_snapshot.status.empty()) {
				SMF3::UI::TextColored({ 0.45f, 0.85f, 0.55f, 1.0f }, "%s", a_snapshot.status.c_str());
			}

			SMF3::UI::SeparatorText("Logging");
			for (int level = 0; level < 3; ++level) {
				const char* labels[] = { "Normal", "Verbose", "Diagnostic" };
				if (SMF3::UI::Selectable(labels[level], a_snapshot.loggingLevel == level)) {
					Dispatch({ ActionKind::setLogging, 0, {}, {}, {}, {}, 0.0f, false, level });
				}
			}

			SMF3::UI::SeparatorText("Refresh");
			if (SuccessButton("Reload INIs and refresh loaded actors")) {
				Dispatch({ ActionKind::reloadAndRefresh });
			}
			if (SuccessButton("Refresh nearest actor")) {
				Dispatch({ ActionKind::refreshNearest });
			}
			SMF3::UI::SameLine();
			if (SuccessButton("Refresh all loaded actors")) {
				Dispatch({ ActionKind::refreshAll });
			}
			SMF3::UI::TextDisabled("Refresh rebuilds actor 3D so an old face delta cannot remain stacked.");
		}

		void RenderDistribution(const Snapshot& a_snapshot)
		{
			if (!SMF3::UI::BeginTabBar("mpf-distribution-tabs")) {
				return;
			}
			if (SMF3::UI::BeginTabItem("NPCs")) {
				if (SMF3::UI::BeginTabBar("mpf-npc-distribution-tabs")) {
					if (SMF3::UI::BeginTabItem("Named NPCs")) {
						RenderNpcs(a_snapshot);
						SMF3::UI::EndTabItem();
					}
					if (SMF3::UI::BeginTabItem("Faction NPCs")) {
						RenderFactionNpcs(a_snapshot);
						SMF3::UI::EndTabItem();
					}
					SMF3::UI::EndTabBar();
				}
				SMF3::UI::EndTabItem();
			}
			if (SMF3::UI::BeginTabItem("Factions")) {
				RenderFactions(a_snapshot);
				SMF3::UI::EndTabItem();
			}
			SMF3::UI::EndTabBar();
		}

		void __stdcall Render()
		{
			const auto snapshot = Current();
			if (!snapshot) {
				SMF3::UI::TextUnformatted("MPF is still loading.");
				return;
			}
			if (!SMF3::UI::BeginTabBar("mpf-tabs")) {
				return;
			}
			if (SMF3::UI::BeginTabItem("Distribution")) {
				RenderDistribution(*snapshot);
				SMF3::UI::EndTabItem();
			}
			if (SMF3::UI::BeginTabItem("Content")) {
				RenderContent(*snapshot);
				SMF3::UI::EndTabItem();
			}
			if (SMF3::UI::BeginTabItem("Settings / Diagnostics")) {
				RenderSettings(*snapshot);
				SMF3::UI::EndTabItem();
			}
			SMF3::UI::EndTabBar();
		}
	}

	void SetActionHandler(ActionHandler a_handler)
	{
		g_handler = a_handler;
	}

	void Publish(std::shared_ptr<const Snapshot> a_snapshot)
	{
		std::lock_guard lock(g_snapshotLock);
		g_snapshot = std::move(a_snapshot);
	}

	bool Register()
	{
		if (g_registered.load(std::memory_order_acquire) || !SMF3::IsAvailable()) {
			return g_registered.load(std::memory_order_relaxed);
		}
		if (!SMF3::AddPage("Modern Physique Framework/Configuration", Render)) {
			return false;
		}
		g_registered.store(true, std::memory_order_release);
		return true;
	}

	float FrameworkApiVersion()
	{
		return SMF3::ApiVersion();
	}
}
